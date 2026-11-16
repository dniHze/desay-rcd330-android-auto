// SPDX-License-Identifier: GPL-3.0-or-later
//
// The servant of ISVP_CarLifeService and the publisher of ISVP_CarLifeListener: the state of
// the CarLife slot, driven by appconnect's calls and the receiver's lines, and every part the
// shim runs (video surface, audio sources, CAN data, mic path, Bluetooth, bridge notify, power,
// appconnect patch).

use std::io;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, Weak};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use rcf_lite::client::args;
use rcf_lite::{Publisher, Reader, Service, Writer};

use crate::appc::AppcWatch;
use crate::audio::{self, AudioSource};
use crate::bridge::BridgeNotify;
use crate::btinfo::BtInfo;
use crate::btpairing::BtPairing;
use crate::carinfo::CarInfo;
use crate::config::{replace_file, status_name, touch_name, Config};
use crate::control::ControlChannel;
use crate::idl::{self, carlife, CL_ERR_FAILED, CL_ERR_NONE, CL_ST_CONNECTING, CL_ST_CONNECT_FAILED, CL_ST_DISCONNECTED,
                 CL_ST_NONE, CL_ST_TIMEOUT, CL_ST_VIDEO};
use crate::lastmode::LastMode;
use crate::mic::MicPath;
use crate::power::PowerWatch;
use crate::scan::{prefixed_int, Scan};
use crate::video::VideoSurface;
use crate::{logi, logv, logw};

const MODULE_VR: u32 = 4; // CarLife module 4: voice recognition
// A late subscriber's announcement (subscriber_joined).
const ANNOUNCE_DELAY: Duration = Duration::from_millis(500);

struct StatusState {
    status: i32,
    status_once: bool, // GetCarLifeConnectStatus: answer 2 once
    status_once_until: Instant,
    announce: bool,
    announce_at: Instant,
}

struct OnTop {
    on_top: i32, // what AA_ONTOP_FILE says (-1: not written yet)
    failed: bool,
}

struct WorkerState {
    handle: Option<JoinHandle<()>>,
    connecting: bool,
}

pub struct CarLifeShim {
    cfg: Arc<Config>,
    ctl: Arc<ControlChannel>,
    me: Weak<CarLifeShim>,
    video: Arc<VideoSurface>,
    last: Arc<LastMode>, // before audio, which reports to it
    audio: Arc<AudioSource>,
    car: Arc<CarInfo>,
    mic: Arc<MicPath>,
    bt: Arc<BtInfo>,
    pair: Arc<BtPairing>,
    notify: Arc<BridgeNotify>, // after video, which its "video" datagrams show again
    power: Arc<PowerWatch>,
    appc: Arc<AppcWatch>,
    home_on_show: AtomicBool, // the next Show(1) sends "home"
    shown: AtomicBool,        // the last Show(n): AA on screen
    on_top: Mutex<OnTop>,
    boots_cleared: AtomicBool, // the boot-loop counter cleared at this boot's switch-off
    voice: AtomicU32,          // CarLife module 4 (voice) status: 1 while the phone has the mic
    status: Mutex<StatusState>,
    cv: Condvar,
    publisher: Mutex<Option<Arc<Publisher>>>, // mPubMutex and mPub
    worker: Mutex<WorkerState>,
    stopping: AtomicBool, // set with `status` held
    announcer: Mutex<Option<JoinHandle<()>>>,
}

impl CarLifeShim {
    pub fn new(cfg: Arc<Config>, ctl: Arc<ControlChannel>) -> Arc<Self> {
        let now = Instant::now();
        let shim = Arc::new_cyclic(|me: &Weak<CarLifeShim>| {
            let video = VideoSurface::new(cfg.clone());
            let last = LastMode::new(&cfg);
            let audio = AudioSource::new(cfg.clone(), ctl.clone(), last.clone());
            let car = CarInfo::new(cfg.clone(), ctl.clone());
            let mic = MicPath::new(cfg.clone());
            let bt = BtInfo::new(cfg.clone(), ctl.clone());
            let pair = BtPairing::new(cfg.clone(), ctl.clone());
            let v = video.clone();
            let notify = BridgeNotify::new(cfg.clone(), Box::new(move || v.new_session()));
            let w = me.clone();
            let power = PowerWatch::new(
                cfg.clone(),
                Box::new(move |s, a, b| {
                    if let Some(shim) = w.upgrade() {
                        shim.on_power(s, a, b);
                    }
                }),
            );
            let appc = AppcWatch::new(cfg.clone());
            CarLifeShim {
                cfg: cfg.clone(),
                ctl: ctl.clone(),
                me: me.clone(),
                video,
                last,
                audio,
                car,
                mic,
                bt,
                pair,
                notify,
                power,
                appc,
                home_on_show: AtomicBool::new(false),
                shown: AtomicBool::new(false),
                on_top: Mutex::new(OnTop { on_top: -1, failed: false }),
                boots_cleared: AtomicBool::new(false),
                voice: AtomicU32::new(0),
                status: Mutex::new(StatusState {
                    status: CL_ST_NONE,
                    status_once: false,
                    status_once_until: now,
                    announce: false,
                    announce_at: now,
                }),
                cv: Condvar::new(),
                publisher: Mutex::new(None),
                worker: Mutex::new(WorkerState { handle: None, connecting: false }),
                stopping: AtomicBool::new(false),
                announcer: Mutex::new(None),
            }
        });
        shim.power.start(); // after every part: its events call on_power
        shim.appc.start();
        let a = shim.clone();
        *shim.announcer.lock().unwrap() = Some(std::thread::spawn(move || a.run_announcer()));
        shim.write_on_top(0);
        shim
    }

    /// What the C++ shim's destructor did: AA off screen, the bring-up and announcer threads
    /// joined, then every part stopped, in the reverse order of their making.
    pub fn shutdown(&self) {
        self.write_on_top(0);
        {
            let _g = self.status.lock().unwrap();
            self.stopping.store(true, Ordering::SeqCst);
        }
        self.cv.notify_all();
        let worker = self.worker.lock().unwrap().handle.take();
        if let Some(t) = worker {
            let _ = t.join();
        }
        if let Some(t) = self.announcer.lock().unwrap().take() {
            let _ = t.join();
        }
        self.appc.stop();
        self.power.stop();
        self.notify.stop();
        self.pair.stop();
        self.bt.stop();
        self.mic.stop();
        self.car.stop();
        self.audio.stop();
        self.last.stop();
        self.video.stop();
    }

    pub fn set_publisher(&self, p: Option<Arc<Publisher>>) {
        *self.publisher.lock().unwrap() = p;
    }

    /// A client subscribed to our status events (OnSubscriberConnect, on the publisher's
    /// thread, before it is added: the announcement must come later, and never from here).
    /// appconnect subscribes once, when it starts, and shows Android Auto only on the 2 -> 3
    /// events, never from GetCarLifeConnectStatus (step 3 run 46). With a phone attached at
    /// boot the session can reach 2 or 3 before appconnect starts, so a newcomer hears the
    /// running session announced again (announce_session).
    pub fn subscriber_joined(&self) {
        logi!("a client subscribed to the status events");
        {
            let mut st = self.status.lock().unwrap();
            st.announce_at = Instant::now() + ANNOUNCE_DELAY;
            st.announce = true;
        }
        self.cv.notify_all();
    }

    /// SIGTERM: a running session ends as the user's Disconnect ends it, while the publisher is
    /// still up (appconnect hears (4, 0): after a restart it would hear nothing, being
    /// subscribed to this process only), then the sources and the mic path are given back,
    /// within `ms`. The receiver gets "stop" and the phone's music link comes back as the
    /// Bluetooth worker stops.
    pub fn end_session(&self, ms: u64) {
        {
            let _g = self.status.lock().unwrap();
            self.stopping.store(true, Ordering::SeqCst); // no bring-up or announcement goes on from here
        }
        self.cv.notify_all();
        let st = self.current_status();
        if st == CL_ST_CONNECTING || st == CL_ST_VIDEO {
            logi!("stopping: ending the session (status {})", st);
            self.ctl.send("stop");
            self.set_status(CL_ST_DISCONNECTED, CL_ERR_NONE);
        }
        self.set_voice(false);
        let until = Instant::now() + Duration::from_millis(ms);
        while !(self.audio.idle() && self.mic.idle()) {
            if Instant::now() >= until {
                logw!("stopping: the audio sources or the mic path not given back within {} ms", ms);
                break;
            }
            std::thread::sleep(Duration::from_millis(50));
        }
    }

    // ---- ISVP_CarLifeService, in wire order ----------------------------------------------

    fn play(&self, n: i32) -> i32 {
        logi!("Play({})", n);
        self.ctl.send("key play");
        0
    }

    fn pause(&self, n: i32) -> i32 {
        logi!("Pause({})", n);
        self.ctl.send("key pause");
        0
    }

    fn show(&self, n: i32) -> i32 {
        logi!("Show({})", n);
        self.shown.store(n != 0, Ordering::SeqCst);
        self.publish_on_top();
        self.video.show(n != 0);
        // Not the media source on every Show(1), as stock CarLife does: that switched the
        // speakers to a silent AA (the owner wants CarPlay's way: AA takes them when its media
        // plays, "audio media 1"). Only to resume what another source interrupted for a while;
        // never over a main source the user chose (the radio): see resume_if_interrupted.
        if n != 0 {
            self.audio.resume_if_interrupted();
        }
        if n != 0 {
            self.patch_appconnect(); // again if appconnect restarted since
        }
        self.ctl.send(&format!("show {n}"));
        // Back after the phone's "back to the car" icon: open on Android Auto's home screen,
        // not on the launcher the user left from.
        if n != 0 && self.home_on_show.swap(false, Ordering::SeqCst) {
            self.ctl.send("home");
        }
        0
    }

    /// The unit's power state (PowerWatch), as stock carlifeservice follows it (power_idl.hpp):
    /// switched off, AA's media pauses and the phone's video stops (the screen is dark); on
    /// again, both come back; last mode keeps what it was at the switch-off.
    fn on_power(&self, state: i32, a: i32, b: i32) {
        logi!("power: {} ({}, {}, {})", idl::power_state_name(state), state, a, b);
        // A boot that reaches a switch-off was a healthy one, however short: the launcher's
        // boot-loop counter (aa_run.sh, AA_BOOTS_FILE) is cleared, once per boot.
        if (state == idl::PS_USER_OFF || state == idl::PS_SHUT_DOWN)
            && !self.cfg.boots_file.is_empty()
            && !self.boots_cleared.swap(true, Ordering::SeqCst)
        {
            let err = replace_file(&self.cfg.boots_file, "0\n");
            if err.is_empty() {
                logi!("power: switch-off: boot-loop counter cleared ({})", self.cfg.boots_file);
            } else {
                logw!("power: clearing {}: {}", self.cfg.boots_file, err);
            }
        }
        if state == idl::PS_USER_OFF {
            self.last.freeze(true);
            self.audio.power(false, true);
            if self.shown.load(Ordering::SeqCst) {
                self.ctl.send("show 0");
            }
        } else if state == idl::PS_USER_ON {
            self.audio.power(true, !self.power.temporary_on());
            self.last.freeze(false);
            if self.shown.load(Ordering::SeqCst) {
                self.ctl.send("show 1");
            }
        } else if state == idl::PS_SHUT_DOWN || state == idl::PS_RESET {
            self.last.freeze(true);
        }
    }

    /// appconnect's code changes (appc.rs, AppcWatch): look again now. Until the outgoing-call
    /// change is on, the call fix in NeedBlockBTPopup() stands in.
    fn patch_appconnect(&self) {
        self.appc.now();
    }

    fn start_app(&self, n: i32) -> i32 {
        logi!("StartApp({})", n);
        0
    }

    fn carlife_connection(&self) -> i32 {
        self.patch_appconnect(); // before any call of this session
        // Called synchronously from connectionservice's USB hot-plug handler: return at once
        // and do the bring-up on a worker, as stock does (CCarLifeSetup::setupRunThread).
        // connectionservice calls this on every USB attach of a phone set to CarLife, and the
        // receiver's own accessory-mode switch re-attaches the phone. During a bring-up the
        // call is ignored. During a running session it is answered without touching the
        // session: connecting, then video, published again (reannounce).
        let mut w = self.worker.lock().unwrap();
        if w.connecting {
            logi!("CarLifeConnection(): bring-up already running, ignored");
            return 0;
        }
        if let Some(t) = w.handle.take() {
            let _ = t.join();
        }
        w.connecting = true;
        let Some(me) = self.me.upgrade() else { return 0 };
        if self.current_status() == CL_ST_VIDEO {
            logi!("CarLifeConnection(): session running; announcing it again");
            w.handle = Some(std::thread::spawn(move || me.reannounce()));
        } else {
            logi!("CarLifeConnection()");
            w.handle = Some(std::thread::spawn(move || me.run_connect()));
        }
        0
    }

    // The unit asked for a connection while one runs. Since the receiver waits for the unit
    // after a session ends (Disconnect), the session now often reaches video before the unit's
    // second call, the one for the phone re-attached in accessory mode (step 3 run 45: video at
    // 145.8 s, the call at 146.2 s). That call was ignored and nothing was published, so
    // appconnect's "connecting" for it never ended: AA not brought to the screen, no
    // "connected", until the APP key. Answering it as the stock flow sees a connection, 2 then 3,
    // ends it (run 42: the call came before video, and AA came up 0.1 s after 3).
    fn reannounce(&self) {
        self.announce_session();
        self.worker.lock().unwrap().connecting = false;
    }

    // The running session told again: connecting (2), then 0.3 s later video (3) if it is still
    // 3, and the voice module's status. Each only while the status is what it announces, checked
    // under the publisher's lock as set_status publishes, so a session that ends meanwhile is
    // never announced after its (4, 0). The session's own state and its audio, Bluetooth and mic
    // set-up are left as they are (unlike set_status).
    fn announce_session(&self) {
        if !self.announce(CL_ST_CONNECTING, true) {
            return;
        }
        if self.sleep_unless_stopping(300) {
            self.announce(CL_ST_VIDEO, false);
        }
        let p = self.publisher.lock().unwrap();
        let st = self.current_status();
        let Some(p) = p.as_ref() else { return };
        if st != CL_ST_CONNECTING && st != CL_ST_VIDEO {
            return;
        }
        let voice = self.voice.load(Ordering::SeqCst);
        p.publish(carlife::CAR_LIFE_LISTENER_ON_MODULE_STATUS_CHANGE, &args(|a| {
            a.put(&MODULE_VR).put(&voice);
        }));
    }

    // OnCarLifeConnectStatusChange(st, 0) to the subscribers if the status is st (or, with
    // or_video, st or 3); false when it was not.
    fn announce(&self, st: i32, or_video: bool) -> bool {
        let p = self.publisher.lock().unwrap();
        let cur = self.current_status();
        if cur != st && !(or_video && cur == CL_ST_VIDEO) {
            return false;
        }
        logi!("OnCarLifeConnectStatusChange({} {}, 0) (announced again)", st, status_name(st));
        if let Some(p) = p.as_ref() {
            p.publish(carlife::CAR_LIFE_LISTENER_ON_CAR_LIFE_CONNECT_STATUS_CHANGE, &args(|a| {
                a.put(&st).put(&CL_ERR_NONE);
            }));
        }
        true
    }

    // The announcer thread: announce_session() ANNOUNCE_DELAY after a client subscribed, while
    // a session runs (several subscribers close together get one announcement).
    fn run_announcer(&self) {
        loop {
            {
                let mut st = self.status.lock().unwrap();
                loop {
                    if self.stopping.load(Ordering::SeqCst) {
                        return;
                    }
                    let now = Instant::now();
                    if st.announce && now >= st.announce_at {
                        break;
                    }
                    if st.announce {
                        let d = st.announce_at - now;
                        st = self.cv.wait_timeout(st, d).unwrap().0;
                    } else {
                        st = self.cv.wait(st).unwrap();
                    }
                }
                st.announce = false;
                if st.status != CL_ST_CONNECTING && st.status != CL_ST_VIDEO {
                    continue;
                }
                logi!("a session is running (status {}): announcing it to the new subscriber", st.status);
            }
            self.announce_session();
        }
    }

    fn carlife_disconnection(&self) -> i32 {
        logi!("CarLifeDisConnection()");
        self.ctl.send("stop");
        self.set_status(CL_ST_DISCONNECTED, CL_ERR_NONE);
        0
    }

    fn single_touch_info(&self, x: u32, y: u32, ev: i32) {
        let Some(name) = touch_name(ev) else {
            logw!("SingleTouchInfo({}, {}, {}): unknown action", x, y, ev);
            return;
        };
        logv!("SingleTouchInfo({}, {}, {})", x, y, name);
        self.ctl.send(&format!("touch {name} {x} {y}"));
    }

    fn rotate_key_event(&self, code: u32, value: i32) {
        // The seek keys (15 seek -, 16 seek +) at info level, each with its time in the line's
        // stamp: whether a held key reaches the software at all (a long press, 17, about 1.6 s
        // after the press, 19, and the release, 20, after it) shows in any log brought back.
        if code == 15 || code == 16 {
            logi!("RotateKeyEvent(0x{:04x}, {})", code, value);
        } else {
            logv!("RotateKeyEvent(0x{:04x}, {})", code, value);
        }
        self.ctl.send(&format!("rotary {code} {value}"));
    }

    fn get_carlife_connect_status(&self) -> i32 {
        let mut st = self.status.lock().unwrap();
        if st.status_once {
            st.status_once = false;
            if st.status == CL_ST_VIDEO && Instant::now() < st.status_once_until {
                logi!(
                    "GetCarLifeConnectStatus() -> 2 once: an outgoing call starts; appconnect keeps Android Auto \
                     on screen (see NeedBlockBTPopup)"
                );
                return CL_ST_CONNECTING;
            }
        }
        logv!("GetCarLifeConnectStatus() -> {}", st.status);
        st.status
    }

    fn go_to_foreground(&self) {
        logi!("GoToForeGround()");
        self.ctl.send("foreground");
    }

    // Asked by the Bluetooth app on every call status (ShowCallPop) and by appconnect for the
    // steering-wheel phone key. 1 while Android Auto is on screen: the app shows no incoming-call
    // pop-up over it (AA shows the call), and appconnect answers / ends calls with the phone key
    // itself through btservice (MFLPhoneFunction). 0 otherwise, or the key would go unhandled.
    //
    // It is also the first sign of a call status change we get: the Bluetooth app asks it as
    // soon as one arrives, ~14 ms before appconnect's CAppconnectDataMgr::OnCallStatusChanged
    // (unit 0x33590) asks GetCarLifeConnectStatus() for an outgoing call (status 1): when that
    // is 3 and appconnect is in front, appconnect sleeps 1 s and launches the Bluetooth app in
    // "bt_carlife_temp_call" mode, which covers Android Auto for the whole call (step 3 runs
    // 38, 39). So when an outgoing call is starting (BtPairing::outgoing_call_starting), the
    // next GetCarLifeConnectStatus() within AA_CALL_FIX_MS (400) answers 2 once: appconnect
    // skips the launch and the call stays on AA's own screen. Lost race: the launch happens, as
    // before (step 3 run 41: by 9 ms). An incoming call (status 4) and a call ending are left
    // alone. AA_CALL_FIX=0 turns it off. It stands in only while appconnect is not patched
    // (appc.rs), which removes the launch itself.
    fn need_block_bt_popup(&self) -> i32 {
        let block = (self.current_status() == CL_ST_VIDEO && self.shown.load(Ordering::SeqCst)) as i32;
        logi!("NeedBlockBTPopup() -> {}", block);
        if block != 0 && self.cfg.call_fix && !self.appc.patched() && self.pair.outgoing_call_starting() {
            let mut st = self.status.lock().unwrap();
            st.status_once_until = Instant::now() + Duration::from_millis(self.cfg.call_fix_ms as u64);
            st.status_once = true;
        }
        block
    }

    fn need_switch_carlife_app_to_fg(&self) -> i32 {
        logi!("NeedSwitchCarLifeAppToFG() -> 0");
        0
    }

    fn get_module_status(&self, module: u32) -> u32 {
        let st = if module == MODULE_VR { self.voice.load(Ordering::SeqCst) } else { 0 };
        logv!("GetModuleStatus({}) -> {}", module, st);
        st
    }

    fn set_source_available(&self, n: i32) {
        // TODO(source): claim the media/nav/VR sources through svp.svc.source.
        logi!("SetSourceAvailable({})", n);
    }

    fn send_module_control_cmd(&self, module: u32, cmd: u32) {
        logi!("SendModuleControlCmd({}, {})", module, cmd);
        // CarLife modules: 3 music (0 pause, 1 play; see AudioSource::music_control), 4 voice
        // (0 stop: appconnect going to the background mid-assistant; AA ends its own voice
        // session, nothing to do).
        if module == 3 && cmd <= 1 {
            self.audio.music_control(cmd == 1);
        }
    }

    fn get_connect_carlife_phone_bt_address(&self) -> String {
        logi!("GetConnectCarlifePhoneBtAddress() -> \"\"");
        String::new()
    }

    fn connect_carlife_phone_is_ios(&self) -> i32 {
        logi!("ConnectCarlifePhoneIsiOS() -> 0");
        0
    }

    fn need_change_hand_free_mode(&self) -> i32 {
        // See carlife_idl.hpp: the call stays on the hands-free mode the Bluetooth app chose.
        logi!("NeedChangeHandFreeMode() -> 0");
        0
    }

    fn clear_carlife_status(&self) {
        // Stock writes 4 to the status global here; whether it also publishes is unknown,
        // so only the stored value changes.
        logi!("ClearCarLifeStatus()");
        self.status.lock().unwrap().status = CL_ST_DISCONNECTED;
        self.publish_on_top();
    }

    // Android Auto on screen, as NeedBlockBTPopup tells it (status 3 and shown), to
    // AA_ONTOP_FILE as "<pid> <0|1>", for the Bluetooth app, which shows no pop-up while it is 1
    // and this process lives (i18n/lib/bluetooth.cpp). Written when it changes; 0 at the start
    // and the stop.
    fn publish_on_top(&self) {
        self.write_on_top((self.current_status() == CL_ST_VIDEO && self.shown.load(Ordering::SeqCst)) as i32);
    }

    fn write_on_top(&self, on: i32) {
        if self.cfg.on_top_file.is_empty() {
            return;
        }
        let mut t = self.on_top.lock().unwrap();
        if on == t.on_top {
            return;
        }
        let line = format!("{} {}\n", std::process::id(), on);
        let err = replace_file(&self.cfg.on_top_file, &line);
        if !err.is_empty() {
            if !t.failed {
                logw!("{}: {}; the Bluetooth app's pop-ups are not held back", self.cfg.on_top_file, err);
            }
            t.failed = true;
            return;
        }
        t.on_top = on;
        t.failed = false;
        logi!(
            "Android Auto {} screen: Bluetooth pop-ups {}",
            if on != 0 { "on" } else { "off" },
            if on != 0 { "held back" } else { "allowed" }
        );
    }

    // ---- receiver events ---------------------------------------------------------------

    pub fn receiver_line(&self, line: &str) {
        if let Some(state) = line.strip_prefix("state ") {
            self.receiver_state(state);
            return;
        }
        let mut s = Scan::new(line);
        let audio_line = s.lit("audio").and_then(|_| {
            let what = s.word(15)?;
            let on = s.int()?;
            Some((what, on))
        });
        if let Some((what, on)) = audio_line {
            logi!("receiver: {} audio {}", what, if on != 0 { "on" } else { "off" });
            // As stock: media playback asks for the media source (mediaPrepared), the voice
            // prompts for the navigation one (ttsInit / ttsStop); system sounds too.
            match what.as_str() {
                "media" => self.audio.media(on != 0),
                "speech" => self.audio.navi(audio::NAVI_SPEECH, on != 0),
                "system" => self.audio.navi(audio::NAVI_SYSTEM, on != 0),
                _ => {}
            }
        } else if line.starts_with("btpair ") {
            let mut s = Scan::new(line);
            s.lit("btpair");
            if let Some(addr) = s.word(31) {
                let method = s.int().unwrap_or(0);
                logi!("receiver: the phone {} asks whether it is paired with the car", addr);
                self.pair.query(&addr, method);
            }
        } else if line == "hostui" {
            // The phone's "back to the car" icon (the make logo in Android Auto's launcher).
            // Stock carlifeservice publishes OnGoToDeskTop when CarLife's phone asks for the
            // same; appconnect then sends its window system event 85 and leaves the screen.
            logi!("receiver: back to the car's home screen -> OnGoToDeskTop");
            self.home_on_show.store(true, Ordering::SeqCst);
            self.go_to_desktop();
        } else if let Some(on) = prefixed_int(line, "mic") {
            logi!("receiver: mic {}", if on != 0 { "on" } else { "off" });
            // The voice module's status, on which appconnect routes the car mic to us (see
            // MicPath), as stock voice recognition (vrInit / vrStop); for the sound, the
            // navigation source rather than stock's ASR one (see AudioSource::navi).
            self.audio.navi(audio::NAVI_MIC, on != 0);
            self.set_voice(on != 0);
        } else {
            logv!("receiver: {}", line);
        }
    }

    fn receiver_state(&self, state: &str) {
        logi!("receiver state: {}", state);
        let cur = self.current_status();
        match state {
            // Stock stays at connecting (2) from ConnectionInit until the first video frame.
            "connecting" | "connected" => {
                if cur != CL_ST_CONNECTING {
                    self.set_status(CL_ST_CONNECTING, CL_ERR_NONE);
                }
            }
            "projecting" => {
                self.set_status(CL_ST_VIDEO, CL_ERR_NONE);
                self.video.new_session();
                self.audio.attach(true);
            }
            "failed" => {
                if cur == CL_ST_VIDEO {
                    self.set_status(CL_ST_DISCONNECTED, CL_ERR_FAILED);
                } else {
                    self.set_status(CL_ST_CONNECT_FAILED, CL_ERR_FAILED);
                }
            }
            // Only the end of a bring-up or a session is news. After a failure (1 or 5) the
            // receiver's "disconnected" answer to "stop" leaves the failure as the final state.
            "disconnected" | "gone" => {
                if cur == CL_ST_CONNECTING || cur == CL_ST_VIDEO {
                    self.set_status(CL_ST_DISCONNECTED, CL_ERR_NONE);
                }
            }
            _ => logw!("unknown receiver state '{}'", state),
        }
    }

    fn current_status(&self) -> i32 {
        self.status.lock().unwrap().status
    }

    // Mirrors the stock emit: store the value the getter returns, then publish. The publisher's
    // lock is held from the store to the publish, so the order of the published statuses is the
    // order they were stored in, whichever threads set them (the receiver's lines, appconnect's
    // calls, the bring-up worker); the fan-out under it only queues work for the parts' own
    // threads.
    fn set_status(&self, st: i32, err: i32) {
        let p = self.publisher.lock().unwrap();
        self.status.lock().unwrap().status = st;
        self.publish_on_top();
        // A session ended: give the audio sources back, as stock SetSourceAvailable(0) does,
        // and the mic back to Bluetooth.
        if st == CL_ST_DISCONNECTED || st == CL_ST_CONNECT_FAILED || st == CL_ST_TIMEOUT {
            self.audio.attach(false);
            self.set_voice_locked(&p, false);
            self.pair.session(false);
        }
        if st == CL_ST_VIDEO {
            self.pair.session(true);
        }
        self.cv.notify_all();
        logi!("OnCarLifeConnectStatusChange({} {}, {})", st, status_name(st), err);
        if let Some(p) = p.as_ref() {
            p.publish(carlife::CAR_LIFE_LISTENER_ON_CAR_LIFE_CONNECT_STATUS_CHANGE, &args(|a| {
                a.put(&st).put(&err);
            }));
        }
    }

    // CarLife's module 4 is voice recognition: stock carlifeservice publishes its status
    // (cmdModuleStatus) and appconnect answers OnModuleStatusChange(4, 1) with
    // SetECNRAudioPath(EAP_WIRED_PHONE_MODE) and (4, 0) with EAP_BT_MODE. While the session runs
    // it also puts the path back to what GetModuleStatus(4) says whenever btservice reports a
    // change, so GetModuleStatus must tell the truth. Running only during a session: a late
    // "mic 1" after the session's end must not take the car mic from Bluetooth calls.
    fn set_voice(&self, on: bool) {
        let p = self.publisher.lock().unwrap();
        if on {
            let st = self.current_status();
            if st != CL_ST_CONNECTING && st != CL_ST_VIDEO {
                logw!("voice module (4): the phone wants the mic outside a session (status {}); ignored", st);
                return;
            }
        }
        self.set_voice_locked(&p, on);
    }

    // The publisher's lock held.
    fn set_voice_locked(&self, p: &MutexGuard<Option<Arc<Publisher>>>, on: bool) {
        let v = on as u32;
        if self.voice.swap(v, Ordering::SeqCst) == v {
            return;
        }
        logi!("voice module (4): {} -> OnModuleStatusChange(4, {})", if on { "running" } else { "idle" }, v);
        self.mic.set(on);
        if let Some(p) = p.as_ref() {
            p.publish(carlife::CAR_LIFE_LISTENER_ON_MODULE_STATUS_CHANGE, &args(|a| {
                a.put(&MODULE_VR).put(&v);
            }));
        }
    }

    fn go_to_desktop(&self) {
        let p = self.publisher.lock().unwrap();
        if let Some(p) = p.as_ref() {
            p.publish(carlife::CAR_LIFE_LISTENER_ON_GO_TO_DESK_TOP, &[]);
        }
    }

    fn sleep_unless_stopping(&self, ms: u64) -> bool {
        let st = self.status.lock().unwrap();
        let (_st, _) = self
            .cv
            .wait_timeout_while(st, Duration::from_millis(ms), |_| !self.stopping.load(Ordering::SeqCst))
            .unwrap();
        !self.stopping.load(Ordering::SeqCst)
    }

    fn run_connect(&self) {
        if self.cfg.fake {
            self.run_fake_connect();
        } else {
            self.run_receiver_connect();
        }
        self.worker.lock().unwrap().connecting = false;
    }

    // Plays the happy path without a phone, to see what appconnect does with it. Stock sends
    // no OnConnectingStatusPercentage on the way, so neither does this.
    fn run_fake_connect(&self) {
        self.set_status(CL_ST_CONNECTING, CL_ERR_NONE);
        if !self.sleep_unless_stopping(self.cfg.fake_video_ms as u64) {
            return;
        }
        self.set_status(CL_ST_VIDEO, CL_ERR_NONE);
    }

    fn run_receiver_connect(&self) {
        self.set_status(CL_ST_CONNECTING, CL_ERR_NONE);
        if !self.ctl.wait_connected(self.cfg.receiver_wait_ms) {
            if self.stopping.load(Ordering::SeqCst) {
                return; // end_session has published the end
            }
            logw!("AA receiver not connected to {} after {} ms", self.cfg.ctl_sock, self.cfg.receiver_wait_ms);
            self.set_status(CL_ST_CONNECT_FAILED, CL_ERR_FAILED);
            return;
        }
        if self.stopping.load(Ordering::SeqCst) {
            return;
        }
        // Last mode for the session about to start (LastMode), and the car's night mode and
        // driver's side, which its service discovery reads.
        if self.last.enabled() {
            self.ctl.send(if self.last.aa_had_speakers() { "autoplay 1" } else { "autoplay 0" });
        }
        self.car.send_current();
        if !self.ctl.send("start") {
            self.set_status(CL_ST_CONNECT_FAILED, CL_ERR_FAILED);
            return;
        }
        // The receiver's state lines move the status (receiver_state); wait for an outcome.
        let st = self.status.lock().unwrap();
        let (st, _) = self
            .cv
            .wait_timeout_while(st, Duration::from_millis(self.cfg.connect_timeout_ms as u64), |s| {
                !(self.stopping.load(Ordering::SeqCst) || s.status != CL_ST_CONNECTING)
            })
            .unwrap();
        let done = self.stopping.load(Ordering::SeqCst) || st.status != CL_ST_CONNECTING;
        drop(st);
        if !done {
            // As stock ConnectionTimeOutProcess does when its connection timer expires.
            logw!("no video after {} ms, giving up", self.cfg.connect_timeout_ms);
            self.ctl.send("stop");
            self.set_status(CL_ST_TIMEOUT, CL_ERR_FAILED);
        }
    }
}

/// ISVP_CarLifeService, bound on the service port.
pub struct CarLifeService(pub Arc<CarLifeShim>);

impl Service for CarLifeService {
    fn interface(&self) -> &str {
        carlife::CAR_LIFE_SERVICE
    }

    fn call(&self, fn_id: i32, a: &mut Reader, out: &mut Writer) -> io::Result<()> {
        let s = &self.0;
        match fn_id {
            carlife::CAR_LIFE_SERVICE_PLAY => {
                let n = a.get()?;
                out.put(&s.play(n));
            }
            carlife::CAR_LIFE_SERVICE_PAUSE => {
                let n = a.get()?;
                out.put(&s.pause(n));
            }
            carlife::CAR_LIFE_SERVICE_SHOW => {
                let n = a.get()?;
                out.put(&s.show(n));
            }
            carlife::CAR_LIFE_SERVICE_START_APP => {
                let n = a.get()?;
                out.put(&s.start_app(n));
            }
            carlife::CAR_LIFE_SERVICE_CAR_LIFE_CONNECTION => {
                out.put(&s.carlife_connection());
            }
            carlife::CAR_LIFE_SERVICE_CAR_LIFE_DIS_CONNECTION => {
                out.put(&s.carlife_disconnection());
            }
            carlife::CAR_LIFE_SERVICE_SINGLE_TOUCH_INFO => {
                let (x, y, ev) = (a.get()?, a.get()?, a.get()?);
                s.single_touch_info(x, y, ev);
            }
            carlife::CAR_LIFE_SERVICE_ROTATE_KEY_EVENT => {
                let (code, value) = (a.get()?, a.get()?);
                s.rotate_key_event(code, value);
            }
            carlife::CAR_LIFE_SERVICE_GET_CAR_LIFE_CONNECT_STATUS => {
                out.put(&s.get_carlife_connect_status());
            }
            carlife::CAR_LIFE_SERVICE_GO_TO_FORE_GROUND => s.go_to_foreground(),
            carlife::CAR_LIFE_SERVICE_NEED_BLOCK_BT_POPUP => {
                out.put(&s.need_block_bt_popup());
            }
            carlife::CAR_LIFE_SERVICE_NEED_SWITCH_CAR_LIFE_APP_TO_FG => {
                out.put(&s.need_switch_carlife_app_to_fg());
            }
            carlife::CAR_LIFE_SERVICE_GET_MODULE_STATUS => {
                let m = a.get()?;
                out.put(&s.get_module_status(m));
            }
            carlife::CAR_LIFE_SERVICE_SET_SOURCE_AVAILABLE => {
                let n = a.get()?;
                s.set_source_available(n);
            }
            carlife::CAR_LIFE_SERVICE_SEND_MODULE_CONTROL_CMD => {
                let (m, c) = (a.get()?, a.get()?);
                s.send_module_control_cmd(m, c);
            }
            carlife::CAR_LIFE_SERVICE_GET_CONNECT_CARLIFE_PHONE_BT_ADDRESS => {
                out.put(&s.get_connect_carlife_phone_bt_address());
            }
            carlife::CAR_LIFE_SERVICE_CONNECT_CARLIFE_PHONE_ISI_OS => {
                out.put(&s.connect_carlife_phone_is_ios());
            }
            carlife::CAR_LIFE_SERVICE_CLEAR_CAR_LIFE_STATUS => s.clear_carlife_status(),
            carlife::CAR_LIFE_SERVICE_NEED_CHANGE_HAND_FREE_MODE => {
                out.put(&s.need_change_hand_free_mode());
            }
            _ => return Err(io::Error::other("no such method")),
        }
        Ok(())
    }
}
