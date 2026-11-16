// SPDX-License-Identifier: GPL-3.0-or-later
//
// Android Auto's Bluetooth pairing with the car (bt_idl.hpp has btservice's side). The phone
// gets the car's address in service discovery and asks whether it is paired with it (the
// receiver's "btpair <address> <method>"). Answering "already paired" without asking made an
// unpaired phone connect to the car, and the unit's Bluetooth app then asked the user to pair
// (step 3 runs 30, 31). Now:
//   * paired: the car's paired list (GetDeviceNum / GetDevice of BT_DEVICE_PAIRED) has the
//     phone's address -> "btpaired 1": the phone connects for calls and the car knows it;
//   * not paired -> "btpaired 0", and for 60 s a pairing window: SetAutoAcceptConnect(1) (the
//     unit's own setting saved first and put back after), so btservice takes the phone's link
//     without OnRemoteConnectRequest and the app's "pairing request" dialog; the phone bonds,
//     btservice publishes OnPairingComfirmRequest(passkey, address): the passkey goes to the
//     phone ("btauth %06u", BluetoothAuthenticationRequest), which confirms by itself, and
//     AcceptSspRequest(1) confirms on the car. The app still opens its numeric dialog until
//     OnPairStatusChanged(0) closes it, so it flashes briefly.
// Every pairing and connection event btservice publishes is logged ("bt:"), so a run shows what
// the car's Bluetooth did. Calls to btservice run on one worker.
//
// Also the phone's Bluetooth music (A2DP) while Android Auto plays, as stock CarLife and CarPlay
// do: AA's media comes over USB, and a music link to the same (or another) phone could make the
// car switch to its Bluetooth audio source. btservice itself refuses new A2DP links while our
// status is 3 (OnHandleAuthoriseInd), but not one that was up before, nor its link-loss
// reconnect: when the session starts, the device holding A2DP (GetAvpConnectAddr) is
// remembered and Disconnect(A2DP) (HFP stays); an A2DP connect during the session is dropped
// again; 2 s after the session ends, Connect(that address, A2DP) puts it back (stock CarLife
// never did). AA_BT_A2DP_DROP=0 leaves A2DP alone.
//
// And the lock: while the session runs and its phone (the address of its btpair) holds the
// car's phone link (GetHfpConnectAddr), capability 10 is on: btservice refuses every other
// device's pairing and connection without asking anyone (bt_idl.hpp), so no other phone joins
// or takes over, and the Bluetooth app has nothing to ask about. Links already up stay up. It is
// looked at on each pairing or connection event, the btpair, the session's start and end, and
// every 10 s, and set again each time while it holds (the Bluetooth app clears it when a CarPlay
// session ends, and it cannot be read back). It comes off when that phone lets go of the link
// (so it can come back) or the session ends, and at the shim's stop; a file (AA_BT_LOCK_FILE)
// marks it as ours, so a shim that died holding it takes it off at its next start, and one set
// by the Bluetooth app for CarPlay is left alone. AA_BT_LOCK=0 never sets it.

use std::io::{self, Read};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex, Weak};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use rcf_lite::client::args;
use rcf_lite::{Client, Reader, Subscription};

use crate::config::{errstr, replace_file, tcp_reachable, Config};
use crate::control::ControlChannel;
use crate::idl::{self, bt, BtDevice};
use crate::log::printable;
use crate::{logi, logv, logw};

// Capability 10: every link key refused (bt_idl.hpp).
const CAP_AUTO_EJECT: i32 = 10;

struct State {
    stop: bool,
    restore_pending: bool, // the window is over; auto-accept not yet put back
    query: String,         // a btpair to answer
    query_method: i32,
    phone: String, // the phone being paired (a window is open)
    window_end: Instant,
    accept: bool,     // AcceptSspRequest(1) to send
    accept_pin: bool, // AcceptPinCodeRequest(pin) to send
    pin: String,      // PIN pairing: the PIN given to the phone
    watch_phone: String, // the session's phone (last btpair), for a re-bond
    watch_end: Instant,
    rebond: String,                   // a re-bond being confirmed (for the log)
    call_status: i32,                 // OnCallStatusChanged
    call_status_at: Option<Instant>,  // when it last changed (never: None)
    session: bool,                    // AA projecting
    session_changed: bool,
    drop_a2dp: bool,       // a Disconnect(A2DP) to send
    a2dp_addr: String,     // the device whose music link was dropped
    session_phone: String, // the session's phone (its btpair), lower case; "" none
    link_changed: bool,    // a link event, a btpair: the lock to look at
    a2dp_back_pending: bool, // Connect(a2dp_addr, A2DP) at a2dp_back_at
    a2dp_back_at: Instant,
}

// The worker's own.
#[derive(Default)]
struct Worker {
    saved_auto_accept: i32,
    locked: bool, // capability 10 set by us
    client: Option<Client>,
    subscription: Option<Subscription>,
    subscribe_tried: Option<Instant>,
    subscribe_failing: bool,
}

impl Worker {
    fn client(&mut self, cfg: &Config) -> &mut Client {
        self.client.get_or_insert_with(|| {
            let mut c = Client::new(&cfg.bt_host, cfg.bt_port as u16, bt::BT_SERVICE);
            c.set_connect_timeout(Duration::from_millis(2000));
            c.set_call_timeout(Duration::from_millis(3000));
            c
        })
    }
}

pub struct BtPairing {
    cfg: Arc<Config>,
    ctl: Arc<ControlChannel>,
    me: Weak<BtPairing>,
    enabled: bool, // the worker runs
    state: Mutex<State>,
    cv: Condvar,
    subscribed: AtomicBool,
    worker: Mutex<Option<JoinHandle<()>>>,
}

fn lower(s: &str) -> String {
    s.to_ascii_lowercase()
}

// OnPairingComfirmRequest's address: 6 raw bytes, most significant first, cut at the first 0.
fn hex_addr(raw: &[u8]) -> String {
    raw.iter().map(|b| format!("{b:02X}")).collect::<Vec<_>>().join(":")
}

// Six random digits (a PIN for legacy pairing).
fn random_pin() -> String {
    let mut b = [0u8; 4];
    let v = match std::fs::File::open("/dev/urandom").and_then(|mut f| f.read_exact(&mut b)) {
        Ok(()) => u32::from_ne_bytes(b),
        Err(_) => {
            let t = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs());
            (t as u32).wrapping_mul(2654435761)
        }
    };
    format!("{:06}", v % 1000000)
}

// Whether OnPairingComfirmRequest's raw address is `addr` (it may be cut at a 0 byte).
fn same_addr(raw: &[u8], addr: &str) -> bool {
    let h = lower(&hex_addr(raw));
    !h.is_empty() && lower(addr).starts_with(&h)
}

// A string event argument for a log line, as %s prints it.
fn text(b: &[u8]) -> String {
    String::from_utf8_lossy(b).into_owned()
}

impl BtPairing {
    /// Off with AA_BT_PAIRING=0. The method and event IDs are the unit's own (bt_idl.hpp).
    pub fn new(cfg: Arc<Config>, ctl: Arc<ControlChannel>) -> Arc<Self> {
        let enabled =
            cfg.bt_port > 0 && cfg.bt_listen_port > 0 && (cfg.bt_pairing || cfg.bt_a2dp_drop || cfg.bt_lock);
        let now = Instant::now();
        let me = Arc::new_cyclic(|me| BtPairing {
            cfg,
            ctl,
            me: me.clone(),
            enabled,
            state: Mutex::new(State {
                stop: false,
                restore_pending: false,
                query: String::new(),
                query_method: 0,
                phone: String::new(),
                window_end: now,
                accept: false,
                accept_pin: false,
                pin: String::new(),
                watch_phone: String::new(),
                watch_end: now,
                rebond: String::new(),
                call_status: 0,
                call_status_at: None,
                session: false,
                session_changed: false,
                drop_a2dp: false,
                a2dp_addr: String::new(),
                session_phone: String::new(),
                link_changed: false,
                a2dp_back_pending: false,
                a2dp_back_at: now,
            }),
            cv: Condvar::new(),
            subscribed: AtomicBool::new(false),
            worker: Mutex::new(None),
        });
        if enabled {
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

    /// The AA session is projecting (on) or over (off).
    pub fn session(&self, on: bool) {
        if !self.enabled {
            return;
        }
        {
            let mut st = self.state.lock().unwrap();
            if st.session == on {
                return;
            }
            st.session = on;
            st.session_changed = true;
            if !on {
                st.session_phone.clear();
            }
        }
        self.cv.notify_all();
    }

    /// The receiver's "btpair <address> <method>": answered with "btpaired 0|1". The address is
    /// also the session's phone, for the lock.
    pub fn query(&self, address: &str, method: i32) {
        if self.enabled {
            {
                let mut st = self.state.lock().unwrap();
                st.session_phone = lower(address);
                st.link_changed = true;
            }
            self.cv.notify_all();
        }
        if !self.enabled || !self.cfg.bt_pairing {
            logi!("bt: pairing loop off: phone {} answered \"already paired\" (as before)", address);
            self.ctl.send("btpaired 1");
            return;
        }
        {
            let mut st = self.state.lock().unwrap();
            st.query = address.to_string();
            st.query_method = method;
            // Whatever the answer, the session's phone may bond now: it asks this every session,
            // even when both sides are paired, and when it has forgotten the car while the car
            // still lists it, the answer is "paired" and then it bonds anyway (step 3 run 38:
            // confirm requests left to the Bluetooth app, which failed every time).
            st.watch_phone = address.to_string();
            st.watch_end = Instant::now() + Duration::from_secs(60);
        }
        self.cv.notify_all();
    }

    /// Whether a call status event that starts an outgoing call is about to be, or was just,
    /// handled: no call for a second (its event not here yet) or dialing for under 600 ms. False
    /// when unknown (no events), while a call runs, as one ends, or for an incoming call.
    pub fn outgoing_call_starting(&self) -> bool {
        if !self.subscribed.load(Ordering::SeqCst) {
            return false;
        }
        let st = self.state.lock().unwrap();
        let since = st.call_status_at.map(|t| t.elapsed());
        if st.call_status == 0 {
            return since.is_none_or(|s| s >= Duration::from_secs(1));
        }
        st.call_status == 1 && since.is_some_and(|s| s < Duration::from_millis(600))
    }

    // On the subscription's thread: one SVP_BtListener event. Every event has a method on the
    // C++ servant (SVPBtListenerNoop); these are the ones the loop logs or uses.
    fn on_event(&self, fn_id: i32, a: &[u8]) {
        let mut r = Reader::new(a);
        match fn_id {
            bt::BT_LISTENER_ON_PAIRING_COMFIRM_REQUEST => {
                if let (Ok(passkey), Ok(raw)) = (r.get::<u32>(), r.get::<Vec<u8>>()) {
                    self.on_confirm(passkey, &raw);
                }
            }
            bt::BT_LISTENER_ON_PAIRING_PIN_CODE_REQUEST => {
                if let (Ok(a), Ok(b), Ok(c)) = (r.get::<u32>(), r.get::<Vec<u8>>(), r.get::<Vec<u8>>()) {
                    self.on_pin_request(a, &b, &c);
                }
            }
            bt::BT_LISTENER_ON_REMOTE_CONNECT_REQUEST => {
                if let Ok(addr) = r.get::<Vec<u8>>() {
                    logi!("bt: event RemoteConnectRequest({})", text(&addr));
                }
            }
            bt::BT_LISTENER_ON_RETURN_REMOTE_DEVICE_NAME => {
                if let (Ok(addr), Ok(name)) = (r.get::<Vec<u8>>(), r.get::<Vec<u8>>()) {
                    logi!("bt: event RemoteDeviceName({}, \"{}\")", printable(&addr), printable(&name));
                }
            }
            bt::BT_LISTENER_ON_PAIR_STATUS_CHANGED => {
                if let (Ok(st), Ok(addr)) = (r.get::<i32>(), r.get::<Vec<u8>>()) {
                    self.on_pair_status(st, &text(&addr));
                }
            }
            bt::BT_LISTENER_ON_DEVICE_NEED_PAIR => {
                if let Ok(addr) = r.get::<Vec<u8>>() {
                    logi!("bt: event DeviceNeedPair({})", text(&addr));
                }
            }
            bt::BT_LISTENER_ON_PAIR_LIST_CHANGED => logi!("bt: event PairListChanged"),
            bt::BT_LISTENER_ON_CALL_STATUS_CHANGED => {
                if let Ok(st) = r.get::<i32>() {
                    self.on_call_status(st);
                }
            }
            bt::BT_LISTENER_ON_CONNECT_STATUS_CHANGED => {
                if let (Ok(p), Ok(v), Ok(addr)) = (r.get::<i32>(), r.get::<i32>(), r.get::<Vec<u8>>()) {
                    let addr = text(&addr);
                    logi!("bt: event ConnectStatusChanged(profile {}, {}, {})", p, v, addr);
                    if p == 2 && v == 1 {
                        self.on_a2dp_connected(&addr);
                    }
                    self.on_link();
                }
            }
            bt::BT_LISTENER_ON_DISCONNECT_STATUS_CHANGED => {
                if let (Ok(p), Ok(v)) = (r.get::<i32>(), r.get::<i32>()) {
                    logi!("bt: event DisconnectStatusChanged(profile {}, {})", p, v);
                    self.on_link();
                }
            }
            _ => {}
        }
    }

    // On the subscription's thread. Only the phone that asked (btpair) is confirmed here: while
    // a window is open, btservice takes any device's connection (auto-accept), and
    // AcceptSspRequest answers whichever request came last (bt_idl.hpp), so another device's
    // request is left to the Bluetooth app and its dialog. The key is logged at debug only.
    fn on_confirm(&self, passkey: u32, raw: &[u8]) {
        let mut st = self.state.lock().unwrap();
        let now = Instant::now();
        let window = !st.phone.is_empty() && now < st.window_end && same_addr(raw, &st.phone);
        // The session's phone bonding again although the car lists it: it lost its pairing.
        let rebond = !window && !st.watch_phone.is_empty() && now < st.watch_end && same_addr(raw, &st.watch_phone);
        let ours = window || rebond;
        logv!("bt: pairing key {:06}", passkey);
        logi!(
            "bt: event PairingComfirmRequest({}){}",
            hex_addr(raw),
            if window {
                " -> Android Auto's pairing: key to the phone, accepted here"
            } else if rebond {
                " -> the session's phone pairs again (it forgot the car): key to the phone, accepted here"
            } else {
                " (not ours: left to the Bluetooth app)"
            }
        );
        if !ours {
            return;
        }
        if rebond {
            st.rebond = st.watch_phone.clone();
        }
        self.ctl.send(&format!("btauth {passkey:06}"));
        st.accept = true;
        self.cv.notify_all();
    }

    // On the subscription's thread: the calls' combined status (0 none, 1 one outgoing dialing
    // or ringing out, 4 one incoming ringing; btservice 0x15395c, 0x1535d0).
    fn on_call_status(&self, s: i32) {
        let mut st = self.state.lock().unwrap();
        if s == st.call_status {
            return;
        }
        logi!("bt: event CallStatus {}", s);
        st.call_status = s;
        st.call_status_at = Some(Instant::now());
    }

    // On the subscription's thread: a link came up or went down (the lock is looked at again).
    fn on_link(&self) {
        self.state.lock().unwrap().link_changed = true;
        self.cv.notify_all();
    }

    // On the subscription's thread: a music link (A2DP) came up.
    fn on_a2dp_connected(&self, addr: &str) {
        if !self.cfg.bt_a2dp_drop {
            return;
        }
        let mut st = self.state.lock().unwrap();
        if !st.session {
            return;
        }
        st.a2dp_addr = lower(addr);
        st.drop_a2dp = true;
        self.cv.notify_all();
    }

    // On the subscription's thread: the car's Bluetooth wants a PIN (legacy pairing; the phone
    // asked for BLUETOOTH_PAIRING_METHOD_PIN). The phone already has the PIN ("btauth").
    fn on_pin_request(&self, a: u32, b: &[u8], c: &[u8]) {
        let mut st = self.state.lock().unwrap();
        let ours = !st.phone.is_empty() && Instant::now() < st.window_end && !st.pin.is_empty() && same_addr(b, &st.phone);
        logi!(
            "bt: event PinCodeRequest({}, {}, {}){}",
            a,
            hex_addr(b),
            printable(c),
            if ours { " -> Android Auto's pairing: our PIN" } else { " (not ours: left to the Bluetooth app)" }
        );
        if !ours {
            return;
        }
        st.accept_pin = true;
        self.cv.notify_all();
    }

    fn on_pair_status(&self, status: i32, addr: &str) {
        let mut st = self.state.lock().unwrap();
        logi!("bt: event PairStatusChanged({}, {})", status, addr);
        if !st.rebond.is_empty() {
            logi!("bt: {} paired again {}", st.rebond, if status == 0 { "(done)" } else { "failed" });
            st.rebond.clear();
        }
        if st.phone.is_empty() {
            return;
        }
        if status == 0 && lower(addr) != lower(&st.phone) {
            return;
        }
        logi!("bt: Android Auto pairing with {} {}", st.phone, if status == 0 { "done" } else { "failed" });
        st.window_end = Instant::now(); // close the window (the worker restores the setting)
        self.cv.notify_all();
    }

    // Worker: the lock on while the session runs and its phone holds the phone link, else off;
    // set again while it holds.
    fn update_lock(&self, w: &mut Worker, session: bool, phone: &str) -> io::Result<()> {
        let mut want = false;
        let mut hfp = String::new();
        if session && !phone.is_empty() {
            let body = w.client(&self.cfg).call(bt::BT_SERVICE_GET_HFP_CONNECT_ADDR, &args(|a| {
                a.put(&String::new());
            }))?;
            let mut rd = Reader::new(&body);
            let (r, h): (i32, String) = (rd.get()?, rd.get()?);
            hfp = if r == 1 { h } else { String::new() };
            want = !hfp.is_empty() && lower(&hfp) == phone;
        }
        if !want && !w.locked {
            return Ok(());
        }
        let r = set_capability(w.client(&self.cfg), CAP_AUTO_EJECT, want as i32)?;
        if want == w.locked {
            return Ok(()); // set again
        }
        w.locked = want;
        if !self.cfg.bt_lock_file.is_empty() {
            if want {
                let err = replace_file(&self.cfg.bt_lock_file, "10\n");
                if !err.is_empty() {
                    logw!("bt: {}: {}", self.cfg.bt_lock_file, err);
                }
            } else {
                let _ = std::fs::remove_file(&self.cfg.bt_lock_file);
            }
        }
        if want {
            logi!(
                "bt: Android Auto's phone {} holds the phone link: no other device may pair or connect \
                 (SetCapability(10, 1) -> {})",
                phone,
                r
            );
        } else {
            let why = if !session {
                "the session is over".to_string()
            } else if hfp.is_empty() {
                "Android Auto's phone let go of the phone link".to_string()
            } else {
                format!("the phone link is {hfp}'s")
            };
            logi!("bt: other devices may pair and connect again: {} (SetCapability(10, 0) -> {})", why, r);
        }
        Ok(())
    }

    // Worker: whether the car's paired list has the address.
    fn is_paired(&self, w: &mut Worker, address: &str) -> io::Result<bool> {
        let c = w.client(&self.cfg);
        let body = c.call(bt::BT_SERVICE_GET_DEVICE_NUM, &args(|a| {
            a.put(&idl::BT_DEVICE_PAIRED);
        }))?;
        let n: i32 = Reader::new(&body).get()?;
        let mut list = String::new();
        let mut found = false;
        let mut i = 0;
        while i < n && i < 32 {
            let body = c.call(bt::BT_SERVICE_GET_DEVICE, &args(|a| {
                a.put(&i).put(&BtDevice::default()).put(&idl::BT_DEVICE_PAIRED);
            }))?;
            let mut rd = Reader::new(&body);
            let (r, d): (i32, BtDevice) = (rd.get()?, rd.get()?);
            i += 1;
            if r != 1 && d.address.is_empty() {
                continue;
            }
            if !list.is_empty() {
                list.push_str(", ");
            }
            list.push_str(&d.address);
            if lower(&d.address) == lower(address) {
                found = true;
            }
        }
        logi!("bt: paired with the car: {} ({})", n, list);
        Ok(found)
    }

    fn subscribe(&self, w: &mut Worker) {
        if w.subscription.as_ref().is_some_and(|s| s.is_connected()) {
            return;
        }
        let now = Instant::now();
        if w.subscribe_tried.is_some_and(|t| now - t < Duration::from_secs(10)) {
            return;
        }
        w.subscribe_tried = Some(now);
        let (host, port) = (&self.cfg.bt_host, self.cfg.bt_listen_port);
        if !tcp_reachable(host, port, 1000) {
            if !w.subscribe_failing {
                logw!("bt: no Bluetooth event publisher on {}:{}", host, port);
            }
            w.subscribe_failing = true;
            return;
        }
        if let Some(s) = w.subscription.take() {
            logw!("bt: event subscription lost; subscribing again");
            s.close();
        }
        let me = self.me.clone();
        match Subscription::create(
            (host.as_str(), port as u16),
            bt::BT_LISTENER,
            rcf_lite::client::DEFAULT_CONNECT_TIMEOUT,
            rcf_lite::client::DEFAULT_CALL_TIMEOUT,
            move |fn_id, a| {
                if let Some(s) = me.upgrade() {
                    s.on_event(fn_id, a);
                }
            },
        ) {
            Ok(s) => {
                w.subscription = Some(s);
                self.subscribed.store(true, Ordering::SeqCst);
                logi!("bt: subscribed to the Bluetooth events ({}:{})", host, port);
                w.subscribe_failing = false;
            }
            Err(e) => {
                if !w.subscribe_failing {
                    logw!("bt: event subscription failed: {}", errstr(&e));
                }
                w.subscribe_failing = true;
            }
        }
    }

    fn run(&self) {
        let mut w = Worker::default();
        // A shim that died with a pairing window open left the unit's auto-accept on; its boot
        // default is off and no stock program sets it (bt_idl.hpp).
        if self.cfg.bt_pairing {
            match set_auto_accept(w.client(&self.cfg), 0) {
                Ok(()) => logi!("bt: SetAutoAcceptConnect(0) (the unit's default)"),
                Err(e) => {
                    logw!("bt: SetAutoAcceptConnect(0) failed: {}", errstr(&e));
                    w.client = None;
                }
            }
        }
        // A shim that died holding the lock left the unit refusing every device; one that the
        // Bluetooth app set for CarPlay has no file and is left alone.
        if !self.cfg.bt_lock_file.is_empty() && std::path::Path::new(&self.cfg.bt_lock_file).exists() {
            match set_capability(w.client(&self.cfg), CAP_AUTO_EJECT, 0) {
                Ok(r) => {
                    let _ = std::fs::remove_file(&self.cfg.bt_lock_file);
                    logi!("bt: the lock of an earlier run taken off (SetCapability(10, 0) -> {})", r);
                }
                Err(e) => {
                    logw!("bt: taking off an earlier run's lock failed: {}", errstr(&e));
                    w.client = None;
                }
            }
        }
        loop {
            let (query, method, accept, restore, session_changed, session, drop_a2dp, a2dp_back);
            let (accept_pin, a2dp_addr, session_phone);
            {
                let mut st = self.state.lock().unwrap();
                // Wake for work, the window's end, the music link's return, and every 10 s to
                // keep the subscription up.
                let mut until = Instant::now() + Duration::from_secs(10);
                if !st.phone.is_empty() && st.window_end < until {
                    until = st.window_end;
                }
                if st.a2dp_back_pending && st.a2dp_back_at < until {
                    until = st.a2dp_back_at;
                }
                let pred = |s: &State| {
                    let now = Instant::now();
                    s.stop
                        || !s.query.is_empty()
                        || s.accept
                        || s.accept_pin
                        || s.session_changed
                        || s.drop_a2dp
                        || s.link_changed
                        || (!s.phone.is_empty() && now >= s.window_end)
                        || (s.a2dp_back_pending && now >= s.a2dp_back_at)
                };
                st = self
                    .cv
                    .wait_timeout_while(st, until.saturating_duration_since(Instant::now()), |s| !pred(s))
                    .unwrap()
                    .0;
                session_changed = st.session_changed;
                st.session_changed = false;
                st.link_changed = false;
                session = st.session;
                session_phone = st.session_phone.clone();
                drop_a2dp = st.drop_a2dp;
                st.drop_a2dp = false;
                a2dp_back = st.a2dp_back_pending && Instant::now() >= st.a2dp_back_at;
                if a2dp_back {
                    st.a2dp_back_pending = false;
                }
                if session_changed && session {
                    st.a2dp_back_pending = false; // a new session before it returned
                }
                a2dp_addr = st.a2dp_addr.clone();
                if st.stop {
                    break;
                }
                query = std::mem::take(&mut st.query);
                method = st.query_method;
                accept = st.accept;
                st.accept = false;
                accept_pin = if st.accept_pin { st.pin.clone() } else { String::new() };
                st.accept_pin = false;
                if !st.phone.is_empty() && Instant::now() >= st.window_end {
                    st.phone.clear();
                    st.restore_pending = true;
                }
                restore = st.restore_pending;
            }
            let r = self.work(
                &mut w,
                Work {
                    query,
                    method,
                    accept,
                    restore,
                    session_changed,
                    session,
                    drop_a2dp,
                    a2dp_back,
                    accept_pin,
                    a2dp_addr,
                    session_phone,
                },
            );
            if let Err(e) = r {
                logw!("bt: pairing call failed: {}", errstr(&e));
                w.client = None;
            }
            self.subscribe(&mut w);
        }
        // Put the unit's setting back if a window is still open, or its restore failed, and the
        // phone's music link if a session ended (or was ended by the shutdown) without it.
        let (open, a2dp_addr) = {
            let st = self.state.lock().unwrap();
            // dropped for a session, not yet given back
            (!st.phone.is_empty() || st.restore_pending, if !st.session { st.a2dp_addr.clone() } else { String::new() })
        };
        let r = (|| -> io::Result<()> {
            if open {
                let saved = w.saved_auto_accept;
                set_auto_accept(w.client(&self.cfg), saved)?;
            }
            if w.locked {
                let r = set_capability(w.client(&self.cfg), CAP_AUTO_EJECT, 0)?;
                w.locked = false;
                if !self.cfg.bt_lock_file.is_empty() {
                    let _ = std::fs::remove_file(&self.cfg.bt_lock_file);
                }
                logi!("bt: stopping: other devices may pair and connect again (SetCapability(10, 0) -> {})", r);
            }
            if !a2dp_addr.is_empty() && self.cfg.bt_a2dp_drop {
                connect(w.client(&self.cfg), &a2dp_addr)?;
                logi!("bt: stopping: Bluetooth music (A2DP) back to {}", a2dp_addr);
            }
            Ok(())
        })();
        if let Err(e) = r {
            logw!("bt: closing: {}", errstr(&e));
        }
        if let Some(s) = w.subscription.take() {
            s.close();
        }
    }

    // Worker: what one wake-up asks for.
    fn work(&self, w: &mut Worker, k: Work) -> io::Result<()> {
        // First, and until it succeeds: a failed call below must not leave the unit accepting
        // connections from any device.
        if k.restore {
            let saved = w.saved_auto_accept;
            set_auto_accept(w.client(&self.cfg), saved)?;
            self.state.lock().unwrap().restore_pending = false;
            logi!("bt: pairing window over; SetAutoAcceptConnect({}) (as it was)", w.saved_auto_accept);
        }
        if k.session_changed && k.session && self.cfg.bt_a2dp_drop {
            let body = w.client(&self.cfg).call(bt::BT_SERVICE_GET_AVP_CONNECT_ADDR, &args(|a| {
                a.put(&String::new());
            }))?;
            let mut rd = Reader::new(&body);
            let (r, addr): (i32, String) = (rd.get()?, rd.get()?);
            if r == 1 && addr.len() == 17 {
                self.state.lock().unwrap().a2dp_addr = lower(&addr);
                disconnect_a2dp(w.client(&self.cfg))?;
                logi!("bt: Bluetooth music (A2DP) of {} off for the session", addr);
            } else {
                logi!("bt: no Bluetooth music (A2DP) link at the session's start");
            }
        }
        if k.session_changed && !k.session {
            let mut st = self.state.lock().unwrap();
            if !st.a2dp_addr.is_empty() {
                st.a2dp_back_pending = true;
                st.a2dp_back_at = Instant::now() + Duration::from_secs(2);
            }
        }
        if k.drop_a2dp && k.session {
            disconnect_a2dp(w.client(&self.cfg))?;
            logi!("bt: Bluetooth music (A2DP) of {} connected during the session: off again", k.a2dp_addr);
        }
        if k.a2dp_back && !k.session && !k.a2dp_addr.is_empty() {
            connect(w.client(&self.cfg), &k.a2dp_addr)?;
            logi!("bt: session over: Bluetooth music (A2DP) back to {}", k.a2dp_addr);
            self.state.lock().unwrap().a2dp_addr.clear();
        }
        if k.accept {
            w.client(&self.cfg).call(bt::BT_SERVICE_ACCEPT_SSP_REQUEST, &args(|a| {
                a.put(&1i32);
            }))?;
            logi!("bt: AcceptSspRequest(1)");
        }
        if !k.accept_pin.is_empty() {
            w.client(&self.cfg).call(bt::BT_SERVICE_ACCEPT_PIN_CODE_REQUEST, &args(|a| {
                a.put(&k.accept_pin);
            }))?;
            logi!("bt: AcceptPinCodeRequest(the PIN)");
        }
        if !k.query.is_empty() {
            let paired = self.is_paired(w, &k.query)?;
            logi!(
                "bt: phone {} (method {}) {} the car",
                k.query,
                k.method,
                if paired { "is paired with" } else { "is not paired with" }
            );
            self.ctl.send(if paired { "btpaired 1" } else { "btpaired 0" });
            if !paired {
                let open = !self.state.lock().unwrap().phone.is_empty();
                if !open {
                    let body = w.client(&self.cfg).call(bt::BT_SERVICE_GET_AUTO_ACCEPT_CONNECT, &[])?;
                    w.saved_auto_accept = Reader::new(&body).get()?;
                }
                set_auto_accept(w.client(&self.cfg), 1)?;
                let mut st = self.state.lock().unwrap();
                st.phone = k.query.clone();
                st.window_end = Instant::now() + Duration::from_secs(60);
                logi!(
                    "bt: pairing window for {}: 60 s, SetAutoAcceptConnect(1) (was {})",
                    k.query,
                    w.saved_auto_accept
                );
                // PIN pairing (method 4): the phone gets the PIN now and bonds with it; the car's
                // PIN request is answered with the same one (on_pin_request). Numeric comparison
                // (2): the key comes from the car's confirm request (on_confirm).
                st.pin.clear();
                if k.method == 4 {
                    st.pin = random_pin();
                    self.ctl.send(&format!("btauth {}", st.pin));
                    logi!("bt: PIN pairing: a PIN to the phone");
                    logv!("bt: the PIN is {}", st.pin);
                }
            }
        }
        if self.cfg.bt_lock {
            self.update_lock(w, k.session, &k.session_phone)?;
        }
        Ok(())
    }
}

struct Work {
    query: String,
    method: i32,
    accept: bool,
    restore: bool,
    session_changed: bool,
    session: bool,
    drop_a2dp: bool,
    a2dp_back: bool,
    accept_pin: String,
    a2dp_addr: String,
    session_phone: String,
}

fn set_auto_accept(c: &mut Client, on: i32) -> io::Result<()> {
    c.call(bt::BT_SERVICE_SET_AUTO_ACCEPT_CONNECT, &args(|a| {
        a.put(&on);
    }))
    .map(|_| ())
}

fn set_capability(c: &mut Client, cap: i32, on: i32) -> io::Result<i32> {
    let body = c.call(bt::BT_SERVICE_SET_CAPABILITY, &args(|a| {
        a.put(&cap).put(&on);
    }))?;
    Reader::new(&body).get()
}

fn disconnect_a2dp(c: &mut Client) -> io::Result<()> {
    c.call(bt::BT_SERVICE_DISCONNECT, &args(|a| {
        a.put(&idl::BT_SERVICE_A2DP);
    }))
    .map(|_| ())
}

fn connect(c: &mut Client, addr: &str) -> io::Result<()> {
    c.call(bt::BT_SERVICE_CONNECT, &args(|a| {
        a.put(&addr.to_string()).put(&idl::BT_SERVICE_A2DP);
    }))
    .map(|_| ())
}
