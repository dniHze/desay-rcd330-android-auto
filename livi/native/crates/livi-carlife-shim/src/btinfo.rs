// SPDX-License-Identifier: GPL-3.0-or-later
//
// The car's Bluetooth address, for the receiver's service discovery response (else
// 00:00:00:00:00:00). A wired phone ignores it, but a wireless adapter's phone identifies and
// pairs the car by it (step 3 run 29: an adapter stopped after service discovery).
// btservice answers GetAttribute(3) with "%02x:%02x:%02x:%02x:%02x:%02x" from
// CBtManager::GetBtAddr (SVPBtServer::GetAttribute 0x1843dc), all zeros while Bluetooth is
// not up yet: asked every 10 s until it is real, then sent to the receiver ("btmac ...",
// upper case) once per receiver connection.

use std::io;
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use rcf_lite::client::args;
use rcf_lite::{Client, Reader};

use crate::config::{errstr, Config};
use crate::control::ControlChannel;
use crate::idl::bt;
use crate::{logi, logw};

const ATTR_LOCAL_ADDRESS: i32 = 3;

pub struct BtInfo {
    cfg: Arc<Config>,
    ctl: Arc<ControlChannel>,
    stop: Mutex<bool>,
    cv: Condvar,
    worker: Mutex<Option<JoinHandle<()>>>,
}

impl BtInfo {
    pub fn new(cfg: Arc<Config>, ctl: Arc<ControlChannel>) -> Arc<Self> {
        let me = Arc::new(BtInfo { cfg, ctl, stop: Mutex::new(false), cv: Condvar::new(), worker: Mutex::new(None) });
        if me.cfg.bt_port > 0 {
            let w = me.clone();
            *me.worker.lock().unwrap() = Some(std::thread::spawn(move || w.run()));
        }
        me
    }

    pub fn stop(&self) {
        *self.stop.lock().unwrap() = true;
        self.cv.notify_all();
        if let Some(t) = self.worker.lock().unwrap().take() {
            let _ = t.join();
        }
    }

    fn ask(&self, failing: &mut bool) -> String {
        let r = (|| -> io::Result<(i32, String)> {
            let mut c = Client::new(&self.cfg.bt_host, self.cfg.bt_port as u16, bt::BT_SERVICE);
            c.set_connect_timeout(Duration::from_millis(2000));
            c.set_call_timeout(Duration::from_millis(3000));
            let body = c.call(bt::BT_SERVICE_GET_ATTRIBUTE, &args(|a| {
                a.put(&ATTR_LOCAL_ADDRESS).put(&String::new());
            }))?;
            let mut rd = Reader::new(&body);
            Ok((rd.get()?, rd.get()?))
        })();
        match r {
            Ok((r, addr)) => {
                if r != 1 || addr.len() != 17 || addr == "00:00:00:00:00:00" {
                    return String::new();
                }
                addr.to_ascii_uppercase()
            }
            Err(e) => {
                if !*failing {
                    logw!("bt: GetAttribute(3) failed: {}", errstr(&e));
                }
                *failing = true;
                String::new()
            }
        }
    }

    fn run(&self) {
        let mut addr = String::new();
        let mut failing = false;
        let mut sent_to: u32 = 0; // ControlChannel::connections() when last sent
        let mut next_ask = Instant::now();
        loop {
            if addr.is_empty() && Instant::now() >= next_ask {
                addr = self.ask(&mut failing);
                if !addr.is_empty() {
                    logi!("bt: the unit's Bluetooth address is {}", addr);
                }
                next_ask = Instant::now() + Duration::from_secs(10);
            }
            if !addr.is_empty() {
                let c = self.ctl.connections();
                if c != sent_to && self.ctl.send(&format!("btmac {addr}")) {
                    sent_to = c;
                }
            }
            let g = self.stop.lock().unwrap();
            let (g, _) = self.cv.wait_timeout_while(g, Duration::from_secs(1), |s| !*s).unwrap();
            if *g {
                return;
            }
        }
    }
}
