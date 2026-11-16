// SPDX-License-Identifier: GPL-3.0-or-later
//
// --log-relay TAG: stdin to syslog, one datagram per line, ident TAG, through the same sink
// (never blocks: a full or missing syslog drops lines and counts them); lines are cut at 900
// bytes. For the receiver's and the bridge's output, which the launcher sends here through a
// FIFO instead of to the serial console. Exits 0 at the end of the input.

use crate::log;

pub fn run(tag: &str) -> i32 {
    unsafe { libc::signal(libc::SIGPIPE, libc::SIG_IGN) };
    log::set_stderr(false);
    log::set_ident(tag);
    log::set_with_pid(false);
    let mut buf: Vec<u8> = Vec::new();
    let mut tmp = [0u8; 4096];
    loop {
        let n = unsafe { libc::read(0, tmp.as_mut_ptr() as *mut libc::c_void, tmp.len()) };
        if n < 0 && std::io::Error::last_os_error().raw_os_error() == Some(libc::EINTR) {
            continue;
        }
        if n <= 0 {
            break;
        }
        buf.extend_from_slice(&tmp[..n as usize]);
        loop {
            let pos = buf.iter().position(|&c| c == b'\n');
            if pos.is_none() && buf.len() < 900 {
                break;
            }
            let cut = match pos {
                Some(p) if p < 900 => p,
                _ => 900,
            };
            let mut line = buf[..cut].to_vec();
            let skip = if cut < buf.len() && buf[cut] == b'\n' { cut + 1 } else { cut };
            buf.drain(..skip);
            if line.last() == Some(&b'\r') {
                line.pop();
            }
            if !line.is_empty() {
                log::line(log::INFO, &c_text(&line));
            }
        }
    }
    if !buf.is_empty() {
        log::line(log::INFO, &c_text(&buf));
    }
    let lost = log::dropped();
    if lost > 0 {
        log::line(log::WARNING, &format!("log relay: {lost} lines dropped (syslog busy or down)"));
    }
    0
}

// The line as the C++ relay passed it on: a C string, so up to its first NUL.
fn c_text(b: &[u8]) -> String {
    let b = &b[..b.iter().position(|&c| c == 0).unwrap_or(b.len())];
    String::from_utf8_lossy(b).into_owned()
}
