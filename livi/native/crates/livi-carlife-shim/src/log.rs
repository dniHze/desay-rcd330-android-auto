// SPDX-License-Identifier: GPL-3.0-or-later
//
// Logging. Every thread logs, the RCF dispatch threads and sourcemgr's synchronous callback
// among them, so a line must never wait on its reader. On the unit's syslog (busybox syslogd,
// /dev/log) the line is one datagram, sent without blocking: a full socket gets 5 ms once,
// then the line is dropped and counted, and the count is logged once lines get through again;
// a syslogd that is not there (yet), or restarted, is reconnected at most once a second.
// --debug-output console writes to stderr instead (the SD-card flow's shim.log). Each line
// carries the monotonic time, as the receiver's and bridge's own stamps do.

use std::io::{self, Write};
use std::os::fd::AsRawFd;
use std::os::unix::net::UnixDatagram;
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};
use std::sync::Mutex;
use std::time::{Duration, Instant};

pub const TAG: &str = "aa-carlife-shim";

const USER: i32 = 1 << 3; // LOG_USER, as the unit's own services log
pub const ERR: i32 = 3;
pub const WARNING: i32 = 4;
pub const INFO: i32 = 6;
pub const DEBUG: i32 = 7;

struct Sink {
    sock: Option<UnixDatagram>,
    dropped: u32,
    dropping: bool, // the last line was dropped: do not wait again
    next_connect: Option<Instant>,
}

static SINK: Mutex<Sink> = Mutex::new(Sink { sock: None, dropped: 0, dropping: false, next_connect: None });
/// The same socket, for the crash handler (no locks there).
pub static CRASH_FD: AtomicI32 = AtomicI32::new(-1);
static STDERR: AtomicBool = AtomicBool::new(true); // until main knows --debug-output
static WITH_PID: AtomicBool = AtomicBool::new(true);
static IDENT: Mutex<String> = Mutex::new(String::new()); // "" is TAG
static PATH: Mutex<String> = Mutex::new(String::new()); // "" is /dev/log
static VERBOSE: AtomicBool = AtomicBool::new(false);

pub fn set_stderr(on: bool) {
    STDERR.store(on, Ordering::SeqCst);
}

pub fn to_stderr() -> bool {
    STDERR.load(Ordering::SeqCst)
}

pub fn set_ident(ident: &str) {
    *IDENT.lock().unwrap() = ident.to_string();
}

pub fn set_with_pid(on: bool) {
    WITH_PID.store(on, Ordering::SeqCst);
}

pub fn set_path(path: &str) {
    *PATH.lock().unwrap() = path.to_string();
}

pub fn path() -> String {
    let p = PATH.lock().unwrap();
    if p.is_empty() { "/dev/log".to_string() } else { p.clone() }
}

fn ident() -> String {
    let i = IDENT.lock().unwrap();
    if i.is_empty() { TAG.to_string() } else { i.clone() }
}

pub fn verbose() -> bool {
    VERBOSE.load(Ordering::Relaxed)
}

pub fn set_verbose(on: bool) {
    VERBOSE.store(on, Ordering::Relaxed);
}

/// The monotonic clock: seconds and milliseconds.
pub fn stamp() -> (i64, i64) {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
    #[allow(clippy::unnecessary_cast)] // time_t and long are 32 bits on the unit
    (ts.tv_sec as i64, ts.tv_nsec as i64 / 1_000_000)
}

fn level_name(level: i32) -> &'static str {
    if level <= ERR {
        "ERROR"
    } else if level == WARNING {
        "WARN"
    } else if level == DEBUG {
        "DEBUG"
    } else {
        "INFO"
    }
}

// SINK held.
fn connect_locked(sink: &mut Sink) {
    let now = Instant::now();
    if sink.next_connect.is_some_and(|t| now < t) {
        return;
    }
    sink.next_connect = Some(now + Duration::from_secs(1));
    let Ok(s) = UnixDatagram::unbound() else { return };
    if s.set_nonblocking(true).is_err() || s.connect(path()).is_err() {
        return;
    }
    CRASH_FD.store(s.as_raw_fd(), Ordering::SeqCst);
    sink.sock = Some(s);
}

// SINK held. One datagram; false when it was dropped.
fn send_locked(sink: &mut Sink, data: &[u8]) -> bool {
    if sink.sock.is_none() {
        connect_locked(sink);
    }
    let Some(sock) = &sink.sock else { return false };
    let mut attempt = 0;
    while attempt < 2 {
        match sock.send(data) {
            Ok(_) => {
                sink.dropping = false;
                return true;
            }
            Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
            Err(e) if e.kind() == io::ErrorKind::WouldBlock || e.raw_os_error() == Some(libc::ENOBUFS) => {
                if sink.dropping || attempt > 0 {
                    break;
                }
                let mut p = libc::pollfd { fd: sock.as_raw_fd(), events: libc::POLLOUT, revents: 0 };
                if unsafe { libc::poll(&mut p, 1, 5) } <= 0 {
                    break;
                }
            }
            Err(_) => {
                // ECONNREFUSED / ENOTCONN / ENOENT: syslogd went away or was restarted.
                CRASH_FD.store(-1, Ordering::SeqCst);
                sink.sock = None;
                break;
            }
        }
        attempt += 1;
    }
    sink.dropping = true;
    false
}

// The first `max` bytes of `s` at most (a C buffer's cut), on a character boundary.
pub fn cut(s: &str, max: usize) -> &str {
    if s.len() <= max {
        return s;
    }
    let mut n = max;
    while !s.is_char_boundary(n) {
        n -= 1;
    }
    &s[..n]
}

// "<pri>ident[pid]: [stamp] LEVEL: text", cut to `size` - 1 bytes.
fn format(size: usize, level: i32, s: i64, ms: i64, text: &str) -> String {
    let line = if WITH_PID.load(Ordering::SeqCst) {
        format!("<{}>{}[{}]: [{}.{:03}] {}: {}", USER | level, ident(), std::process::id(), s, ms, level_name(level), text)
    } else {
        format!("<{}>{}: [{}.{:03}] {}: {}", USER | level, ident(), s, ms, level_name(level), text)
    };
    cut(&line, size - 1).to_string()
}

/// One line to syslog with this ident, or to stderr.
pub fn line(level: i32, text: &str) {
    let (s, ms) = stamp();
    if to_stderr() {
        let out = format!("[{} {}.{:03}] {}: {}\n", ident(), s, ms, level_name(level), text);
        let _ = io::stderr().write_all(out.as_bytes());
        return;
    }
    let buf = format(1024, level, s, ms, text);
    let mut sink = SINK.lock().unwrap_or_else(|e| e.into_inner());
    if !send_locked(&mut sink, buf.as_bytes()) {
        sink.dropped += 1;
        return;
    }
    if sink.dropped > 0 {
        let what = format!("{} log lines dropped (syslog busy or down)", sink.dropped);
        let note = format(160, WARNING, s, ms, cut(&what, 63));
        if send_locked(&mut sink, note.as_bytes()) {
            sink.dropped = 0;
        }
    }
}

/// The drops not yet reported (the log relay's exit).
pub fn dropped() -> u32 {
    SINK.lock().unwrap_or_else(|e| e.into_inner()).dropped
}

/// A formatted line, cut as the C++ shim's 512-byte buffer cut it.
pub fn logf(level: i32, msg: String) {
    line(level, cut(&msg, 511));
}

#[macro_export]
macro_rules! logi {
    ($($a:tt)*) => { $crate::log::logf($crate::log::INFO, format!($($a)*)) };
}
#[macro_export]
macro_rules! logw {
    ($($a:tt)*) => { $crate::log::logf($crate::log::WARNING, format!($($a)*)) };
}
#[macro_export]
macro_rules! loge {
    ($($a:tt)*) => { $crate::log::logf($crate::log::ERR, format!($($a)*)) };
}
#[macro_export]
macro_rules! logv {
    ($($a:tt)*) => { if $crate::log::verbose() { $crate::log::logf($crate::log::DEBUG, format!($($a)*)) } };
}

/// A string from another device (a Bluetooth name) for a log line: control characters out.
pub fn printable(s: &[u8]) -> String {
    let b: Vec<u8> = s.iter().take(64).map(|&c| if c < 0x20 || c == 0x7f { b'?' } else { c }).collect();
    String::from_utf8_lossy(&b).into_owned()
}
