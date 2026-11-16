// SPDX-License-Identifier: GPL-3.0-or-later
//
// The car microphone's route through the Bluetooth module (bt_idl.hpp). Its echo-cancelling
// stage sits between the mic and MIB2BtOptIn and gives the mic to the SoC only in
// EAP_WIRED_PHONE_MODE; in EAP_BT_MODE (the default) the mic stays with Bluetooth calls and
// MIB2BtOptIn reads zeros (step 3 runs 12/13: 5 s of capture, peak level 0). Stock, appconnect
// switches it on CarLife's module-4 (voice) status, which the shim now publishes too; this
// sets it directly as well, as MirrorLink's ASR does, in case appconnect does not. Calls run
// on one worker; only the latest wanted state counts, and it is retried every 2 s until
// btservice takes it. At start it is put back to EAP_BT_MODE: a shim that died while the
// phone had the mic left it with the SoC, and btservice itself resets it only on a Bluetooth
// (re)connect, so calls on a phone already connected had no mic.

use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use rcf_lite::client::args;
use rcf_lite::Client;

use crate::config::{errstr, Config};
use crate::idl::{self, bt};
use crate::{logi, logw};

struct State {
    want: bool,
    dirty: bool,
    stop: bool,
    busy: bool,
    retry_at: Option<Instant>,
}

pub struct MicPath {
    cfg: Arc<Config>,
    state: Mutex<State>,
    cv: Condvar,
    worker: Mutex<Option<JoinHandle<()>>>,
}

impl MicPath {
    pub fn new(cfg: Arc<Config>) -> Arc<Self> {
        let me = Arc::new(MicPath {
            cfg,
            state: Mutex::new(State { want: false, dirty: true, stop: false, busy: false, retry_at: None }),
            cv: Condvar::new(),
            worker: Mutex::new(None),
        });
        if me.cfg.bt_port > 0 {
            let w = me.clone();
            *me.worker.lock().unwrap() = Some(std::thread::spawn(move || w.run()));
        }
        me
    }

    pub fn stop(&self) {
        self.state.lock().unwrap().stop = true;
        self.cv.notify_all();
        if let Some(t) = self.worker.lock().unwrap().take() {
            let _ = t.join();
        }
    }

    pub fn set(&self, phone: bool) {
        if self.cfg.bt_port <= 0 {
            return;
        }
        {
            let mut st = self.state.lock().unwrap();
            st.want = phone;
            st.dirty = true;
            st.retry_at = None;
        }
        self.cv.notify_all();
    }

    /// The wanted path is set (shutdown waits for it).
    pub fn idle(&self) -> bool {
        if self.cfg.bt_port <= 0 {
            return true;
        }
        let st = self.state.lock().unwrap();
        !st.dirty && !st.busy
    }

    fn run(&self) {
        let mut client: Option<Client> = None;
        let mut failing = false;
        loop {
            let want;
            {
                let mut st = self.state.lock().unwrap();
                loop {
                    if st.stop {
                        return;
                    }
                    let now = Instant::now();
                    if st.dirty && st.retry_at.is_none_or(|r| now >= r) {
                        break;
                    }
                    if st.dirty {
                        let until = st.retry_at.unwrap();
                        st = self.cv.wait_timeout(st, until.saturating_duration_since(now)).unwrap().0;
                    } else {
                        st = self.cv.wait(st).unwrap();
                    }
                }
                want = st.want;
                st.dirty = false;
                st.busy = true;
            }
            let path = if want { idl::EAP_WIRED_PHONE_MODE } else { idl::EAP_BT_MODE };
            let c = client.get_or_insert_with(|| {
                let mut c = Client::new(&self.cfg.bt_host, self.cfg.bt_port as u16, bt::BT_SERVICE);
                c.set_connect_timeout(Duration::from_millis(2000));
                c.set_call_timeout(Duration::from_millis(3000));
                c
            });
            let ok = match c.call(bt::BT_SERVICE_SET_ECNR_AUDIO_PATH, &args(|a| {
                a.put(&path);
            })) {
                Ok(_) => {
                    logi!("bt: SetECNRAudioPath({} {})", path, if want { "wired phone: the mic to the SoC" } else { "bt" });
                    failing = false;
                    true
                }
                Err(e) => {
                    if !failing {
                        logw!(
                            "bt: SetECNRAudioPath({}) on {}:{} failed: {} (retrying every 2 s)",
                            path,
                            self.cfg.bt_host,
                            self.cfg.bt_port,
                            errstr(&e)
                        );
                    }
                    failing = true;
                    client = None;
                    false
                }
            };
            let mut st = self.state.lock().unwrap();
            st.busy = false;
            if !ok && !st.dirty {
                st.dirty = true; // the same wanted path again, unless a newer one came meanwhile
                st.retry_at = Some(Instant::now() + Duration::from_secs(2));
            }
        }
    }
}
