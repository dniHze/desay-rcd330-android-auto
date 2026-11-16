// SPDX-License-Identifier: GPL-3.0-or-later
//
// Crashes. A fault logs the signal, where it happened (the PC and LR, to look up in the
// unstripped aa-headless with addr2line: the binary is static) and a backtrace, to syslog (or
// stderr), using only async-signal-safe calls; then the signal is raised again with its default
// action, so the process dies by it and procguard restarts it within the launcher's budget. A
// panic (the C++ shim's uncaught exception) logs its text first, then aborts into the same path.

use std::sync::atomic::Ordering;

use crate::log::{self, TAG};
use crate::loge;

/// The build id ("src-" and the md5 of the sources; build.rs).
pub const VERSION: &str = env!("AA_SHIM_VERSION");

#[cfg(any(target_os = "linux", target_os = "android"))]
const MSG_NOSIGNAL: libc::c_int = libc::MSG_NOSIGNAL;
#[cfg(not(any(target_os = "linux", target_os = "android")))]
const MSG_NOSIGNAL: libc::c_int = 0;

struct Buf {
    b: [u8; 256],
    n: usize,
}

impl Buf {
    const CAP: usize = 254; // as the C++ emit: room for the newline

    fn new() -> Self {
        Buf { b: [0; 256], n: 0 }
    }

    fn put(&mut self, s: &[u8]) -> &mut Self {
        for &c in s {
            if self.n >= Self::CAP {
                break;
            }
            self.b[self.n] = c;
            self.n += 1;
        }
        self
    }

    fn num(&mut self, mut v: u64, base: u64) -> &mut Self {
        let mut tmp = [0u8; 24];
        let mut k = 0;
        loop {
            tmp[k] = b"0123456789abcdef"[(v % base) as usize];
            k += 1;
            v /= base;
            if v == 0 || k == tmp.len() {
                break;
            }
        }
        if base == 16 {
            self.put(b"0x");
        }
        while k > 0 {
            k -= 1;
            self.put(&[tmp[k]]);
        }
        self
    }
}

// One line, as the log sink formats it, to the syslog socket or stderr.
fn emit(text: &[u8]) {
    let mut b = Buf::new();
    let fd = if log::to_stderr() { -1 } else { log::CRASH_FD.load(Ordering::SeqCst) };
    let (s, ms) = log::stamp();
    if fd >= 0 {
        b.put(b"<11>").put(TAG.as_bytes()).put(b"[").num(unsafe { libc::getpid() } as u64, 10).put(b"]: [");
    } else {
        b.put(b"[").put(TAG.as_bytes()).put(b" ");
    }
    b.num(s as u64, 10).put(b".");
    if ms < 100 {
        b.put(b"0");
    }
    if ms < 10 {
        b.put(b"0");
    }
    b.num(ms as u64, 10).put(b"] ERROR: ").put(text);
    unsafe {
        if fd >= 0 {
            libc::send(fd, b.b.as_ptr() as *const libc::c_void, b.n, libc::MSG_DONTWAIT | MSG_NOSIGNAL);
        } else {
            b.b[b.n] = b'\n';
            libc::write(2, b.b.as_ptr() as *const libc::c_void, b.n + 1);
        }
    }
}

// The return addresses on the stack, through the unwinder Rust's runtime links (as glibc's
// backtrace() goes through libgcc's).
mod unwind {
    use std::ffi::c_void;

    #[repr(C)]
    pub struct Context {
        _p: [u8; 0],
    }
    pub type Trace = extern "C" fn(*mut Context, *mut c_void) -> libc::c_int;

    unsafe extern "C" {
        pub fn _Unwind_Backtrace(trace: Trace, arg: *mut c_void) -> libc::c_int;
    }

    #[cfg(not(target_arch = "arm"))]
    unsafe extern "C" {
        fn _Unwind_GetIP(ctx: *mut Context) -> usize;
    }

    #[cfg(not(target_arch = "arm"))]
    pub fn ip(ctx: *mut Context) -> usize {
        unsafe { _Unwind_GetIP(ctx) }
    }

    // ARM EHABI has no _Unwind_GetIP function: r15 through _Unwind_VRS_Get, the Thumb bit off.
    #[cfg(target_arch = "arm")]
    unsafe extern "C" {
        fn _Unwind_VRS_Get(ctx: *mut Context, class: libc::c_int, reg: u32, repr: libc::c_int, v: *mut c_void)
            -> libc::c_int;
    }

    #[cfg(target_arch = "arm")]
    pub fn ip(ctx: *mut Context) -> usize {
        let mut v: u32 = 0;
        unsafe { _Unwind_VRS_Get(ctx, 0, 15, 0, &mut v as *mut u32 as *mut c_void) };
        (v & !1) as usize
    }
}

struct Frames {
    pc: [usize; 32],
    n: usize,
}

extern "C" fn frame(ctx: *mut unwind::Context, arg: *mut std::ffi::c_void) -> libc::c_int {
    let f = unsafe { &mut *(arg as *mut Frames) };
    if f.n >= f.pc.len() {
        return 4; // _URC_NORMAL_STOP
    }
    f.pc[f.n] = unwind::ip(ctx);
    f.n += 1;
    0 // _URC_NO_REASON
}

fn sig_name(sig: libc::c_int) -> &'static [u8] {
    match sig {
        libc::SIGSEGV => b"SIGSEGV",
        libc::SIGBUS => b"SIGBUS",
        libc::SIGILL => b"SIGILL",
        libc::SIGFPE => b"SIGFPE",
        libc::SIGABRT => b"SIGABRT",
        _ => b"?",
    }
}

#[cfg(any(target_os = "linux", target_os = "android"))]
unsafe fn fault_address(info: *const libc::siginfo_t) -> usize {
    unsafe { (*info).si_addr() as usize }
}

#[cfg(not(any(target_os = "linux", target_os = "android")))]
unsafe fn fault_address(info: *const libc::siginfo_t) -> usize {
    unsafe { (*info).si_addr as usize }
}

extern "C" fn on_crash(sig: libc::c_int, info: *mut libc::siginfo_t, ctx: *mut libc::c_void) {
    let mut line = Buf::new();
    line.put(b"crash: signal ").num(sig as u64, 10).put(b" (").put(sig_name(sig)).put(b"), build ");
    line.put(VERSION.as_bytes());
    if !info.is_null() && (sig == libc::SIGSEGV || sig == libc::SIGBUS) {
        line.put(b", address ").num(unsafe { fault_address(info) } as u64, 16);
    }
    emit(&line.b[..line.n]);
    #[cfg(all(target_arch = "arm", any(target_os = "linux", target_os = "android")))]
    if !ctx.is_null() {
        let uc = unsafe { &*(ctx as *const libc::ucontext_t) };
        let mut l = Buf::new();
        l.put(b"crash: pc ").num(uc.uc_mcontext.arm_pc as u64, 16);
        l.put(b" lr ").num(uc.uc_mcontext.arm_lr as u64, 16);
        l.put(b" sp ").num(uc.uc_mcontext.arm_sp as u64, 16);
        emit(&l.b[..l.n]);
    }
    #[cfg(not(all(target_arch = "arm", any(target_os = "linux", target_os = "android"))))]
    let _ = ctx;
    let mut f = Frames { pc: [0; 32], n: 0 };
    unsafe { unwind::_Unwind_Backtrace(frame, &mut f as *mut Frames as *mut std::ffi::c_void) };
    for (i, pc) in f.pc[..f.n].iter().enumerate() {
        let mut l = Buf::new();
        l.put(b"crash: frame ").num(i as u64, 10).put(b" ").num(*pc as u64, 16);
        emit(&l.b[..l.n]);
    }
    // Die by the signal (procguard restarts after a signal, not after exit 1).
    unsafe {
        libc::signal(sig, libc::SIG_DFL);
        libc::raise(sig);
    }
}

static mut ALT_STACK: [u8; 16384] = [0; 16384];

pub fn install() {
    unsafe {
        let ss = libc::stack_t {
            ss_sp: std::ptr::addr_of_mut!(ALT_STACK) as *mut libc::c_void,
            ss_flags: 0,
            ss_size: 16384,
        };
        libc::sigaltstack(&ss, std::ptr::null_mut()); // the main thread's own stack overflowing still gets logged
        let mut sa: libc::sigaction = std::mem::zeroed();
        sa.sa_sigaction = on_crash as extern "C" fn(libc::c_int, *mut libc::siginfo_t, *mut libc::c_void) as usize;
        sa.sa_flags = libc::SA_SIGINFO | libc::SA_ONSTACK | libc::SA_RESETHAND;
        libc::sigemptyset(&mut sa.sa_mask);
        for sig in [libc::SIGSEGV, libc::SIGBUS, libc::SIGILL, libc::SIGFPE, libc::SIGABRT] {
            libc::sigaction(sig, &sa, std::ptr::null_mut());
        }
    }
    // The C++ shim's std::set_terminate(onTerminate): a panic anywhere ends the process.
    std::panic::set_hook(Box::new(|info| {
        let msg = info
            .payload()
            .downcast_ref::<&str>()
            .map(|s| s.to_string())
            .or_else(|| info.payload().downcast_ref::<String>().cloned())
            .unwrap_or_else(|| "a panic of an unknown type".to_string());
        match info.location() {
            Some(l) => loge!("uncaught exception: {} ({}:{})", msg, l.file(), l.line()),
            None => loge!("uncaught exception: {}", msg),
        }
        std::process::abort();
    }));
}
