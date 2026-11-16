// SPDX-License-Identifier: GPL-3.0-or-later
//
// The CarLife audio sources in the SVP source service (sourcemgr, svp.svc.source; see
// source_idl.hpp). Which source is active decides what the speakers play and where the media
// keys go: without this the radio keeps both (step 3 on the unit: no AA sound, steering-wheel
// next/previous changed the radio station). As stock carlifeservice does:
//   * a session starts: SID_CARLIFE_MEDIA/NAVI/PROMPT/ASR SS_ATTACHED, then SS_VALID;
//   * the phone starts media audio: RequestSource(SID_CARLIFE_MEDIA) and, when it answers 1,
//     ConfirmSource (stock also did this on every Show(1); we do only to resume after a
//     temporary interruption, see Show);
//   * a voice prompt plays (AA speech audio): SID_CARLIFE_NAVI is requested, and released when
//     it ends (stock ttsInit / ttsStop). Unlike stock, the assistant uses it too: while the
//     phone has the mic and while AA system sounds play (see navi());
//   * sourcemgr calls back cbSourceOp(SID_CARLIFE_MEDIA, PAUSE) when another source takes
//     over; it never calls back PLAY. Instead its SVP_SourceListener topic publishes who became
//     active, and (34, SS_RESTORED) when the interruption is over and the owner should take the
//     source back (source_idl.hpp). The phone hears it as Android Auto audio focus (receiver
//     "focus" lines), as CarPlay's service tells the iPhone: a temporary loss when the media is
//     interrupted, a permanent one once a main source (radio, USB, Bluetooth audio...) turns
//     out to have taken over, a gain when the media source is ours again;
//   * the car's mute: appconnect's SendModuleControlCmd(3, 0 / 1) while the media source is
//     active (see music_control); the unit switched off and on: the same, as stock does (power);
//   * the session ends: SS_DETACHED.
// The callback arrives over a callback connection: register with getpid() and turn that
// connection around (RCF::createCallbackConnection), exactly as libsource's client does.
// Every call runs on one worker thread, in order; a failed call drops the connection and the
// next try makes a new one. Attached or detached is a wanted state: the worker sets it until a
// whole Attach (or Detach) has gone through, every 2 s while sourcemgr fails (one lost call
// meant no AA sound for that session, or sources left attached after it); a request or a
// release is tried 3 times, 1 s apart. Requests wait until the attach has gone through.

use std::collections::VecDeque;
use std::io;
use std::sync::{Arc, Condvar, Mutex, Weak};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use rcf_lite::client::args;
use rcf_lite::{Client, Connection, Reader, Server, Service, Subscription, Writer};

use crate::config::{errstr, tcp_reachable, Config};
use crate::control::ControlChannel;
use crate::idl::{self, source};
use crate::lastmode::LastMode;
use crate::{logi, logv, logw};

/// What holds the navigation source: AA's speech (navigation prompts and the assistant's
/// replies and chimes), its system sounds, and the mic while the phone listens.
pub const NAVI_SPEECH: u32 = 1;
pub const NAVI_SYSTEM: u32 = 2;
pub const NAVI_MIC: u32 = 4;

const NAVI_LINGER: Duration = Duration::from_millis(2000);
const SOURCES: [i32; 4] = [idl::SID_CARLIFE_MEDIA, idl::SID_CARLIFE_NAVI, idl::SID_CARLIFE_PROMPT, idl::SID_CARLIFE_ASR];

#[derive(Clone, Copy, PartialEq)]
enum Kind {
    Request,
    Release,
}

#[derive(Clone, Copy, PartialEq)]
enum Loss {
    None,
    Transient,
    Permanent,
}

// What sourcemgr has (Unknown: an attach or detach failed midway).
#[derive(Clone, Copy, PartialEq)]
#[allow(clippy::enum_variant_names)] // the C++ shim's kDetachedState, kApplied, kUnknown
enum Applied {
    Detached,
    Applied,
    Unknown,
}

#[derive(Clone, Copy)]
struct Task {
    kind: Kind,
    source: i32,
    tries: i32,
}

impl Task {
    fn new(kind: Kind, source: i32) -> Self {
        Task { kind, source, tries: 0 }
    }
}

struct State {
    tasks: VecDeque<Task>,
    applied: Applied,            // what sourcemgr has (the worker sets it)
    retry_at: Option<Instant>,   // no call before this (after a failure)
    busy: bool,                  // the worker is in a call
    loss: Loss,                  // the media source taken by another (see cb_source_op)
    loss_resume_play: bool,      // AA media was playing when lost
    muted: bool,                 // the car's mute paused the music (music_control)
    power_paused: bool,          // the unit's switch-off paused it (power)
    media_playing: bool,         // AA media audio on (receiver)
    main_active_at: Option<Instant>, // the last main source's SS_ACTIVED
    navi_uses: u32,              // NAVI_* bits
    navi_held: bool,             // requested and not (yet) given back
    release_pending: bool,       // all uses ended: give it back at release_at
    release_at: Instant,
    stop: bool,
    attached: bool,
}

// The worker's own: the connections to sourcemgr and the status subscription.
#[derive(Default)]
struct Worker {
    failing: bool, // a failure was logged
    callback: Option<Connection>,
    callback_client: Option<Client>,
    client: Option<Client>,
    subscription: Option<Subscription>,
    subscribe_tried: Option<Instant>,
    subscribe_failing: bool,
}

pub struct AudioSource {
    cfg: Arc<Config>,
    ctl: Arc<ControlChannel>,
    last: Arc<LastMode>,
    me: Weak<AudioSource>,
    state: Mutex<State>,
    cv: Condvar,
    worker: Mutex<Option<JoinHandle<Worker>>>,
}

// SVP_SourceCallback servant, on the callback connection's thread.
struct Callback(Weak<AudioSource>);

impl Service for Callback {
    fn interface(&self) -> &str {
        source::SOURCE_CALLBACK
    }
    fn call(&self, fn_id: i32, a: &mut Reader, out: &mut Writer) -> io::Result<()> {
        match fn_id {
            source::SOURCE_CALLBACK_CB_SOURCE_OP => {
                let (src, op): (i32, i32) = (a.get()?, a.get()?);
                let r = match self.0.upgrade() {
                    Some(s) => s.cb_source_op(src, op),
                    None => 1,
                };
                out.put(&r);
                Ok(())
            }
            _ => Err(io::Error::other("no such method")),
        }
    }
}

// Main (background) sources in svp/etc/source.conf (FG 0), other than CL_MEDIA: tuner, disc,
// aux, terminal, Bluetooth audio, iPod, SD, USB, stream, QQ music, CarPlay and AA media,
// "no media". Taking over from CL_MEDIA, they keep the speakers until the user switches.
fn is_main_source(source: i32) -> bool {
    const MAIN: [i32; 17] = [1, 2, 3, 4, 8, 9, 10, 11, 14, 15, 16, 17, 22, 24, 25, 30, 39];
    MAIN.contains(&source)
}

fn pid() -> i32 {
    std::process::id() as i32
}

impl AudioSource {
    pub fn new(cfg: Arc<Config>, ctl: Arc<ControlChannel>, last: Arc<LastMode>) -> Arc<Self> {
        let me = Arc::new_cyclic(|me| AudioSource {
            cfg,
            ctl,
            last,
            me: me.clone(),
            state: Mutex::new(State {
                tasks: VecDeque::new(),
                applied: Applied::Detached,
                retry_at: None,
                busy: false,
                loss: Loss::None,
                loss_resume_play: false,
                muted: false,
                power_paused: false,
                media_playing: false,
                main_active_at: None,
                navi_uses: 0,
                navi_held: false,
                release_pending: false,
                release_at: Instant::now(),
                stop: false,
                attached: false,
            }),
            cv: Condvar::new(),
            worker: Mutex::new(None),
        });
        if me.cfg.source_port > 0 {
            let w = me.clone();
            *me.worker.lock().unwrap() = Some(std::thread::spawn(move || w.run()));
        }
        me
    }

    pub fn stop(&self) {
        self.state.lock().unwrap().stop = true;
        self.cv.notify_all();
        let t = self.worker.lock().unwrap().take();
        if let Some(t) = t
            && let Ok(mut w) = t.join() {
                w.callback_client = None;
                w.client = None;
                if let Some(c) = w.callback.take() {
                    c.close();
                }
                if let Some(s) = w.subscription.take() {
                    s.close();
                }
            }
    }

    /// A session started (on) or ended (off). Repeats are ignored.
    pub fn attach(&self, on: bool) {
        if self.cfg.source_port <= 0 {
            return;
        }
        {
            let mut st = self.state.lock().unwrap();
            if st.attached == on {
                return;
            }
            st.attached = on;
            // Detaching gives every source back; what was still queued for the old state is moot.
            st.navi_uses = 0;
            st.navi_held = false;
            st.release_pending = false;
            st.loss = Loss::None;
            st.muted = false;
            st.power_paused = false;
            st.media_playing = false;
            st.tasks.clear();
            st.retry_at = None;
        }
        self.cv.notify_all();
    }

    /// Nothing queued, nothing running, and sourcemgr has the wanted state (shutdown waits for it).
    pub fn idle(&self) -> bool {
        if self.cfg.source_port <= 0 {
            return true;
        }
        let st = self.state.lock().unwrap();
        st.tasks.is_empty() && !st.busy && st.applied == if st.attached { Applied::Applied } else { Applied::Detached }
    }

    /// Ask for a source: RequestSource and, when it answers 1, ConfirmSource.
    pub fn request(&self, source: i32) {
        if self.cfg.source_port <= 0 {
            return;
        }
        {
            let mut st = self.state.lock().unwrap();
            if !st.attached {
                return;
            }
            st.tasks.push_back(Task::new(Kind::Request, source));
        }
        self.cv.notify_all();
    }

    /// AA media playback started (on) or stopped (off), from the receiver: on asks for the media
    /// source, as stock mediaPrepared.
    pub fn media(&self, on: bool) {
        if self.cfg.source_port <= 0 {
            return;
        }
        self.state.lock().unwrap().media_playing = on;
        if on {
            self.request(idl::SID_CARLIFE_MEDIA);
        }
    }

    /// Back on the AA screen while another source interrupts the media for a while (a call, a
    /// prompt): ask for the media source again (granted, the phone gets the audio focus back;
    /// see do_task). While the interruption still runs, sourcemgr keeps it waiting and
    /// publishes SS_RESTORED later. Not after a main source took over (the radio, USB, Bluetooth
    /// audio): the user chose it, and opening the AA screen with the APP key, say for the map,
    /// silenced the radio, even with no AA media to play (a friend's unit, rc3). AA takes the
    /// speakers back when its media plays again (media), as CarPlay does; its prompts mix over
    /// the radio.
    pub fn resume_if_interrupted(&self) {
        let interrupted = self.state.lock().unwrap().loss == Loss::Transient;
        if interrupted {
            self.request(idl::SID_CARLIFE_MEDIA);
        }
    }

    /// appconnect's SendModuleControlCmd(3, cmd), CarLife's music module: 0 pause, 1 play. It
    /// sends 0 on the car's mute while the media source is active, on its Disconnect button and
    /// when it goes to the background for the radio or media player during voice; 1 on unmute
    /// while the media source is active, and at times that are not a request to play (a Mute OFF
    /// broadcast at session start, a call ending) (CCarlifeDataMgr::OnAudioMuteStateChanged
    /// 0x476dc, CAppconnectMainDlg::OnButtonClick 0x301c0, SVP_AppFw_IsReady 0x29af4). As
    /// CarPlay's service borrows the iPhone's main audio on mute and gives it back on unmute: a
    /// temporary focus loss, and the gain only if that loss was ours (the latch drops the other
    /// 1s; repeats are harmless). The DSP mutes the media in hardware either way.
    pub fn music_control(&self, play: bool) {
        if self.cfg.source_port <= 0 {
            return;
        }
        let mut st = self.state.lock().unwrap();
        if !st.attached {
            return;
        }
        if !play {
            if st.muted {
                return;
            }
            st.muted = true;
            if st.loss == Loss::None {
                self.send_focus("transient", "the car paused the music (mute)");
            }
        } else {
            if !st.muted {
                return;
            }
            st.muted = false;
            if st.loss == Loss::None && !st.power_paused {
                self.send_focus("gain", "the car resumed the music (unmute)");
            }
        }
    }

    /// The unit switched off (false) or on (true), as stock carlifeservice's OnPowerStateChanged
    /// (power_idl.hpp): off pauses AA's media unless it is not playing or already paused (mute,
    /// another source), and remembers that it did; on resumes only that, and not on a temporary
    /// switch-on (resume false).
    pub fn power(&self, on: bool, resume: bool) {
        if self.cfg.source_port <= 0 {
            return;
        }
        let mut st = self.state.lock().unwrap();
        if !st.attached {
            return;
        }
        if !on {
            if st.power_paused {
                return;
            }
            if !st.media_playing || st.muted || st.loss != Loss::None {
                logi!("power: Android Auto's media is not playing; nothing to pause");
                return;
            }
            st.power_paused = true;
            self.send_focus("transient", "the unit was switched off");
        } else {
            if !st.power_paused {
                return;
            }
            st.power_paused = false;
            if !resume {
                logi!("power: a temporary switch-on; the media stays paused, as stock CarLife leaves it");
                return;
            }
            if st.loss == Loss::None && !st.muted {
                self.send_focus("gain", "the unit was switched on");
            }
        }
    }

    /// Hold SID_CARLIFE_NAVI while anything uses it (the first use requests it) and give it back
    /// 2 s after the last use ends, so one assistant turn (mic, then a reply, then maybe the mic
    /// again) keeps it throughout. Stock voice recognition took SID_CARLIFE_ASR for the mic,
    /// but on this unit that source plays at the speech-recognition volume, full by default:
    /// source.conf gives it its own audio source, and audiomgr sets the prompt stream's volume
    /// only for CL_NAVI (the navigation volume; SVPRmtAudio::ActiveSource), so the assistant's
    /// first chime came out very loud (step 3). ASR also paused the radio; navigation mixes
    /// with it. The mic does not need ASR: it follows the voice module status (MicPath).
    pub fn navi(&self, use_: u32, on: bool) {
        if self.cfg.source_port <= 0 {
            return;
        }
        {
            let mut st = self.state.lock().unwrap();
            if !st.attached {
                return;
            }
            let before = st.navi_uses != 0;
            if on {
                st.navi_uses |= use_;
            } else {
                st.navi_uses &= !use_;
            }
            let now = st.navi_uses != 0;
            if before == now {
                return;
            }
            if now {
                if st.release_pending {
                    st.release_pending = false; // never given back: keep it
                } else if !st.navi_held {
                    st.navi_held = true;
                    st.tasks.push_back(Task::new(Kind::Request, idl::SID_CARLIFE_NAVI));
                }
            } else {
                st.release_pending = true;
                st.release_at = Instant::now() + NAVI_LINGER;
            }
        }
        self.cv.notify_all();
    }

    /// SVP_SourceCallback, on the callback server's thread.
    pub fn cb_source_op(&self, source: i32, op: i32) -> i32 {
        let name = match op {
            idl::OP_PLAY => "play",
            idl::OP_PAUSE => "pause",
            idl::OP_STOP => "stop",
            _ => "?",
        };
        logi!("source: cbSourceOp({}, {} {})", source, op, name);
        if source != idl::SID_CARLIFE_MEDIA || op == idl::OP_PLAY {
            return 1;
        }
        let mut st = self.state.lock().unwrap();
        if !st.attached {
            return 1; // STOP at the session's end
        }
        if st.navi_held {
            // CL_NAVI coexists with the media source, so our own prompts should not pause it.
            logi!("source: media paused while our navigation source is held; left to Android Auto");
            return 1;
        }
        if st.loss != Loss::None {
            return 1;
        }
        st.loss_resume_play = st.media_playing;
        // Who took over is published after this answer (source_idl.hpp); one that already
        // showed up is a main source: for good.
        if st.main_active_at.is_some_and(|t| t.elapsed() < Duration::from_secs(1)) {
            st.loss = Loss::Permanent;
            self.send_focus("loss", "a main source took the audio");
            self.ctl.send("key pause");
            self.last.speakers(false);
        } else {
            st.loss = Loss::Transient;
            self.send_focus("transient", "another source interrupted the media");
        }
        1
    }

    /// SVP_SourceListener::pubSourceStatus, on the subscription's thread.
    pub fn on_source_status(&self, source: i32, status: i32) {
        let ours = (idl::SID_CARLIFE_MEDIA..=idl::SID_CARLIFE_ASR).contains(&source);
        if !ours && !is_main_source(source) {
            logv!("source: status {} -> {}", source, status);
            return;
        }
        logi!("source: status {} -> {}", source, status);
        let mut resume = false;
        {
            let mut st = self.state.lock().unwrap();
            if !st.attached {
                return;
            }
            if !ours && (status == idl::SS_ACTIVED || status == idl::SS_INBACKGROUND || status == idl::SS_RESTORED) {
                if status == idl::SS_ACTIVED {
                    st.main_active_at = Some(Instant::now());
                }
                if st.loss == Loss::Transient {
                    // The radio or another main source took the speakers, or is next in line.
                    st.loss = Loss::Permanent;
                    self.send_focus("loss", "a main source took the audio");
                    self.ctl.send("key pause");
                    self.last.speakers(false);
                }
            } else if source == idl::SID_CARLIFE_MEDIA && status == idl::SS_RESTORED && st.loss != Loss::None {
                resume = true; // the interruption is over: take the source back (do_task)
            }
        }
        if resume {
            self.request(idl::SID_CARLIFE_MEDIA);
        }
    }

    // state held.
    fn send_focus(&self, state: &str, why: &str) {
        logi!("focus: {} ({})", state, why);
        self.ctl.send(&format!("focus {state}"));
    }

    // The media source is ours again (granted after a loss).
    fn regained(&self) {
        let mut g = self.state.lock().unwrap();
        let st = &mut *g;
        if st.loss == Loss::None {
            return;
        }
        let permanent = st.loss == Loss::Permanent;
        st.loss = Loss::None;
        if st.muted || st.power_paused {
            return; // the unmute / switch-on gives it back
        }
        self.send_focus("gain", if permanent { "the media source is ours again" } else { "the interruption is over" });
        // After a permanent loss the phone does not resume by itself: resume what was playing,
        // unless the phone already plays again (the usual way back: the user played AA media).
        if permanent && st.loss_resume_play && !st.media_playing {
            self.ctl.send("key play");
        }
    }

    // Subscribe to sourcemgr's status topic, unless subscribed; at most every 10 s. Worker only.
    fn subscribe(&self, w: &mut Worker) {
        if self.cfg.source_listen_port <= 0 {
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
        let (host, port) = (&self.cfg.source_host, self.cfg.source_listen_port);
        if !tcp_reachable(host, port, 1000) {
            if !w.subscribe_failing {
                logw!(
                    "source: no status publisher on {}:{}; interruptions end only when the AA screen is opened again",
                    host,
                    port
                );
            }
            w.subscribe_failing = true;
            return;
        }
        if let Some(s) = w.subscription.take() {
            logw!("source: status subscription lost; subscribing again");
            s.close();
        }
        let me = self.me.clone();
        let r = Subscription::create(
            (host.as_str(), port as u16),
            source::SOURCE_LISTENER,
            rcf_lite::client::DEFAULT_CONNECT_TIMEOUT,
            rcf_lite::client::DEFAULT_CALL_TIMEOUT,
            move |fn_id, a| {
                if fn_id != source::SOURCE_LISTENER_PUB_SOURCE_STATUS {
                    return;
                }
                let mut r = Reader::new(a);
                if let (Ok(src), Ok(status)) = (r.get::<i32>(), r.get::<i32>())
                    && let Some(s) = me.upgrade() {
                        s.on_source_status(src, status);
                    }
            },
        );
        match r {
            Ok(s) => {
                w.subscription = Some(s);
                logi!("source: subscribed to the source status topic ({}:{})", host, port);
                w.subscribe_failing = false;
            }
            Err(e) => {
                if !w.subscribe_failing {
                    logw!("source: status subscription failed: {}", errstr(&e));
                }
                w.subscribe_failing = true;
            }
        }
    }

    // Worker. Logged once per stretch of failures.
    fn failed(&self, w: &mut Worker, what: &str, why: &str) {
        if !w.failing {
            logw!("source: {}: {} (retrying quietly)", what, why);
        }
        w.failing = true;
    }

    fn connect(&self, w: &mut Worker) -> bool {
        if w.client.is_some() {
            return true;
        }
        let r = (|| -> io::Result<()> {
            // A server only for the callback connection; libsource uses TcpEndpoint(0) too.
            let server = Arc::new(Server::new().bind(Arc::new(Callback(self.me.clone()))));
            let mut cb = Client::new(&self.cfg.source_host, self.cfg.source_port as u16, source::SOURCE_SERVICE);
            cb.set_call_timeout(Duration::from_millis(5000));
            cb.call(source::SOURCE_SERVICE_REGISTER_CLIENT, &args(|a| {
                a.put(&pid());
            }))?;
            w.callback = Some(cb.create_callback_connection(&server)?);
            w.callback_client = Some(cb);
            let mut client = Client::new(&self.cfg.source_host, self.cfg.source_port as u16, source::SOURCE_SERVICE);
            client.set_call_timeout(Duration::from_millis(5000));
            w.client = Some(client);
            Ok(())
        })();
        match r {
            Ok(()) => {
                logi!("source: registered as client {}, callback connection up", pid());
                self.subscribe(w);
                true
            }
            Err(e) => {
                let what = format!("cannot reach the source service on {}:{}", self.cfg.source_host, self.cfg.source_port);
                self.failed(w, &what, &errstr(&e));
                self.drop_connection(w);
                false
            }
        }
    }

    fn drop_connection(&self, w: &mut Worker) {
        w.client = None;
        w.callback_client = None;
        if let Some(c) = w.callback.take() {
            c.close();
        }
    }

    // Worker: the four sources attached and valid (true), or detached; true when every call
    // went through.
    fn apply_state(&self, w: &mut Worker, attach: bool) -> bool {
        if !self.connect(w) {
            return false;
        }
        let client = w.client.as_mut().unwrap();
        let r = (|| -> io::Result<()> {
            if attach {
                // The unit's own record of the last main source (diagnostic, for last mode).
                if crate::log::verbose() {
                    match client.call(source::SOURCE_SERVICE_GET_LAST_BG_SOURCE, &[]).and_then(|b| Reader::new(&b).get::<i32>()) {
                        Ok(v) => logv!("source: GetLastBGSource() -> {}", v),
                        Err(e) => logv!("source: GetLastBGSource failed: {}", errstr(&e)),
                    }
                }
                for src in SOURCES {
                    let r = set_source_status(client, src, idl::SS_ATTACHED)?;
                    logi!("source: SetSourceStatus({}, attached) -> {}", src, r);
                }
                for src in SOURCES {
                    let r = set_source_status(client, src, idl::SS_VALID)?;
                    logi!("source: SetSourceStatus({}, valid) -> {}", src, r);
                }
            } else {
                for src in SOURCES {
                    let r = set_source_status(client, src, idl::SS_DETACHED)?;
                    logi!("source: SetSourceStatus({}, detached) -> {}", src, r);
                }
            }
            Ok(())
        })();
        match r {
            Ok(()) => {
                if w.failing {
                    logi!("source: the source service answers again");
                }
                w.failing = false;
                true
            }
            Err(e) => {
                self.failed(w, if attach { "attaching the sources" } else { "detaching the sources" }, &errstr(&e));
                self.drop_connection(w);
                false
            }
        }
    }

    // Worker: a request or a release; true when it went through.
    fn do_task(&self, w: &mut Worker, t: Task) -> bool {
        if !self.connect(w) {
            return false;
        }
        let client = w.client.as_mut().unwrap();
        let pid = pid();
        let r = (|| -> io::Result<()> {
            if t.kind == Kind::Request {
                let body = client.call(source::SOURCE_SERVICE_REQUEST_SOURCE, &args(|a| {
                    a.put(&t.source).put(&pid);
                }))?;
                let r: i32 = Reader::new(&body).get()?;
                logi!("source: RequestSource({}, {}) -> {}", t.source, pid, r);
                if r == 1 {
                    let body = client.call(source::SOURCE_SERVICE_CONFIRM_SOURCE, &args(|a| {
                        a.put(&t.source);
                    }))?;
                    let c: i32 = Reader::new(&body).get()?;
                    logi!("source: ConfirmSource({}) -> {}", t.source, c);
                }
                // Getting the media source back after a loss is the phone's gain (sourcemgr
                // never calls back PLAY). 3: parked behind a foreground source (a call); it
                // publishes SS_RESTORED when that ends.
                if r == 1 && t.source == idl::SID_CARLIFE_MEDIA {
                    self.last.speakers(true);
                    self.regained();
                }
                if r == 3 && t.source == idl::SID_CARLIFE_MEDIA {
                    logi!("source: media source waits behind another source until it is restored");
                }
            } else {
                // (source, client, restore), as libsource sends it (source_idl.hpp).
                let body = client.call(source::SOURCE_SERVICE_RELEASE_SOURCE, &args(|a| {
                    a.put(&t.source).put(&pid).put(&1i32);
                }))?;
                let r: i32 = Reader::new(&body).get()?;
                logi!("source: ReleaseSource({}, {}, 1) -> {}", t.source, pid, r);
            }
            Ok(())
        })();
        match r {
            Ok(()) => {
                if w.failing {
                    logi!("source: the source service answers again");
                }
                w.failing = false;
                true
            }
            Err(e) => {
                self.failed(w, if t.kind == Kind::Request { "RequestSource" } else { "ReleaseSource" }, &errstr(&e));
                self.drop_connection(w);
                false
            }
        }
    }

    fn run(&self) -> Worker {
        let mut w = Worker::default();
        loop {
            let mut t = Task::new(Kind::Request, 0);
            let mut reconcile = false;
            let mut want = false;
            {
                let mut st = self.state.lock().unwrap();
                loop {
                    if st.stop {
                        return w;
                    }
                    let now = Instant::now();
                    if st.release_pending && now >= st.release_at {
                        st.release_pending = false;
                        st.navi_held = false;
                        st.tasks.push_back(Task::new(Kind::Release, idl::SID_CARLIFE_NAVI));
                    }
                    let target = if st.attached { Applied::Applied } else { Applied::Detached };
                    if st.retry_at.is_none_or(|r| now >= r) {
                        if st.applied != target {
                            reconcile = true;
                            want = st.attached;
                            break;
                        }
                        if !st.tasks.is_empty() {
                            break;
                        }
                    }
                    // Wake for the navigation release, a retry, and every 10 s while attached to
                    // keep the status subscription up.
                    let mut until = now + Duration::from_secs(10);
                    if st.release_pending && st.release_at < until {
                        until = st.release_at;
                    }
                    let retrying = st.applied != target || !st.tasks.is_empty();
                    if let Some(r) = st.retry_at
                        && retrying && r < until {
                            until = r;
                        }
                    st = self.cv.wait_timeout(st, until.saturating_duration_since(now)).unwrap().0;
                    let after = Instant::now();
                    if after >= until
                        && st.attached
                        && w.client.is_some()
                        && st.applied == Applied::Applied
                        && !retrying
                        && !(st.release_pending && after >= st.release_at)
                    {
                        drop(st);
                        self.subscribe(&mut w);
                        st = self.state.lock().unwrap();
                    }
                }
                if !reconcile {
                    t = st.tasks.pop_front().unwrap();
                }
                st.busy = true;
            }
            let ok = if reconcile { self.apply_state(&mut w, want) } else { self.do_task(&mut w, t) };
            let mut st = self.state.lock().unwrap();
            st.busy = false;
            if reconcile {
                if ok {
                    st.applied = if want { Applied::Applied } else { Applied::Detached };
                } else {
                    st.applied = Applied::Unknown;
                    st.retry_at = Some(Instant::now() + Duration::from_secs(2));
                }
            } else if !ok && st.attached {
                t.tries += 1;
                if t.tries < 3 {
                    st.tasks.push_front(t);
                    st.retry_at = Some(Instant::now() + Duration::from_secs(1));
                }
            }
        }
    }
}

fn set_source_status(c: &mut Client, src: i32, status: i32) -> io::Result<i32> {
    let body = c.call(source::SOURCE_SERVICE_SET_SOURCE_STATUS, &args(|a| {
        a.put(&src).put(&status);
    }))?;
    Reader::new(&body).get()
}
