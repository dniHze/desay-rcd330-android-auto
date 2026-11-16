// SPDX-License-Identifier: GPL-3.0-or-later
//
// The unit's power state (power_idl.hpp): powermgr's publisher tells when the unit is switched
// off with the ignition on (PS_USER_OFF), on again (PS_USER_ON), or going down (PS_SHUT_DOWN).
// Subscribed (again every 10 s if lost) on a worker; each event goes to the callback, on the
// subscription's thread.

use std::io;
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::Duration;

use rcf_lite::client::args;
use rcf_lite::{Client, Reader, Subscription};

use crate::config::{errstr, tcp_reachable, Config};
use crate::idl::{self, power};
use crate::{logi, logw};

pub type Callback = Box<dyn Fn(i32, i32, i32) + Send + Sync>;

#[derive(Default)]
struct Worker {
    failing: bool,
    subscription: Option<Subscription>,
}

pub struct PowerWatch {
    cfg: Arc<Config>,
    cb: Arc<Callback>,
    stop: Mutex<bool>,
    cv: Condvar,
    worker: Mutex<Option<JoinHandle<Worker>>>,
}

impl PowerWatch {
    pub fn new(cfg: Arc<Config>, cb: Callback) -> Arc<Self> {
        Arc::new(PowerWatch { cfg, cb: Arc::new(cb), stop: Mutex::new(false), cv: Condvar::new(), worker: Mutex::new(None) })
    }

    pub fn start(self: &Arc<Self>) {
        if self.cfg.power_listen_port > 0 {
            let w = self.clone();
            *self.worker.lock().unwrap() = Some(std::thread::spawn(move || w.run()));
        }
    }

    pub fn stop(&self) {
        *self.stop.lock().unwrap() = true;
        self.cv.notify_all();
        if let Some(t) = self.worker.lock().unwrap().take()
            && let Ok(mut w) = t.join()
                && let Some(s) = w.subscription.take() {
                    s.close();
                }
    }

    /// Stock's test for a temporary switch-on (an incoming call while the unit is off):
    /// GetPowerState answers (PS_USER_ON, 1).
    pub fn temporary_on(&self) -> bool {
        match self.query() {
            Some((state, sub)) => state == idl::PS_USER_ON && sub == 1,
            None => false,
        }
    }

    fn query(&self) -> Option<(i32, i32)> {
        if self.cfg.power_port <= 0 {
            return None;
        }
        let r = (|| -> io::Result<(i32, i32, u8)> {
            let mut c = Client::new(&self.cfg.power_host, self.cfg.power_port as u16, power::POWER_SERVICE);
            c.set_connect_timeout(Duration::from_millis(1000));
            c.set_call_timeout(Duration::from_millis(1000));
            let body = c.call(power::POWER_SERVICE_GET_POWER_STATE, &args(|a| {
                a.put(&idl::PS_UNDEFINED).put(&0u8);
            }))?;
            let mut rd = Reader::new(&body);
            Ok((rd.get()?, rd.get()?, rd.get()?))
        })();
        match r {
            Ok((r, state, sub)) => {
                logi!("power: GetPowerState -> {}: {} ({}), {}", r, idl::power_state_name(state), state, sub);
                Some((state, sub as i32))
            }
            Err(e) => {
                logw!("power: GetPowerState failed: {}", errstr(&e));
                None
            }
        }
    }

    // SVP_PowerListener servant: every event of the interface (power_idl.hpp).
    fn on_event(cb: &Callback, fn_id: i32, a: &[u8]) {
        let mut r = Reader::new(a);
        match fn_id {
            power::POWER_LISTENER_ON_SCREEN_OFF_CHANGED => {
                if let Ok(v) = r.get::<u8>() {
                    logi!("power: event ScreenOffChanged {}", v);
                }
            }
            power::POWER_LISTENER_ON_DISPLAYOFF_CHANGED => {
                if let Ok(v) = r.get::<i32>() {
                    logi!("power: event DisplayoffChanged {}", v);
                }
            }
            power::POWER_LISTENER_ON_POWER_STATE_CHANGED => {
                if let (Ok(s), Ok(a), Ok(b)) = (r.get::<i32>(), r.get::<u8>(), r.get::<u8>()) {
                    cb(s, a as i32, b as i32);
                }
            }
            _ => {}
        }
    }

    fn subscribe(&self, w: &mut Worker) {
        if w.subscription.as_ref().is_some_and(|s| s.is_connected()) {
            return;
        }
        let (host, port) = (&self.cfg.power_host, self.cfg.power_listen_port);
        if !tcp_reachable(host, port, 1000) {
            if !w.failing {
                logw!("power: no power publisher on {}:{}; switch-offs are not followed", host, port);
            }
            w.failing = true;
            return;
        }
        if let Some(s) = w.subscription.take() {
            logw!("power: subscription lost; subscribing again");
            s.close();
        }
        let cb = self.cb.clone();
        match Subscription::create(
            (host.as_str(), port as u16),
            power::POWER_LISTENER,
            rcf_lite::client::DEFAULT_CONNECT_TIMEOUT,
            rcf_lite::client::DEFAULT_CALL_TIMEOUT,
            move |fn_id, a| Self::on_event(&cb, fn_id, a),
        ) {
            Ok(s) => {
                w.subscription = Some(s);
                logi!("power: subscribed to the power events ({}:{})", host, port);
                w.failing = false;
                self.query();
            }
            Err(e) => {
                if !w.failing {
                    logw!("power: subscription failed: {}", errstr(&e));
                }
                w.failing = true;
            }
        }
    }

    fn run(&self) -> Worker {
        let mut w = Worker::default();
        loop {
            self.subscribe(&mut w);
            let g = self.stop.lock().unwrap();
            let (g, _) = self.cv.wait_timeout_while(g, Duration::from_secs(10), |s| !*s).unwrap();
            if *g {
                return w;
            }
        }
    }
}
