// SPDX-License-Identifier: GPL-3.0-or-later
//
// The port of aa-carlife-shim/tools/appc_patch_test.cpp: the appconnect patch on a live process,
// on a native Linux host (not under qemu-user, whose guest memory is not at its guest addresses
// in /proc/<pid>/mem), e.g. in the rust:bookworm image:
//   cargo test -p livi-carlife-shim --test appc_patch
//
// A child process named "appconnect" maps a file holding one build's instruction words at their
// addresses (0x2e000..0x4a000, read + exec, private, as the loader maps appconnect's text), and
// the test patches it as the shim does, for the 0358, 0394 and 0317 builds in turn: stock code
// gets every change and keeps it, a second run has nothing to do, a change whose code differs in
// one word is left alone while the other is still made, a change of several runs (callkey) gets
// every run, code already patched (the rebranded appconnect from kzb-re/patch_appconnect.py) is
// taken as is, no process is reported, and the file behind the mapping is never written; with a
// record (appc::Seen) a process found with every change on is reported once and skipped. An
// appconnect of neither build is left alone. One test: apply() looks at every appconnect.
#![cfg(target_os = "linux")]

use std::ffi::CString;
use std::os::unix::fs::FileExt;

use livi_carlife_shim::appc::{self, Seen, PATCHES};

const BASE: u64 = 0x2e000;
const SIZE: usize = 0x1c000; // every build's code words
const BUILDS: [&str; 3] = ["0358", "0394", "0317"];

struct T {
    fails: usize,
}

impl T {
    fn check(&mut self, what: &str, ok: bool) {
        println!("{}: {}", if ok { "PASS" } else { "FAIL" }, what);
        if !ok {
            self.fails += 1;
        }
    }
}

fn put(img: &mut [u8], addr: u64, w: u32) {
    let i = (addr - BASE) as usize;
    img[i..i + 4].copy_from_slice(&w.to_ne_bytes());
}

// The index in PATCHES of change `name`'s variant for `build`.
fn idx(name: &str, build: &str) -> usize {
    PATCHES.iter().position(|p| p.name == name && p.build == build).unwrap_or_else(|| panic!("no {name} for {build}"))
}

// One build's appconnect words; `patched` gives every edit its new word; `spoil` changes one
// guard word of that patch.
fn image(build: &str, patched: bool, spoil: Option<usize>) -> Vec<u8> {
    let mut img = vec![0u8; SIZE];
    for p in PATCHES.iter().filter(|p| p.build == build) {
        for g in p.guards {
            put(&mut img, g.addr, g.word);
        }
    }
    for p in PATCHES.iter().filter(|p| p.build == build) {
        for e in p.edits {
            put(&mut img, e.addr, if patched { e.patched } else { e.stock });
        }
    }
    if let Some(s) = spoil {
        put(&mut img, PATCHES[s].guards[0].addr, PATCHES[s].guards[0].word ^ 1);
    }
    img
}

// A child named "appconnect" with `img` mapped at BASE from file `path`; its pid.
fn spawn(t: &mut T, path: &str, img: &[u8]) -> i32 {
    std::fs::write(path, img).unwrap();
    let cpath = CString::new(path).unwrap();
    let mut ready = [0i32; 2];
    assert_eq!(unsafe { libc::pipe(ready.as_mut_ptr()) }, 0);
    let pid = unsafe { libc::fork() };
    if pid == 0 {
        // Only system calls from here.
        unsafe {
            libc::prctl(libc::PR_SET_NAME, c"appconnect".as_ptr(), 0, 0, 0);
            let fd = libc::open(cpath.as_ptr(), libc::O_RDONLY);
            let p = libc::mmap(
                BASE as *mut libc::c_void,
                SIZE,
                libc::PROT_READ | libc::PROT_EXEC,
                libc::MAP_PRIVATE | libc::MAP_FIXED,
                fd,
                0,
            );
            let ok: u8 = (p == BASE as *mut libc::c_void) as u8;
            if libc::write(ready[1], &ok as *const u8 as *const libc::c_void, 1) != 1 {
                libc::_exit(1);
            }
            loop {
                libc::pause();
            }
        }
    }
    let mut ok = 0u8;
    if unsafe { libc::read(ready[0], &mut ok as *mut u8 as *mut libc::c_void, 1) } != 1 || ok == 0 {
        t.check(&format!("child could not map 0x{BASE:x}"), false);
    }
    unsafe {
        libc::close(ready[0]);
        libc::close(ready[1]);
    }
    pid
}

fn reap(pid: i32) {
    unsafe {
        libc::kill(pid, libc::SIGKILL);
        libc::waitpid(pid, std::ptr::null_mut(), 0);
    }
}

fn word_in(pid: i32, addr: u64) -> u32 {
    let Ok(f) = std::fs::File::open(format!("/proc/{pid}/mem")) else { return 0 };
    let mut b = [0u8; 4];
    match f.read_at(&mut b, addr) {
        Ok(4) => u32::from_ne_bytes(b),
        _ => 0,
    }
}

fn word_in_file(path: &str, addr: u64) -> u32 {
    let f = std::fs::File::open(path).unwrap();
    let mut b = [0u8; 4];
    match f.read_at(&mut b, addr - BASE) {
        Ok(4) => u32::from_ne_bytes(b),
        _ => 0,
    }
}

// Every edit of patch p reads `patched` (or stock) words in the process.
fn edits(pid: i32, p: usize, patched: bool) -> bool {
    PATCHES[p].edits.iter().all(|e| word_in(pid, e.addr) == if patched { e.patched } else { e.stock })
}

// Every change in one build's appconnect: made, kept, left alone when a guard differs, taken
// as is when the file has them, left alone when half done.
fn run_build(t: &mut T, file: &str, b: &str) {
    let mut msg = String::new();
    let (cf, ip, t1, t2) = (idx("callfix", b), idx("iphone", b), idx("iphonetile", b), idx("iphonetile2", b));
    let (sh, ck) = (idx("seekhold", b), idx("callkey", b));
    let pid = spawn(t, file, &image(b, false, None));
    let ok = appc::apply(&mut msg, "appconnect", None);
    println!("      {msg}");
    t.check(
        &format!("{b}: stock code: every change made"),
        ok && msg.contains("callfix: pid")
            && msg.contains("iphone: pid")
            && msg.contains("straight to CarPlay")
            && msg.contains("iphonetile: pid")
            && msg.contains("iphonetile2: pid")
            && msg.contains("seekhold: pid")
            && msg.contains("callkey: pid"),
    );
    t.check(
        &format!("{b}: their words are the patched ones, every run of each"),
        edits(pid, cf, true)
            && edits(pid, ip, true)
            && edits(pid, t1, true)
            && edits(pid, t2, true)
            && edits(pid, sh, true)
            && edits(pid, ck, true),
    );
    t.check(
        &format!("{b}: the guard words are unchanged"),
        word_in(pid, PATCHES[cf].guards[10].addr) == PATCHES[cf].guards[10].word
            && word_in(pid, PATCHES[ip].guards[7].addr) == PATCHES[ip].guards[7].word,
    );
    t.check(
        &format!("{b}: the file behind the mapping is not written"),
        word_in_file(file, PATCHES[cf].edits[0].addr) == PATCHES[cf].edits[0].stock
            && word_in_file(file, PATCHES[ip].edits[3].addr) == PATCHES[ip].edits[3].stock,
    );
    let ok = appc::apply(&mut msg, "appconnect", None);
    t.check(&format!("{b}: second run: already on, nothing to say"), ok && msg.is_empty());
    reap(pid);

    let pid = spawn(t, file, &image(b, false, Some(ip)));
    let ok = appc::apply(&mut msg, "appconnect", None);
    println!("      {msg}");
    t.check(
        &format!("{b}: one iphone guard word differs: iphone left alone, callfix still made"),
        !ok && msg.contains("iphone: pid")
            && msg.contains("not a known appconnect build")
            && edits(pid, ip, false)
            && edits(pid, cf, true),
    );
    reap(pid);

    let pid = spawn(t, file, &image(b, true, None));
    let ok = appc::apply(&mut msg, "appconnect", None);
    t.check(
        &format!("{b}: already patched in the file (rebranded appconnect): taken as is"),
        ok && msg.is_empty()
            && edits(pid, cf, true)
            && edits(pid, ip, true)
            && edits(pid, t1, true)
            && edits(pid, t2, true)
            && edits(pid, sh, true)
            && edits(pid, ck, true),
    );
    reap(pid);

    let mut mixed = image(b, false, None);
    put(&mut mixed, PATCHES[ip].edits[0].addr, PATCHES[ip].edits[0].patched); // half-patched
    let pid = spawn(t, file, &mixed);
    let ok = appc::apply(&mut msg, "appconnect", None);
    println!("      {msg}");
    t.check(
        &format!("{b}: half-patched code: left alone"),
        !ok && msg.contains("neither stock nor patched")
            && word_in(pid, PATCHES[ip].edits[1].addr) == PATCHES[ip].edits[1].stock,
    );
    reap(pid);

    // callkey's last run (the branch into its new code) written, the rest stock.
    let mut mixed = image(b, false, None);
    let last = PATCHES[ck].edits[PATCHES[ck].edits.len() - 1];
    put(&mut mixed, last.addr, last.patched);
    let pid = spawn(t, file, &mixed);
    let ok = appc::apply(&mut msg, "appconnect", None);
    println!("      {msg}");
    t.check(
        &format!("{b}: callkey with one run written: left alone, the others still made"),
        !ok && msg.contains("callkey: pid")
            && msg.contains("neither stock nor patched")
            && word_in(pid, PATCHES[ck].edits[0].addr) == PATCHES[ck].edits[0].stock
            && edits(pid, sh, true),
    );
    reap(pid);
}

#[test]
fn appconnect_patch_on_a_live_process() {
    let mut t = T { fails: 0 };
    let file = format!("/tmp/appc_patch_test.{}.text", std::process::id());
    let mut msg = String::new();
    let names = PATCHES.len() == 6 * BUILDS.len()
        && PATCHES.iter().enumerate().all(|(p, x)| x.build == BUILDS[p % 3] && x.name == PATCHES[p - p % 3].name);
    t.check("six changes, each for the 0358, 0394 and 0317 appconnect", names);

    t.check(
        "no appconnect process: reported, not patched",
        !appc::apply(&mut msg, "appconnect", None) && msg == "no appconnect process",
    );

    for b in BUILDS {
        run_build(&mut t, &file, b);
    }

    let pid = spawn(&mut t, &file, &vec![0u8; SIZE]);
    let ok = appc::apply(&mut msg, "appconnect", None);
    println!("      {msg}");
    t.check(
        "an appconnect of neither build: every change left alone",
        !ok && msg.contains("callfix: pid") && msg.contains("not a known appconnect build"),
    );
    reap(pid);

    // With a record of the processes found patched (the shim's AppcWatch): a process is said
    // once, then not opened again while it lives, and forgotten when it is gone.
    let mut seen = Seen::default();
    let pid = spawn(&mut t, &file, &image("0358", true, None));
    let ok = appc::apply(&mut msg, "appconnect", Some(&mut seen));
    t.check(
        "with a record: the patched file's process is said to have every change, once",
        ok && msg.contains("every change is in place already") && seen.on.contains_key(&pid) && seen.found == 1,
    );
    let ok = appc::apply(&mut msg, "appconnect", Some(&mut seen));
    t.check("... then skipped (nothing said), still counted", ok && msg.is_empty() && seen.found == 1);
    reap(pid);
    let ok = appc::apply(&mut msg, "appconnect", Some(&mut seen));
    t.check("... and forgotten once it is gone", !ok && msg == "no appconnect process" && seen.on.is_empty());
    let pid = spawn(&mut t, &file, &image("0394", false, None));
    let ok = appc::apply(&mut msg, "appconnect", Some(&mut seen));
    t.check(
        "with a record, stock code: every change made and the process recorded",
        ok && msg.contains("callfix: pid") && edits(pid, idx("iphone", "0394"), true) && seen.on.contains_key(&pid),
    );
    t.check(
        "its start time is read",
        appc::start_time(pid) != 0 && Some(&appc::start_time(pid)) == seen.on.get(&pid),
    );
    reap(pid);

    let _ = std::fs::remove_file(&file);
    println!("{} failure(s)", t.fails);
    assert_eq!(t.fails, 0);
}
