// SPDX-License-Identifier: GPL-3.0-or-later
//
// Last mode: whether Android Auto had the speakers when the unit was last switched off, kept in
// a file (AA_LAST_MODE_FILE: on the SD card with start.sh, /storage/data/data/aa/ in the image).
// CarPlay's service keeps the same in connectionservice (Set_/Get_Carplay_LastMode, persist
// 100-102) and restores the audio owner at the next session; the CarLife slot has nothing like
// it, and the phone's autoplay took the speakers from the radio whatever was playing before
// (step 3 run 41: AA media 2.4 s after projecting). At the next connection the receiver gets
// "autoplay 0" when the car's own audio had them, and refuses the phone's autoplay once
// (AaConfig::refuse_autoplay): the source the unit restored keeps playing until the user
// starts AA's media.
//   * AA has the speakers once its media source is granted, and loses them when a main source
//     (radio, USB, Bluetooth audio...) takes over for good (AudioSource). Calls, prompts, the
//     mute and the power pause leave it.
//   * A change is written once it has held for 5 s, or at the switch-off if it is older than
//     1 s there, so the source changes of a switch-off itself are not taken for the user's; from
//     PS_USER_OFF / PS_SHUT_DOWN to PS_USER_ON nothing changes.
//   * Unknown (no file yet): AA may autoplay, as before.
// The file is replaced whole (a temporary file, fsync, rename, fsync of the directory), so a
// power cut leaves the old or the new record; its directory is made (0700) if missing. A
// change still waiting at exit is written if it is older than 1 s.

use std::io::Read;
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use crate::config::{dir_name, errstr, mkdirs, replace_file, Config};
use crate::{logi, logw};

struct State {
    saved: bool,   // what the file says
    pending: i32,  // a change to write: 1 AA, 0 the car, -1 none
    pending_at: Instant,
    frozen: bool,
    stop: bool,
}

pub struct LastMode {
    path: String,
    state: Mutex<State>,
    cv: Condvar,
    worker: Mutex<Option<JoinHandle<()>>>,
}

impl LastMode {
    pub fn new(cfg: &Config) -> Arc<Self> {
        let me = Arc::new(LastMode {
            path: cfg.last_mode_file.clone(),
            state: Mutex::new(State { saved: true, pending: -1, pending_at: Instant::now(), frozen: false, stop: false }),
            cv: Condvar::new(),
            worker: Mutex::new(None),
        });
        if me.path.is_empty() {
            return me;
        }
        let dir = dir_name(&me.path);
        if !dir.is_empty()
            && let Err(e) = mkdirs(&dir, 0o700) {
                logw!("last mode: cannot create {}: {}", dir, errstr(&e));
            }
        if let Ok(mut f) = std::fs::File::open(&me.path) {
            let mut buf = [0u8; 63];
            if let Ok(n) = f.read(&mut buf)
                && n > 0 && buf[..n].starts_with(b"speakers car") {
                    me.state.lock().unwrap().saved = false;
                }
        }
        let saved = me.state.lock().unwrap().saved;
        logi!(
            "last mode: {} ({})",
            if saved { "Android Auto had the speakers, or nothing saved yet" } else { "the car's own audio had the speakers" },
            me.path
        );
        let w = me.clone();
        *me.worker.lock().unwrap() = Some(std::thread::spawn(move || w.run()));
        me
    }

    pub fn stop(&self) {
        self.state.lock().unwrap().stop = true;
        self.cv.notify_all();
        if let Some(t) = self.worker.lock().unwrap().take() {
            let _ = t.join();
        }
    }

    pub fn enabled(&self) -> bool {
        !self.path.is_empty()
    }

    /// At the last switch-off (true when unknown).
    pub fn aa_had_speakers(&self) -> bool {
        self.state.lock().unwrap().saved
    }

    /// AA took (true) or lost (false) the speakers.
    pub fn speakers(&self, aa: bool) {
        if self.path.is_empty() {
            return;
        }
        {
            let mut st = self.state.lock().unwrap();
            if st.frozen {
                return;
            }
            st.pending = if aa { 1 } else { 0 };
            st.pending_at = Instant::now();
        }
        self.cv.notify_all();
    }

    /// The unit is switched off (true) or on again: keep what it was at the switch-off. A change
    /// from just before (1 s) is the switch-off's own; an older one is written now.
    pub fn freeze(&self, on: bool) {
        if self.path.is_empty() {
            return;
        }
        {
            let mut st = self.state.lock().unwrap();
            if on == st.frozen {
                return;
            }
            st.frozen = on;
            if !on || st.pending < 0 {
                return;
            }
            let now = Instant::now();
            if now - st.pending_at < Duration::from_secs(1) {
                st.pending = -1;
            } else {
                st.pending_at = now.checked_sub(Duration::from_secs(5)).unwrap_or(st.pending_at);
            }
        }
        self.cv.notify_all();
    }

    fn run(&self) {
        let mut st = self.state.lock().unwrap();
        while !st.stop {
            if st.pending < 0 {
                st = self.cv.wait(st).unwrap();
                continue;
            }
            let due = st.pending_at + Duration::from_secs(5);
            let now = Instant::now();
            if now < due {
                st = self.cv.wait_timeout(st, due - now).unwrap().0;
                continue;
            }
            let aa = st.pending == 1;
            st.pending = -1;
            if aa == st.saved {
                continue;
            }
            st.saved = aa;
            drop(st);
            self.write(aa);
            st = self.state.lock().unwrap();
        }
        // Stopping: a change that has held for 1 s is the user's, as at a switch-off (freeze).
        if st.pending >= 0
            && !st.frozen
            && Instant::now() - st.pending_at >= Duration::from_secs(1)
            && (st.pending == 1) != st.saved
        {
            st.saved = st.pending == 1;
            let saved = st.saved;
            drop(st);
            self.write(saved);
        }
    }

    fn write(&self, aa: bool) {
        let err = replace_file(&self.path, if aa { "speakers aa\n" } else { "speakers car\n" });
        if !err.is_empty() {
            logw!("last mode: writing {}: {}", self.path, err);
            return;
        }
        logi!("last mode: {} now has the speakers", if aa { "Android Auto" } else { "the car's own audio" });
    }
}
