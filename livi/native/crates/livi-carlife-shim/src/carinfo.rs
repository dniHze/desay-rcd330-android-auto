// SPDX-License-Identifier: GPL-3.0-or-later
//
// Vehicle data for Android Auto from the CAN service (can_idl.hpp):
//   * the CarPlay info record, polled with GetCanCarplayInfo every 2 s (canservice republishes
//     it on every MCU message anyway): DayNight -> "night <0|1>" (AA's night mode, the dark map
//     once the headlights are on; where stock appconnect gets the night mode it hands to
//     CarPlay), RightHandDrive -> "rhd <0|1>" (the driver's side in service discovery; stock
//     appconnect hands it to CarPlay's SetRightHandDrive), both when they change and again every
//     30 s in case the receiver restarted (it ignores repeats); TransmissionState -> "gear
//     <park|reverse|neutral|drive>" every poll, mapped as ipodservice maps it for the iPhone
//     (SVPIpodCANListener::SendThreadProc: 1 P, 2 R, 3 N, 4..6 D, else unknown, not sent);
//   * the vehicle speed, which canservice only publishes (OnCanVehicleSpeedChanged on its
//     SVP_CanListener publisher; GetCanVehicleSpeed asks it to publish the current value), in
//     0.01 km/h: ipodservice divides by 100 and by 3.6 for the iPhone's m/s
//     (SVPIpodCANListener::OnCanVehicleSpeedChanged 0xc8f78). Subscribed as the stock clients
//     do (libcan SVPCanService_Client::InitListener) and sent as "speed <m/s>" at most every
//     200 ms while it changes and every 2 s otherwise; the receiver derives the driving status
//     from it.

use std::collections::HashMap;
use std::io;
use std::sync::atomic::{AtomicI32, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use rcf_lite::client::args;
use rcf_lite::{Client, Reader, Subscription};

use crate::config::{errstr, tcp_reachable, Config};
use crate::control::ControlChannel;
use crate::idl::{can, CanCarplayInfo};
use crate::{logi, logv, logw};

struct Speed {
    seen: bool,
    raw: u16,
    sent_raw: u16,
    at: Option<Instant>,
    sent_at: Option<Instant>,
}

// SVP_CanListener servant's own state: what it logged.
#[derive(Default)]
struct Listener {
    last_info: i32,
    service_msg: HashMap<u16, String>,
    service_lines: i32,
}

pub struct CarInfo {
    cfg: Arc<Config>,
    ctl: Arc<ControlChannel>,
    stop: Mutex<bool>,
    cv: Condvar,
    night: AtomicI32, // the last poll's (-1: none yet)
    rhd: AtomicI32,
    // The speed: written by the subscription's thread, read by the worker.
    speed: Mutex<Speed>,
    worker: Mutex<Option<JoinHandle<()>>>,
}

// The worker's own.
struct Worker {
    client: Option<Client>,
    subscription: Option<Subscription>,
    subscribe_tried: Option<Instant>,
    subscribe_failing: bool,
    listener: Arc<Mutex<Listener>>,
}

fn gear_name(transmission: u8) -> Option<&'static str> {
    match transmission {
        1 => Some("park"),
        2 => Some("reverse"),
        3 => Some("neutral"),
        4..=6 => Some("drive"),
        _ => None,
    }
}

impl CarInfo {
    pub fn new(cfg: Arc<Config>, ctl: Arc<ControlChannel>) -> Arc<Self> {
        let me = Arc::new(CarInfo {
            cfg,
            ctl,
            stop: Mutex::new(false),
            cv: Condvar::new(),
            night: AtomicI32::new(-1),
            rhd: AtomicI32::new(-1),
            speed: Mutex::new(Speed { seen: false, raw: 0, sent_raw: 0xffff, at: None, sent_at: None }),
            worker: Mutex::new(None),
        });
        if me.cfg.can_port > 0 {
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

    /// Night and the driver's side as last read, for a session about to start (its service
    /// discovery reads the driver's side): a receiver that connected since the last poll has
    /// them before "start".
    pub fn send_current(&self) {
        let (night, rhd) = (self.night.load(Ordering::SeqCst), self.rhd.load(Ordering::SeqCst));
        if night < 0 || rhd < 0 {
            return;
        }
        self.ctl.send(if night != 0 { "night 1" } else { "night 0" });
        self.ctl.send(if rhd != 0 { "rhd 1" } else { "rhd 0" });
    }

    // On the subscription's thread, and from the poll (resend).
    fn on_speed(&self, raw: u16) {
        let mut sp = self.speed.lock().unwrap();
        let now = Instant::now();
        if !sp.seen {
            logi!("can: vehicle speed {}.{:02} km/h (first value)", raw / 100, raw % 100);
        }
        sp.seen = true;
        sp.raw = raw;
        sp.at = Some(now);
        self.maybe_send_speed(&mut sp, now);
    }

    // speed held.
    fn maybe_send_speed(&self, sp: &mut Speed, now: Instant) {
        let changed = sp.raw != sp.sent_raw;
        let since = sp.sent_at.map(|t| now - t);
        let due = |d: Duration| since.is_none_or(|s| s >= d);
        if (changed && due(Duration::from_millis(200))) || due(Duration::from_secs(2)) {
            let line = format!("speed {:.3}", sp.raw as f64 / 360.0); // 0.01 km/h -> m/s
            if self.ctl.send(&line) {
                sp.sent_raw = sp.raw;
                sp.sent_at = Some(now);
            }
        }
    }

    // The SVP_CanListener event `fn_id` (only the speed is used; the CarPlay record and the MCU
    // messages are logged at debug level).
    fn on_event(&self, l: &Mutex<Listener>, fn_id: i32, a: &[u8]) {
        let mut r = Reader::new(a);
        match fn_id {
            can::CAN_LISTENER_ON_CAN_VEHICLE_SPEED_CHANGED => {
                if let Ok(raw) = r.get::<u16>() {
                    self.on_speed(raw);
                }
            }
            // Logged (each change) to compare with the polled record: night with the headlights
            // on stayed DayNight 0 in step 3 runs 35-38.
            can::CAN_LISTENER_ON_CAN_CARPLAY_INFO_CHANGED => {
                let Ok(i) = r.get::<CanCarplayInfo>() else { return };
                let v = i.day_night as i32 | (i.transmission_state as i32) << 8 | (i.right_hand_drive as i32) << 16;
                let mut l = l.lock().unwrap();
                if v == l.last_info {
                    return;
                }
                l.last_info = v;
                logv!(
                    "can: event CarplayInfo DayNight {}, transmission {}, right-hand drive {}",
                    i.day_night,
                    i.transmission_state,
                    i.right_hand_drive
                );
            }
            // The MCU messages canservice passes through (0x73E1..0x73FE; no stock consumer):
            // logged when one changes, to look for a headlight / night signal (at most 300 lines).
            can::CAN_LISTENER_ON_CAN_SERVICE_MESSAGE => {
                let (Ok(id), Ok(data)) = (r.get::<u16>(), r.get::<Vec<u8>>()) else { return };
                let mut l = l.lock().unwrap();
                if l.service_lines >= 300 {
                    return;
                }
                let hex: String = data.iter().take(32).map(|b| format!("{b:02x}")).collect();
                let last = l.service_msg.entry(id).or_default();
                if *last == hex {
                    return;
                }
                *last = hex.clone();
                l.service_lines += 1;
                logv!("can: event ServiceMessage 0x{:04x} {}", id, hex);
            }
            _ => {}
        }
    }

    // Subscribe to the CAN service's publisher for the speed, unless subscribed; at most every
    // 10 s. Then ask for the current speed (published back).
    fn subscribe(self: &Arc<Self>, w: &mut Worker) {
        if self.cfg.can_listen_port <= 0 {
            return;
        }
        if w.subscription.as_ref().is_some_and(|s| s.is_connected()) {
            return;
        }
        let now = Instant::now();
        if w.subscribe_tried.is_some_and(|t| now - t < Duration::from_secs(10)) {
            return;
        }
        w.subscribe_tried = Some(now);
        let (host, port) = (&self.cfg.can_host, self.cfg.can_listen_port);
        // Only when the publisher is there (see tcp_reachable).
        if !tcp_reachable(host, port, 1000) {
            if !w.subscribe_failing {
                logw!("can: no CAN publisher on {}:{}; no speed until there is one", host, port);
            }
            w.subscribe_failing = true;
            if let Some(s) = w.subscription.take() {
                s.close();
            }
            return;
        }
        if let Some(s) = w.subscription.take() {
            logw!("can: speed subscription lost; subscribing again");
            s.close();
        }
        let me = self.clone();
        let l = w.listener.clone();
        match Subscription::create(
            (host.as_str(), port as u16),
            can::CAN_LISTENER,
            rcf_lite::client::DEFAULT_CONNECT_TIMEOUT,
            rcf_lite::client::DEFAULT_CALL_TIMEOUT,
            move |fn_id, a| me.on_event(&l, fn_id, a),
        ) {
            Ok(s) => {
                w.subscription = Some(s);
                logi!("can: subscribed to the CAN service's publisher ({}:{}) for the speed", host, port);
            }
            Err(e) => {
                if !w.subscribe_failing {
                    logw!("can: speed subscription on {}:{} failed: {}", host, port, errstr(&e));
                }
                w.subscribe_failing = true;
                return;
            }
        }
        w.subscribe_failing = false;
        if let Some(c) = w.client.as_mut()
            && let Err(e) = c.call(can::CAN_SERVICE_GET_CAN_VEHICLE_SPEED, &[]) {
                logw!("can: GetCanVehicleSpeed failed: {}", errstr(&e));
            }
    }

    fn run(self: Arc<Self>) {
        let mut w = Worker {
            client: None,
            subscription: None,
            subscribe_tried: None,
            subscribe_failing: false,
            listener: Arc::new(Mutex::new(Listener { last_info: -1, ..Default::default() })),
        };
        let (mut last, mut last_rhd, mut last_transmission, mut since_sent_s) = (-1, -1, -1, 0);
        let (mut logged, mut logged_rhd) = (-1, -1);
        let mut sent_to: u32 = 0; // ControlChannel::connections() when night and rhd last reached the receiver
        let mut failing = false;
        loop {
            {
                let g = self.stop.lock().unwrap();
                let (g, _) = self.cv.wait_timeout_while(g, Duration::from_secs(2), |s| !*s).unwrap();
                if *g {
                    break;
                }
            }
            let r = (|| -> io::Result<()> {
                let client = w.client.get_or_insert_with(|| {
                    let mut c = Client::new(&self.cfg.can_host, self.cfg.can_port as u16, can::CAN_SERVICE);
                    c.set_connect_timeout(Duration::from_millis(2000));
                    c.set_call_timeout(Duration::from_millis(2000));
                    c
                });
                let body = client.call(can::CAN_SERVICE_GET_CAN_CARPLAY_INFO, &args(|a| {
                    a.put(&CanCarplayInfo::default());
                }))?;
                let info: CanCarplayInfo = Reader::new(&body).get()?;
                if failing {
                    logi!("can: the CAN service answers again");
                }
                failing = false;
                let night = (info.day_night != 0) as i32;
                let rhd = (info.right_hand_drive != 0) as i32;
                self.night.store(night, Ordering::SeqCst);
                self.rhd.store(rhd, Ordering::SeqCst);
                since_sent_s += 2;
                if night != logged || rhd != logged_rhd {
                    logi!(
                        "can: DayNight {} (transmission {}, right-hand drive {}) -> {}",
                        info.day_night,
                        info.transmission_state,
                        info.right_hand_drive,
                        if night != 0 { "night" } else { "day" }
                    );
                    logged = night;
                    logged_rhd = rhd;
                }
                // Each receiver connection gets them at once (it reads the driver's side at the
                // next session's service discovery, and starts left-hand), then on a change and
                // every 30 s; they count as sent only once the receiver had them.
                let conn = self.ctl.connections();
                if (night != last || rhd != last_rhd || conn != sent_to || since_sent_s >= 30)
                    && self.ctl.send(if night != 0 { "night 1" } else { "night 0" })
                        && self.ctl.send(if rhd != 0 { "rhd 1" } else { "rhd 0" })
                    {
                        last = night;
                        last_rhd = rhd;
                        sent_to = conn;
                        since_sent_s = 0;
                    }
                let gear = gear_name(info.transmission_state);
                if info.transmission_state as i32 != last_transmission {
                    logi!("can: transmission {} -> {}", info.transmission_state, gear.unwrap_or("unknown, not sent"));
                    last_transmission = info.transmission_state as i32;
                }
                if let Some(g) = gear {
                    self.ctl.send(&format!("gear {g}"));
                }
                Ok(())
            })();
            match r {
                Ok(()) => {
                    self.subscribe(&mut w);
                    // Resend a speed that is still current (a new receiver or session gets it).
                    let mut sp = self.speed.lock().unwrap();
                    if sp.seen && sp.at.is_some_and(|t| t.elapsed() < Duration::from_secs(5)) {
                        self.maybe_send_speed(&mut sp, Instant::now());
                    }
                }
                Err(e) => {
                    if !failing {
                        logw!(
                            "can: GetCanCarplayInfo on {}:{} failed: {}",
                            self.cfg.can_host,
                            self.cfg.can_port,
                            errstr(&e)
                        );
                    }
                    failing = true;
                    w.client = None;
                }
            }
        }
        if let Some(s) = w.subscription.take() {
            s.close();
        }
    }
}
