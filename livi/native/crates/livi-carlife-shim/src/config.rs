// SPDX-License-Identifier: GPL-3.0-or-later
//
// The shim's settings (environment and arguments), and the small helpers every part uses:
// files replaced whole, directories made, whether a TCP port answers, errno texts.

use std::ffi::CStr;
use std::fs::{self, DirBuilder, OpenOptions};
use std::io::{self, Write};
use std::net::{Ipv4Addr, SocketAddr, TcpStream};
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
use std::time::Duration;

use crate::logw;
use crate::scan::strtol_whole;

pub const MAX_PORT: i32 = 65535;

pub struct Config {
    pub bind_host: String,
    pub svc_port: i32,
    pub pub_port: i32,
    pub registry_dir: String, // "-" disables
    pub ctl_sock: String,     // "-" disables
    pub fake: bool,
    pub fake_video_ms: i32, // fake mode: connecting (2) -> video (3) delay
    pub connect_timeout_ms: i32,
    pub receiver_wait_ms: i32,
    pub ilm_host: String,
    pub ilm_port: i32,          // 0 disables the ILM calls
    pub bridge_comm: String,    // comma-separated process names of the video bridge
    pub video_source: String,   // "x,y,w,h" of the bridge's surface to show; "-" leaves it
    pub video_dest: String,     // "x,y,w,h" on the screen; "-" leaves it
    pub source_host: String,
    pub source_port: i32,        // svp.svc.source; 0 disables the audio source claim
    pub source_listen_port: i32, // its status publisher (interruption ends); 0 disables
    pub can_host: String,
    pub can_port: i32,        // svp.svc.can; 0 disables day/night, gear, right-hand drive, speed
    pub can_listen_port: i32, // the CAN service's publisher (speed); 0 disables speed
    pub bt_host: String,
    pub bt_port: i32,          // svp.svc.bluetooth; 0 leaves the mic path to appconnect
    pub bt_listen_port: i32,   // its event publisher (pairing)
    pub bt_pairing: bool,      // the AA pairing loop
    pub bt_a2dp_drop: bool,    // Bluetooth music off while AA plays, back after
    pub bt_lock: bool,         // no other device on Bluetooth while AA's phone holds the phone link
    pub bt_lock_file: String,  // says the lock is ours, so a restart takes it off; "" never marks
    pub on_top_file: String,   // "<pid> <0|1>": AA on screen, for the Bluetooth app (no pop-ups)
    pub call_fix: bool,        // keep an outgoing call on AA's screen (NeedBlockBTPopup)
    pub appc_patch: bool,      // appconnect's code changes in memory (appc.rs)
    pub call_fix_ms: i32,
    pub audio_host: String,
    pub audio_port: i32,       // audiomgr; 0 disables RegisterAudio
    pub notify_sock: String,   // datagrams from the bridge ("open <stream>"); "-" disables
    pub power_host: String,
    pub power_port: i32,        // svp.svc.power: whether a switch-on is temporary
    pub power_listen_port: i32, // its publisher: the unit switched off / on; 0 disables
    pub last_mode_file: String, // whether AA had the speakers at the last switch-off; "" disables
    pub fatal_file: String,     // a start-up failure's reason, for the launcher (fatal_start)
    pub receiver_comm: String,  // the receiver's process names: the only ones the liveness check kills
    pub boots_file: String,     // the launcher's boot-loop counter: a clean switch-off clears it
}

/// The variable, or `dflt` when it is unset or empty.
pub fn env_or(name: &str, dflt: &str) -> String {
    match std::env::var_os(name) {
        Some(v) if !v.is_empty() => v.to_string_lossy().into_owned(),
        _ => dflt.to_string(),
    }
}

/// An integer setting within [lo, hi]; anything else is logged and gives the default.
pub fn env_int(name: &str, dflt: i32, lo: i32, hi: i32) -> i32 {
    let v = match std::env::var_os(name) {
        Some(v) if !v.is_empty() => v.to_string_lossy().into_owned(),
        _ => return dflt,
    };
    match strtol_whole(&v) {
        Some(n) if n >= lo as i64 && n <= hi as i64 => n as i32,
        _ => {
            logw!("{}={} is not a number in {}..{}; using {}", name, v, lo, hi, dflt);
            dflt
        }
    }
}

/// strerror() of an error: the C library's text for an errno, else the error's own.
pub fn errstr(e: &io::Error) -> String {
    match e.raw_os_error() {
        Some(code) => strerror(code),
        None => e.to_string(),
    }
}

pub fn strerror(code: i32) -> String {
    unsafe { CStr::from_ptr(libc::strerror(code)) }.to_string_lossy().into_owned()
}

/// strerror(errno) for the last failed call.
pub fn last_errstr() -> String {
    errstr(&io::Error::last_os_error())
}

/// Creates the directories of `path` that are missing, with `mode`.
pub fn mkdirs(path: &str, mode: u32) -> io::Result<()> {
    let b = path.as_bytes();
    let mut pos = 0usize;
    loop {
        let next = b.iter().skip(pos + 1).position(|&c| c == b'/').map(|i| i + pos + 1);
        let cur = match next {
            Some(p) => &path[..p],
            None => path,
        };
        if !cur.is_empty()
            && let Err(e) = DirBuilder::new().mode(mode).create(cur)
                && e.kind() != io::ErrorKind::AlreadyExists {
                    return Err(e);
                }
        match next {
            Some(p) => pos = p,
            None => return Ok(()),
        }
    }
}

/// The directory part of a file path ("" when there is none).
pub fn dir_name(path: &str) -> String {
    match path.rfind('/') {
        None => String::new(),
        Some(0) => "/".to_string(),
        Some(i) => path[..i].to_string(),
    }
}

/// Replaces `path` whole with `text`: a temporary file, fsync, rename, fsync of the directory, so
/// a power cut leaves the old file or the new one. Returns "" or what failed.
pub fn replace_file(path: &str, text: &str) -> String {
    let tmp = format!("{path}.tmp");
    let r = (|| -> Result<(), (&'static str, io::Error)> {
        let mut f = OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(0o600)
            .open(&tmp)
            .map_err(|e| ("open", e))?;
        let n = f.write(text.as_bytes()).map_err(|e| ("write", e))?;
        if n != text.len() {
            return Err(("write", io::Error::last_os_error()));
        }
        f.sync_all().map_err(|e| ("fsync", e))?;
        drop(f);
        fs::rename(&tmp, path).map_err(|e| ("rename", e))
    })();
    if let Err((what, e)) = r {
        return format!("{}: {}", what, errstr(&e));
    }
    // The rename itself is safe only once the directory is on the disk.
    let dir = dir_name(path);
    if let Ok(d) = fs::File::open(if dir.is_empty() { "." } else { dir.as_str() }) {
        let _ = d.sync_all();
    }
    String::new()
}

/// Whether something accepts TCP connections on host:port (within timeout_ms). Checked before
/// subscriptions, as the C++ shim did (RCF 2.1's createSubscription threw from a destructor
/// when no publisher was there, which ended the process); kept so the logs and timing match.
pub fn tcp_reachable(host: &str, port: i32, timeout_ms: u64) -> bool {
    let Ok(ip) = host.parse::<Ipv4Addr>() else { return false };
    if !(0..=MAX_PORT).contains(&port) {
        return false;
    }
    let a = SocketAddr::from((ip, port as u16));
    TcpStream::connect_timeout(&a, Duration::from_millis(timeout_ms.max(1))).is_ok()
}

pub fn status_name(st: i32) -> &'static str {
    match st {
        0 => "none",
        1 => "connect failed",
        2 => "connecting",
        3 => "video",
        4 => "disconnected",
        5 => "timeout",
        _ => "?",
    }
}

pub fn touch_name(ev: i32) -> Option<&'static str> {
    match ev {
        0 => Some("down"),
        1 => Some("up"),
        2 => Some("move"),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dir_names() {
        assert_eq!(dir_name("/tmp/run/aa/ontop"), "/tmp/run/aa");
        assert_eq!(dir_name("/ontop"), "/");
        assert_eq!(dir_name("ontop"), "");
    }

    #[test]
    fn files_are_replaced_whole() {
        let d = std::env::temp_dir().join(format!("shim-cfg-{}", std::process::id()));
        let p = d.join("a/b/f");
        mkdirs(&dir_name(p.to_str().unwrap()), 0o700).unwrap();
        assert_eq!(replace_file(p.to_str().unwrap(), "speakers aa\n"), "");
        assert_eq!(fs::read_to_string(&p).unwrap(), "speakers aa\n");
        assert!(replace_file(d.join("nope/f").to_str().unwrap(), "x").starts_with("open: "));
        let _ = fs::remove_dir_all(&d);
    }
}
