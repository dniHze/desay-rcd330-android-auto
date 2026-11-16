// SPDX-License-Identifier: GPL-3.0-or-later
//
// Control channel to the AA receiver: one Unix stream socket, one client at a time.
// Each side says "hello <protocol>" first. A receiver that says protocol 2 or later is pinged
// every 2 s ("ping <n>", answered "pong <n>"); after 3 pings without an answer it is taken as
// hung: killed (only a process whose name is in AA_RECEIVER_COMM, found by SO_PEERCRED;
// procguard restarts it) and the connection shut down, which reports "state gone" as any end
// of the connection does. hello / ping / pong stay in here; every other line goes to the LineFn.

use std::io::{Read, Write};
use std::net::Shutdown;
use std::os::fd::AsRawFd;
use std::os::unix::fs::PermissionsExt;
use std::os::unix::net::{UnixListener, UnixStream};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use crate::config::errstr;
use crate::scan::prefixed_uint;
use crate::{loge, logi, logw};

/// Gets every line the receiver sends, and "state gone" when it goes away.
pub type LineFn = Box<dyn Fn(&str) + Send + Sync>;

pub const PROTOCOL: i32 = 2;

struct Inner {
    client: Option<Arc<UnixStream>>,
    send_failed: bool, // this connection is being shut down (send)
    connections: u32,
}

pub struct ControlChannel {
    inner: Mutex<Inner>,
    cv: Condvar,
    stop: AtomicBool,
    path: Mutex<String>,
    listening: AtomicBool,
    thread: Mutex<Option<JoinHandle<()>>>,
}

// What one connection said: its protocol, the pings, a line being cut.
#[derive(Default)]
struct Peer {
    protocol: i32,  // from its hello; 0: an old receiver, never pinged
    pinged: u32,    // the last ping sent
    answered: u32,  // the last pong received
    next_ping: Option<Instant>,
    buf: Vec<u8>,
    discarding: bool, // the rest of an over-long line
}

impl ControlChannel {
    pub fn new() -> Arc<Self> {
        Arc::new(ControlChannel {
            inner: Mutex::new(Inner { client: None, send_failed: false, connections: 0 }),
            cv: Condvar::new(),
            stop: AtomicBool::new(false),
            path: Mutex::new(String::new()),
            listening: AtomicBool::new(false),
            thread: Mutex::new(None),
        })
    }

    /// `receiver_names`: the process names (comma-separated) the liveness check may kill.
    pub fn open(self: &Arc<Self>, path: &str, on_line: LineFn, receiver_names: &str) -> bool {
        *self.path.lock().unwrap() = path.to_string();
        let names = format!(",{receiver_names},");
        if path.len() >= 108 {
            loge!("control socket path too long: {}", path);
            return false;
        }
        let _ = std::fs::remove_file(path);
        let listener = match UnixListener::bind(path) {
            Ok(l) => l,
            Err(e) => {
                loge!("control socket {}: {}", path, errstr(&e));
                return false;
            }
        };
        // the receiver runs as root, as every service on the unit
        let _ = std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600));
        self.listening.store(true, Ordering::SeqCst);
        let me = self.clone();
        *self.thread.lock().unwrap() = Some(std::thread::spawn(move || me.run(listener, on_line, names)));
        logi!("control socket listening on {}", path);
        true
    }

    pub fn close(&self) {
        {
            let _g = self.inner.lock().unwrap();
            self.stop.store(true, Ordering::SeqCst);
        }
        self.cv.notify_all(); // a wait_connected() gives up
        if let Some(t) = self.thread.lock().unwrap().take() {
            let _ = t.join();
        }
        let mut g = self.inner.lock().unwrap();
        if let Some(c) = g.client.take() {
            let _ = c.shutdown(Shutdown::Both);
        }
        if self.listening.swap(false, Ordering::SeqCst) {
            let _ = std::fs::remove_file(&*self.path.lock().unwrap());
        }
    }

    #[allow(dead_code)] // as in the C++ shim: nothing asks yet
    pub fn connected(&self) -> bool {
        self.inner.lock().unwrap().client.is_some()
    }

    /// Receiver connections accepted so far: changes whenever the receiver (re)connects, for
    /// what it must hear once per connection.
    pub fn connections(&self) -> u32 {
        self.inner.lock().unwrap().connections
    }

    pub fn wait_connected(&self, timeout_ms: i32) -> bool {
        let g = self.inner.lock().unwrap();
        let (g, _) = self
            .cv
            .wait_timeout_while(g, Duration::from_millis(timeout_ms.max(0) as u64), |i| {
                i.client.is_none() && !self.stop.load(Ordering::SeqCst)
            })
            .unwrap();
        g.client.is_some()
    }

    /// Called from RCF dispatch threads and sourcemgr's synchronous callback, so it never waits
    /// long: a receiver that stops reading gets 1 s (SO_SNDTIMEO), then the connection is shut
    /// down. Only the reader thread closes the socket; it then reports "state gone".
    pub fn send(&self, line: &str) -> bool {
        let mut g = self.inner.lock().unwrap();
        let Some(c) = g.client.clone() else { return false };
        if g.send_failed {
            return false;
        }
        let out = format!("{line}\n");
        let mut p = out.as_bytes();
        while !p.is_empty() {
            match (&*c).write(p) {
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
                Ok(n) if n > 0 => p = &p[n..],
                r => {
                    let why = match r {
                        Err(e) => errstr(&e),
                        _ => "nothing written".to_string(),
                    };
                    logw!("receiver write failed ({}), dropping connection", why);
                    g.send_failed = true;
                    let _ = c.shutdown(Shutdown::Both);
                    return false;
                }
            }
        }
        true
    }

    // Reader thread: one received line.
    fn handle(&self, peer: &mut Peer, line: &str, on_line: &LineFn) {
        if let Some(n) = prefixed_uint(line, "hello") {
            peer.protocol = n as i32;
            logi!("receiver: protocol {}{}", n, if n >= 2 { " (pinged every 2 s)" } else { "" });
            if n >= 2 {
                peer.pinged = 0;
                peer.answered = 0;
                peer.next_ping = Some(Instant::now() + Duration::from_secs(2));
            }
        } else if let Some(n) = prefixed_uint(line, "pong") {
            if n <= peer.pinged && n > peer.answered {
                peer.answered = n;
            }
        } else {
            on_line(line);
        }
    }

    // Reader thread: received bytes, cut into lines. A line over 4096 bytes is dropped whole.
    fn feed(&self, peer: &mut Peer, data: &[u8], on_line: &LineFn) {
        peer.buf.extend_from_slice(data);
        while let Some(pos) = peer.buf.iter().position(|&c| c == b'\n') {
            let mut line: Vec<u8> = peer.buf.drain(..=pos).collect();
            line.pop();
            if peer.discarding {
                peer.discarding = false;
                continue;
            }
            if line.last() == Some(&b'\r') {
                line.pop();
            }
            if !line.is_empty() {
                let text = String::from_utf8_lossy(&line).into_owned();
                self.handle(peer, &text, on_line);
            }
        }
        if peer.buf.len() > 4096 {
            logw!("receiver: a line over 4096 bytes, dropped");
            peer.buf.clear();
            peer.discarding = true;
        }
    }

    // Reader thread: the next ping, or the end of a receiver that stopped answering.
    fn keep_alive(&self, peer: &mut Peer, client: &Arc<UnixStream>, names: &str) {
        if peer.protocol < 2 {
            return;
        }
        let now = Instant::now();
        if peer.next_ping.is_some_and(|t| now < t) {
            return;
        }
        peer.next_ping = Some(now + Duration::from_secs(2));
        if peer.pinged.wrapping_sub(peer.answered) < 3 {
            peer.pinged = peer.pinged.wrapping_add(1);
            self.send(&format!("ping {}", peer.pinged));
            return;
        }
        logw!("receiver: no answer to 3 pings, taken as hung");
        peer.protocol = 0;
        kill_peer(client, names);
        let mut g = self.inner.lock().unwrap();
        if g.client.as_ref().is_some_and(|c| Arc::ptr_eq(c, client)) && !g.send_failed {
            g.send_failed = true;
            let _ = client.shutdown(Shutdown::Both); // the reader sees the end: "state gone"
        }
    }

    fn run(&self, listener: UnixListener, on_line: LineFn, names: String) {
        let mut peer = Peer::default();
        while !self.stop.load(Ordering::SeqCst) {
            let client = self.inner.lock().unwrap().client.clone();
            let mut fds = [
                libc::pollfd { fd: listener.as_raw_fd(), events: libc::POLLIN, revents: 0 },
                libc::pollfd { fd: -1, events: libc::POLLIN, revents: 0 },
            ];
            let mut nfds = 1;
            if let Some(c) = &client {
                fds[1].fd = c.as_raw_fd();
                nfds = 2;
            }
            let r = unsafe { libc::poll(fds.as_mut_ptr(), nfds, 200) };
            if r < 0 {
                continue;
            }

            if fds[0].revents & libc::POLLIN != 0 {
                if let Ok((s, _)) = listener.accept() {
                    let _ = s.set_write_timeout(Some(Duration::from_secs(1)));
                    let mut replaced = false;
                    {
                        let mut g = self.inner.lock().unwrap();
                        if let Some(old) = g.client.take() {
                            logw!("receiver reconnected, replacing the previous connection");
                            let _ = old.shutdown(Shutdown::Both);
                            replaced = true;
                        }
                        g.client = Some(Arc::new(s));
                        g.send_failed = false;
                        g.connections = g.connections.wrapping_add(1);
                        logi!("receiver connected");
                    }
                    self.cv.notify_all();
                    peer = Peer::default();
                    // The connection it replaced ended without its end being read: whatever
                    // session ran on it is over (outside the lock, as below).
                    if replaced {
                        on_line("state gone");
                    }
                    self.send(&format!("hello {PROTOCOL}"));
                }
                continue;
            }
            if let Some(c) = &client {
                if fds[1].revents & (libc::POLLIN | libc::POLLHUP | libc::POLLERR) != 0 {
                    let mut tmp = [0u8; 512];
                    let n = (&**c).read(&mut tmp);
                    match n {
                        Ok(n) if n > 0 => self.feed(&mut peer, &tmp[..n], &on_line),
                        _ => {
                            {
                                let mut g = self.inner.lock().unwrap();
                                if g.client.as_ref().is_some_and(|x| Arc::ptr_eq(x, c)) {
                                    g.client = None;
                                }
                            }
                            peer = Peer::default();
                            logw!("receiver disconnected");
                            // Outside the lock: the handler stops the session, and the parts it
                            // stops send to the receiver (send() takes the lock) while holding
                            // their own.
                            on_line("state gone");
                            continue;
                        }
                    }
                }
                self.keep_alive(&mut peer, c, &names);
            }
        }
    }
}

// The pid at the other end of a Unix stream socket (SO_PEERCRED).
#[cfg(any(target_os = "linux", target_os = "android"))]
fn peer_pid(s: &UnixStream) -> Result<i32, String> {
    let mut cred = libc::ucred { pid: 0, uid: 0, gid: 0 };
    let mut len = std::mem::size_of::<libc::ucred>() as libc::socklen_t;
    let r = unsafe {
        libc::getsockopt(
            s.as_raw_fd(),
            libc::SOL_SOCKET,
            libc::SO_PEERCRED,
            &mut cred as *mut libc::ucred as *mut libc::c_void,
            &mut len,
        )
    };
    if r != 0 || cred.pid <= 0 {
        return Err(crate::config::last_errstr());
    }
    Ok(cred.pid)
}

#[cfg(not(any(target_os = "linux", target_os = "android")))]
fn peer_pid(_s: &UnixStream) -> Result<i32, String> {
    Err(crate::config::strerror(libc::ENOPROTOOPT))
}

/// /proc/<pid>/comm as fgets() into 32 bytes reads it: up to 31 bytes, cut at the newline.
pub fn read_comm(pid: i64) -> String {
    let Ok(b) = std::fs::read(format!("/proc/{pid}/comm")) else { return String::new() };
    let b = &b[..b.len().min(31)];
    let b = match b.iter().position(|&c| c == b'\n') {
        Some(i) => &b[..i],
        None => b,
    };
    String::from_utf8_lossy(b).into_owned()
}

// SIGKILL for the process at the other end, if it is the receiver.
fn kill_peer(s: &UnixStream, names: &str) {
    let pid = match peer_pid(s) {
        Ok(p) => p,
        Err(e) => {
            logw!("receiver: its pid is unknown ({}); not killed", e);
            return;
        }
    };
    let comm = read_comm(pid as i64);
    if comm.is_empty() || !names.contains(&format!(",{comm},")) {
        logw!("receiver: pid {} is \"{}\", not {}; not killed", pid, comm, &names[1..names.len() - 1]);
        return;
    }
    if unsafe { libc::kill(pid, libc::SIGKILL) } == 0 {
        logw!("receiver: pid {} ({}) killed; procguard restarts it", pid, comm);
    } else {
        logw!("receiver: killing pid {}: {}", pid, crate::config::last_errstr());
    }
}
