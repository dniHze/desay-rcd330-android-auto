// SPDX-License-Identifier: GPL-3.0-or-later
//
// Changes to appconnect's code, made in the running process; ported from
// aa-carlife-shim/src/appc_patch.hpp, which stays the source of truth for the tables (the
// rebranded appconnect is baked by kzb-re/patch_appconnect.py from that header;
// tests/appc_tables.rs checks that appc_tables.rs equals it). The header has what each change
// does and why:
//   1. callfix: outgoing calls stay on Android Auto;
//   2. iphone: an iPhone goes straight to CarPlay;
//   3. iphonetile, iphonetile2: an iPhone is not offered Android Auto on the connect screen;
//   4. seekhold: holding a seek key seeks;
//   5. callkey: the phone key reaches Android Auto.
// Three builds of appconnect are known (0358, 0394, 0317); for each change the variant whose
// guard words all match is used; none matching leaves appconnect alone.
//
// Each change is made only when every guard word around it (the code it relies on, the
// functions it calls) is exactly the unit's, and its edit words are all stock or all patched.
// The edits are written run by run (a run: consecutive words, one write), in table order.
// The text is a private mapping, so writing it through /proc/<pid>/mem copies the page for that
// one process: the file on the unit is never written; a new appconnect process or a reboot runs
// the file's code again until the shim patches it again.
//
// AppcWatch keeps them on: applied within about a second of an appconnect process appearing,
// and again if it is replaced. The iPhone change must be in place before an iPhone is plugged
// in, which no CarLife call announces, so this does not wait for one. Each change is logged
// once; "no appconnect process" only while the unit is still booting. A process found with every
// change on (the patched file in the image, or patched here) is not opened again; one whose code
// is not the expected appconnect is looked at every 30 s.

use std::collections::{BTreeMap, HashSet};
use std::fs::{File, OpenOptions};
use std::os::unix::fs::FileExt;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use crate::config::{errstr, Config};
use crate::control::read_comm;
use crate::video::proc_pid;
use crate::{logi, logw};

#[path = "appc_tables.rs"]
mod tables;
pub use tables::PATCHES;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Word {
    pub addr: u64,
    pub word: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Edit {
    pub addr: u64,
    pub stock: u32,
    pub patched: u32,
}

#[derive(Debug)]
pub struct Patch {
    pub name: &'static str,  // the change; its variants (one per appconnect build) share the name
    pub build: &'static str, // the appconnect build this variant is for
    pub what: &'static str,  // logged when applied
    pub guards: &'static [Word],
    pub edits: &'static [Edit], // runs of consecutive words, written run by run in this order
}

// The C++ format()'s 256-byte buffer.
fn cut(s: String) -> String {
    crate::log::cut(&s, 255).to_string()
}

fn read_word(f: &File, addr: u64, w: &mut u32) -> bool {
    let mut b = [0u8; 4];
    match f.read_at(&mut b, addr) {
        Ok(4) => {
            *w = u32::from_ne_bytes(b);
            true
        }
        _ => false,
    }
}

/// When process `pid` started (/proc/<pid>/stat field 22, clock ticks since boot); 0 if gone.
/// With the pid it names one process: a pid can be reused, a pid and its start time cannot.
pub fn start_time(pid: i32) -> u64 {
    let Ok(b) = std::fs::read(format!("/proc/{pid}/stat")) else { return 0 };
    let b = &b[..b.len().min(511)];
    // the name may hold spaces and parentheses
    let Some(p) = b.iter().rposition(|&c| c == b')') else { return 0 };
    let rest = String::from_utf8_lossy(&b[p + 1..]);
    // after ")": state (3), ppid (4) ... starttime (22)
    rest.split_ascii_whitespace().nth(19).and_then(|s| s.parse().ok()).unwrap_or(0)
}

/// The processes already found with every change on, so they are not opened again; filled and
/// pruned by apply(). `found` is how many processes the last apply() saw.
#[derive(Default)]
pub struct Seen {
    pub on: BTreeMap<i32, u64>, // pid -> start time
    pub found: i32,
}

/// Whether every guard word of variant `p` is the process's.
pub fn guards_match(rf: &File, p: &Patch) -> bool {
    let mut w = 0u32;
    p.guards.iter().all(|g| read_word(rf, g.addr, &mut w) && w == g.word)
}

/// One patch in one process, checked through `rf` (read-only); `wf` is opened read-write on
/// `path` only when a change is to be written. True when it is on (now or before); `msg` says
/// what was done or why not, and stays empty when it was already on.
fn apply_one(rf: &File, wf: &mut Option<Result<File, std::io::Error>>, path: &str, pid: i32, p: &Patch, msg: &mut String)
    -> bool
{
    let mut w = 0u32;
    for g in p.guards {
        if !read_word(rf, g.addr, &mut w) || w != g.word {
            *msg = cut(format!(
                "{}: pid {} is not the expected appconnect (0x{:x}: 0x{:08x}, expected 0x{:08x}); left alone",
                p.name, pid, g.addr, w, g.word
            ));
            return false;
        }
    }
    let (mut stock, mut patched) = (0, 0);
    if p.edits.len() > 16 {
        return false;
    }
    let mut want = Vec::with_capacity(p.edits.len());
    for e in p.edits {
        if !read_word(rf, e.addr, &mut w) {
            *msg = cut(format!("{}: pid {}: reading 0x{:x}: {}", p.name, pid, e.addr, crate::config::last_errstr()));
            return false;
        }
        if w == e.stock {
            stock += 1;
        }
        if w == e.patched {
            patched += 1;
        }
        want.push(e.patched);
    }
    if patched == p.edits.len() {
        return true;
    }
    if stock != p.edits.len() {
        *msg = cut(format!("{}: pid {}: the code to change is neither stock nor patched; left alone", p.name, pid));
        return false;
    }
    if wf.is_none() {
        *wf = Some(OpenOptions::new().read(true).write(true).open(path));
    }
    let mut i = 0;
    while i < p.edits.len() {
        let mut end = i + 1;
        while end < p.edits.len() && p.edits[end].addr == p.edits[end - 1].addr + 4 {
            end += 1;
        }
        let bytes: Vec<u8> = want[i..end].iter().flat_map(|v| v.to_ne_bytes()).collect();
        let n = bytes.len();
        let mut back = vec![0u8; n];
        let failed = match wf.as_ref().unwrap() {
            Err(e) => Some(errstr(e)),
            Ok(f) => {
                let ok = matches!(f.write_at(&bytes, p.edits[i].addr), Ok(m) if m == n)
                    && matches!(rf.read_at(&mut back, p.edits[i].addr), Ok(m) if m == n)
                    && back == bytes;
                (!ok).then(crate::config::last_errstr)
            }
        };
        if let Some(err) = failed {
            *msg = cut(format!("{}: pid {}: writing 0x{:x} failed: {}", p.name, pid, p.edits[i].addr, err));
            return false;
        }
        i = end;
    }
    *msg = cut(format!("{}: pid {}: {} (in memory only)", p.name, pid, p.what));
    true
}

/// Every patch in one process. True when all are on.
pub fn patch_process(pid: i32, msg: &mut String) -> bool {
    let path = format!("/proc/{pid}/mem");
    let rf = match File::open(&path) {
        Ok(f) => f,
        Err(e) => {
            *msg = cut(format!("{}: {}", path, errstr(&e)));
            return false;
        }
    };
    let mut wf = None;
    let mut all = true;
    for i in 0..PATCHES.len() {
        if i > 0 && PATCHES[i].name == PATCHES[i - 1].name {
            continue; // a variant, done
        }
        // The variant for this appconnect build: the first whose guard words all match.
        let use_ = PATCHES[i..].iter().take_while(|p| p.name == PATCHES[i].name).find(|p| guards_match(&rf, p));
        let mut one = String::new();
        match use_ {
            None => {
                one = cut(format!("{}: pid {} is not a known appconnect build; left alone", PATCHES[i].name, pid));
                all = false;
            }
            Some(p) => {
                if !apply_one(&rf, &mut wf, &path, pid, p, &mut one) {
                    all = false;
                }
            }
        }
        if !one.is_empty() {
            if !msg.is_empty() {
                msg.push_str("; ");
            }
            msg.push_str(&one);
        }
    }
    all
}

/// Patch every process named `comm` (appconnect). True when all of them, and at least one, have
/// every patch on; `msg` collects what was done, empty when there was nothing new. With `seen`,
/// a process found with every change on is not opened again while it lives, and the first time
/// it is found that way (nothing to write: the patched file) that is said in `msg`.
pub fn apply(msg: &mut String, comm: &str, seen: Option<&mut Seen>) -> bool {
    msg.clear();
    let d = match std::fs::read_dir("/proc") {
        Ok(d) => d,
        Err(e) => {
            *msg = cut(format!("/proc: {}", errstr(&e)));
            return false;
        }
    };
    let mut found = 0;
    let mut all = true;
    let mut alive = BTreeMap::new();
    for e in d.flatten() {
        let Some(pid) = proc_pid(&e) else { continue };
        if read_comm(pid) != comm {
            continue;
        }
        let pid = pid as i32;
        found += 1;
        let start = if seen.is_some() { start_time(pid) } else { 0 };
        if let Some(s) = seen.as_deref()
            && s.on.get(&pid).is_some_and(|&t| t == start) && start != 0 {
                alive.insert(pid, start);
                continue;
            }
        let mut one = String::new();
        let ok = patch_process(pid, &mut one);
        if !ok {
            all = false;
        }
        if ok && seen.is_some() && start != 0 {
            alive.insert(pid, start);
            if one.is_empty() {
                one = cut(format!("pid {pid}: every change is in place already (the patched appconnect)"));
            }
        }
        if !one.is_empty() {
            if !msg.is_empty() {
                msg.push_str("; ");
            }
            msg.push_str(&one);
        }
    }
    if let Some(s) = seen {
        s.on = alive;
        s.found = found;
    }
    if found == 0 {
        *msg = cut(format!("no {comm} process"));
    }
    found > 0 && all
}

// ---------------------------------------------------------------------------------------------

struct WatchState {
    stop: bool,
    now: bool,
}

pub struct AppcWatch {
    cfg: Arc<Config>,
    state: Mutex<WatchState>,
    cv: Condvar,
    patched: AtomicBool,
    worker: Mutex<Option<JoinHandle<()>>>,
}

impl AppcWatch {
    pub fn new(cfg: Arc<Config>) -> Arc<Self> {
        Arc::new(AppcWatch {
            cfg,
            state: Mutex::new(WatchState { stop: false, now: false }),
            cv: Condvar::new(),
            patched: AtomicBool::new(false),
            worker: Mutex::new(None),
        })
    }

    pub fn start(self: &Arc<Self>) {
        if self.cfg.appc_patch {
            let w = self.clone();
            *self.worker.lock().unwrap() = Some(std::thread::spawn(move || w.run()));
        }
    }

    pub fn stop(&self) {
        self.state.lock().unwrap().stop = true;
        self.cv.notify_all();
        if let Some(t) = self.worker.lock().unwrap().take() {
            let _ = t.join();
        }
    }

    /// Look now (a CarLife call came: appconnect may be a new process).
    pub fn now(&self) {
        self.state.lock().unwrap().now = true;
        self.cv.notify_all();
    }

    /// Every change is on in every appconnect process.
    pub fn patched(&self) -> bool {
        self.patched.load(Ordering::SeqCst)
    }

    fn run(&self) {
        let (mut last, mut previous) = (String::new(), String::new());
        let mut seen = Seen::default();
        let mut check = Subscribed::default();
        loop {
            let mut msg = String::new();
            let ok = apply(&mut msg, "appconnect", Some(&mut seen));
            // The same refusal twice in a row: not a process caught mid-exec, its code differs.
            let differs = !ok && seen.found > 0 && msg == previous;
            previous = msg.clone();
            self.patched.store(ok, Ordering::SeqCst);
            if !msg.is_empty() && msg != last {
                if ok {
                    logi!("appconnect patch: {}", msg);
                } else {
                    logw!("appconnect patch: {}", msg);
                }
            }
            if !msg.is_empty() {
                last = msg;
            }
            self.check_subscribed(&mut check);
            // Every second while no appconnect runs (the unit starting), every 5 s once all is
            // on, every 30 s while its code is not what the changes expect (another firmware).
            let wait_s = if ok { 5 } else if differs { 30 } else { 1 };
            let st = self.state.lock().unwrap();
            let (mut st, _) =
                self.cv.wait_timeout_while(st, Duration::from_secs(wait_s), |s| !s.stop && !s.now).unwrap();
            st.now = false;
            if st.stop {
                return;
            }
        }
    }

    // appconnect subscribes to the CarLife status events once, when it starts
    // (SVPCarLifeService_Client::InitListener: no retry, no reconnect). One that started before
    // the shim took the slot is subscribed to the stopped stock service and never hears Android
    // Auto connect: no AA screen after picking it, "connecting" for good (step 3 runs 45, 46).
    // Nothing here can mend that (appmgr does not restart apps); it is logged once per process,
    // so a run shows it at once. Subscribed means holding a TCP connection to our publisher.
    fn check_subscribed(&self, c: &mut Subscribed) {
        if self.cfg.pub_port <= 0 {
            return;
        }
        let pid = first_pid("appconnect");
        if pid <= 0 || pid == c.checked_pid {
            return;
        }
        let now = Instant::now();
        if pid != c.seen_pid {
            c.seen_pid = pid;
            c.seen_at = Some(now);
        }
        if connected_to(pid, self.cfg.pub_port) {
            c.checked_pid = pid;
            logi!("appconnect (pid {}) is subscribed to our status events", pid);
        } else if c.seen_at.is_some_and(|t| now - t >= Duration::from_secs(20)) {
            // Not yet subscribed 20 s after it was first seen: it will not be.
            c.checked_pid = pid;
            logw!(
                "appconnect (pid {}) is NOT subscribed to our status events: it started before the shim took the \
                 CarLife slot, so it will not see Android Auto connect (no AA screen by itself, \"connecting\" \
                 stays) until the unit restarts with the shim in time",
                pid
            );
        }
    }
}

#[derive(Default)]
struct Subscribed {
    checked_pid: i32,           // judged: subscribed or not
    seen_pid: i32,              // the appconnect process being watched
    seen_at: Option<Instant>,
}

fn first_pid(comm: &str) -> i32 {
    let Ok(d) = std::fs::read_dir("/proc") else { return 0 };
    for e in d.flatten() {
        let Some(pid) = proc_pid(&e) else { continue };
        if read_comm(pid) == comm {
            return pid as i32;
        }
    }
    0
}

// Process `pid` has an established TCP connection to local port `port` (/proc/net/tcp).
fn connected_to(pid: i32, port: i32) -> bool {
    let Ok(text) = std::fs::read_to_string("/proc/net/tcp") else { return false };
    let mut inodes = HashSet::new();
    for line in text.lines() {
        // " %*d: %*x:%*x %x:%x %x %*x:%*x %*x:%*x %*x %*d %*d %lu"
        let f: Vec<&str> = line.split_ascii_whitespace().collect();
        if f.len() < 10 || !f[0].ends_with(':') {
            continue;
        }
        let Some((_, rem_port)) = f[2].split_once(':') else { continue };
        let (Ok(rem_port), Ok(state), Ok(inode)) =
            (u32::from_str_radix(rem_port, 16), u32::from_str_radix(f[3], 16), f[9].parse::<u64>())
        else {
            continue;
        };
        if rem_port as i32 == port && state == 1 && inode != 0 {
            inodes.insert(inode);
        }
    }
    if inodes.is_empty() {
        return false;
    }
    let Ok(d) = std::fs::read_dir(format!("/proc/{pid}/fd")) else { return false };
    d.flatten().any(|e| {
        std::fs::read_link(e.path()).ok().and_then(|t| {
            let t = t.to_string_lossy().into_owned();
            t.strip_prefix("socket:[")?.strip_suffix(']')?.parse::<u64>().ok()
        }).is_some_and(|ino| inodes.contains(&ino))
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    const BUILDS: [&str; 3] = ["0358", "0394", "0317"];

    #[test]
    fn six_changes_each_for_the_0358_0394_and_0317_appconnect() {
        assert_eq!(PATCHES.len(), 6 * BUILDS.len());
        for (p, patch) in PATCHES.iter().enumerate() {
            assert_eq!(patch.build, BUILDS[p % BUILDS.len()]);
            assert_eq!(patch.name, PATCHES[p - p % BUILDS.len()].name);
            assert!(patch.edits.len() <= 16);
        }
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn no_appconnect_process_is_reported() {
        let mut msg = String::new();
        assert!(!apply(&mut msg, "appconnect-none", None));
        assert_eq!(msg, "no appconnect-none process");
    }
}
