// SPDX-License-Identifier: GPL-3.0-or-later
//! Link to aa-carlife-shim, the stand-in for the head unit's CarLife service.
//!
//! With AA_CTL_SOCK set the receiver is *gated*: it switches a phone to accessory mode only
//! after the shim sends `start` (the user picked the CarLife tile and connectionservice asked
//! for a connection), and ends the session on `stop`. Without the gate, the platform's own
//! CarLife flow fights the receiver for the phone and resets the USB port (step 2, `aa` runs).
//! When the gate closes again by itself is in gate.rs.
//!
//! The shim listens on the socket; this side connects, and reconnects if the shim restarts.
//! One line per message:
//!
//!   both ways          hello <version>                the first line on a connection: this
//!                                                     side's protocol is 2. An older shim
//!                                                     sends none; it is not required
//!   shim -> receiver   ping <n>                       liveness, every 2 s: answered with
//!                                                     `pong <n>` at once, by the task that
//!                                                     reads the socket (the shim kills a
//!                                                     receiver that misses three)
//!                      start | stop
//!                      touch <down|up|move> <x> <y>   panel pixels from appconnect
//!                      rotary <code> <value>          RotateKeyEvent from appconnect (codes below)
//!                      key <token>                    keyfeed token: play, pause, next, ...
//!                      show <n>                       appconnect shows (1) or hides (0) AA's
//!                                                     screen; the bridge owns the video surface.
//!                                                     Hidden for 2 s: the phone is told (video
//!                                                     focus NATIVE, see Screen); shown: PROJECTED
//!                      foreground                     logged
//!                      home                           Android Auto's home screen (KEYCODE_HOME)
//!                      night <0|1>                    the car's day/night (headlights): AA's
//!                                                     night mode, also for later sessions
//!                      gps <lat> <lon> <acc_m> <alt_m> <speed_m/s> <bearing_deg>
//!                                                     the car's GPS fix (degrees; "-" = unknown)
//!                      btmac <AA:BB:CC:DD:EE:FF>      the car's Bluetooth address, from the unit's
//!                                                     Bluetooth service: advertised in service
//!                                                     discovery from the next session on (it
//!                                                     replaces AA_BT_MAC)
//!                      speed <m/s>                    the car's speed (CAN): the speed sensor, and
//!                                                     the driving status (see Drive)
//!                      gear <park|reverse|neutral|drive>   the gear sensor
//!                      rhd <0|1>                      right-hand drive: the driver's side in
//!                                                     service discovery from the next session on
//!                      btpaired <0|1>                 the answer to btpair: the car's Bluetooth
//!                                                     knows the phone (1) or not (0, the phone
//!                                                     then bonds with the car)
//!                      btauth <key>                   the pairing key the car's Bluetooth shows
//!                                                     for that bonding: sent to the phone
//!                                                     (BluetoothAuthenticationRequest) so it
//!                                                     confirms without asking
//!                      autoplay <0|1>                 last mode, for the sessions that start from
//!                                                     now on: 0 when the car's own audio had the
//!                                                     speakers at the last switch-off, so the
//!                                                     phone's autoplay is refused once
//!                                                     (AaConfig::refuse_autoplay); 1 as usual
//!                      focus <gain|loss|transient|duck>   the car took the audio (for good, for a
//!                                                     while, or ducking it) or gave it back: an
//!                                                     unsolicited AudioFocusNotification; the
//!                                                     phone pauses and resumes its media itself
//!   receiver -> shim   pong <n>                       the answer to ping <n>
//!                      state <connecting|connected|projecting|failed|disconnected>
//!                      audio <media|speech|system|telephony> <0|1>   phone audio stream on/off
//!                      mic <0|1>                                     phone wants the mic / done
//!                      hostui                    the phone's "back to the car" icon was tapped
//!                      btpair <AA:BB:..> <method>  the phone's BluetoothPairingRequest: is it
//!                                                paired with the car? (answer: btpaired)
//!
//! Any other line, and a line longer than MAX_LINE or not UTF-8, is ignored. A shim that does
//! not take a line within WRITE_TIMEOUT is taken for gone, as one that closed the socket.
//!
//! The shim turns audio and mic into the platform's audio sources (speech and mic need their
//! own, as the stock CarLife service's TTS and voice recognition request them).
//!
//! RotateKeyEvent codes come from appconnect's CCarlifeDataMgr::OnCarLifeVKeyEvent (they are
//! Baidu CarLife key codes), seen on the unit in steps 2 and 3:
//!   6 / 7    knob: a positive turn sends 6, a negative one 7. Mapped to knob right / left;
//!            AA_KNOB_RIGHT / AA_KNOB_LEFT swap them if the direction is wrong.
//!   20       right knob press (vkey 790)    -> select (D-pad centre click)
//!   15 / 16  seek - / + (vkeys 780 / 781)   -> previous / next track; appconnect only sends
//!            them while the CarLife audio source is active. A stock appconnect sends value 1
//!            on the press (a skip); the patched one (appc_patch.hpp "seekhold") sends 16 + the
//!            platform's action for each of them (19 press, 17 long press, 18 click, 20
//!            release): a click skips, holding seeks (keys.rs)
//!   2        the phone key (vkey 805), from the patched appconnect only ("callkey"), 16 +
//!            action, while Android Auto is on screen: with no call a click opens Android
//!            Auto's phone screen (keys.rs)
//!   0x21     steering-wheel voice button (vkey 804): Baidu CarLife's VR start key. In step 3
//!            run 10 every press of it came as 0x21 -> the voice assistant
//!   0x22     VR stop: not mapped (Android Auto has no stop key), logged as ignored.

use std::collections::HashSet;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use livi_aa_stack::channels::audio::AudioChannelType;
use livi_aa_stack::config::AaConfig;
use livi_aa_stack::manager::AaCmd;
use livi_aa_stack::sensors::{GpsFix, Sensor};
use livi_aa_stack::commands::{Command, InputCommand, TouchAction};
use livi_aa_stack::driver::SessionCmd;
use livi_aa_stack::manager::{AaHandle, SessionId};
use tokio::io::{AsyncBufReadExt, AsyncRead, AsyncWrite, AsyncWriteExt, BufReader};
use tokio::net::UnixStream;
use tokio::sync::{mpsc, watch};

use crate::gate::Gate;
use crate::keyfeed;
use crate::keys::{HeldKey, KeyAction, Keys};

/// This side's version of the line protocol, sent in its `hello`.
pub const PROTOCOL: u32 = 2;
const RECONNECT: Duration = Duration::from_secs(1);
/// Longest line taken from the shim (its lines are a few dozen bytes).
const MAX_LINE: usize = 1024;
/// How long the shim gets to take one line from us.
const WRITE_TIMEOUT: Duration = Duration::from_secs(2);
/// Most knob detents one rotary line stands for.
const MAX_DETENTS: u32 = 32;
/// How many different unknown verbs are logged per connection (each once).
const UNKNOWN_LOGGED: usize = 16;

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Geometry {
    pub width: u32,
    pub height: u32,
    pub knob_left: u32,
    pub knob_right: u32,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Msg {
    /// The shim's protocol version.
    Hello(u32),
    /// Answered with pong <n>.
    Ping(u64),
    Start,
    Stop,
    /// x, y as fractions of the panel (what SessionCmd::Touch takes).
    Touch { action: TouchAction, x: f64, y: f64 },
    /// One knob detent per command.
    Knob(Command, u32),
    /// Commands sent once each, in order (a click is press + release).
    Commands(Vec<Command>),
    Key(InputCommand),
    /// One action of a seek key or the phone key (keys.rs).
    Held(HeldKey, KeyAction),
    Show(i32),
    Foreground,
    Night(bool),
    Gps(GpsFix),
    /// Upper case, colon separated.
    BtMac(String),
    /// m/s.
    Speed(f64),
    /// Android Auto's Gear value.
    Gear(i32),
    Rhd(bool),
    /// Last mode: the phone may autoplay (false: refuse it once; AaConfig::refuse_autoplay).
    Autoplay(bool),
    /// AudioFocusState: 1 GAIN, 3 LOSS, 4 LOSS_TRANSIENT_CAN_DUCK, 5 LOSS_TRANSIENT.
    Focus(i32),
    BtPaired(bool),
    BtAuth(String),
}

/// The receiver's config, to keep the night mode and the car's Bluetooth address for sessions
/// that start later.
pub type Config = Arc<watch::Sender<AaConfig>>;

/// Parse one line from the shim. None for a blank, malformed or unknown line.
pub fn parse(line: &str, g: &Geometry) -> Option<Msg> {
    let mut it = line.split_whitespace();
    let msg = match it.next()? {
        "hello" => Msg::Hello(it.next()?.parse().ok()?),
        "ping" => Msg::Ping(it.next()?.parse().ok()?),
        "start" => Msg::Start,
        "stop" => Msg::Stop,
        "touch" => {
            let action = match it.next()? {
                "down" => TouchAction::Down,
                "move" => TouchAction::Move,
                "up" => TouchAction::Up,
                _ => return None,
            };
            let x: f64 = it.next()?.parse().ok()?;
            let y: f64 = it.next()?.parse().ok()?;
            // Pixels run 0..width-1: one on (or past) the edge is taken as the last pixel, as
            // the stack drops a point at 1.0, and a dropped "up" would leave the finger down.
            let (w, h) = (g.width.max(1), g.height.max(1));
            Msg::Touch {
                action,
                x: x.clamp(0.0, f64::from(w - 1)) / f64::from(w),
                y: y.clamp(0.0, f64::from(h - 1)) / f64::from(h),
            }
        }
        "rotary" => {
            let code: u32 = it.next()?.parse().ok()?;
            let value: i64 = it.next()?.parse().ok()?;
            // appconnect sends one message per turn; a huge value is junk, not 4e9 commands.
            let detents = u32::try_from(value.unsigned_abs()).ok()?.clamp(1, MAX_DETENTS);
            if code == g.knob_left {
                Msg::Knob(Command::KnobLeft, detents)
            } else if code == g.knob_right {
                Msg::Knob(Command::KnobRight, detents)
            } else {
                match code {
                    20 => Msg::Commands(vec![Command::SelectDown, Command::SelectUp]),
                    0x21 => Msg::Key(InputCommand::VoiceAssistant),
                    15 | 16 | 2 => {
                        let key = match code {
                            15 => HeldKey::Previous,
                            16 => HeldKey::Next,
                            _ => HeldKey::Phone,
                        };
                        match value {
                            // A stock appconnect: the press of a seek key, taken as a skip.
                            1 if key == HeldKey::Previous => Msg::Key(InputCommand::Previous),
                            1 if key == HeldKey::Next => Msg::Key(InputCommand::Next),
                            16..=31 => Msg::Held(key, KeyAction::from_svp(value - 16)?),
                            _ => return None,
                        }
                    }
                    _ => return None,
                }
            }
        }
        "key" => Msg::Key(keyfeed::map_key(it.next()?)?),
        "show" => Msg::Show(it.next()?.parse().ok()?),
        "foreground" => Msg::Foreground,
        "home" => Msg::Commands(vec![Command::Home]),
        "night" => Msg::Night(it.next()?.parse::<u8>().ok()? != 0),
        "btmac" => Msg::BtMac(bt_address(it.next()?)?),
        "speed" => Msg::Speed(
            it.next()?
                .parse::<f64>()
                .ok()
                .filter(|v| v.is_finite() && (0.0..150.0).contains(v))?,
        ),
        // Gear in sensor.proto: GEAR_NEUTRAL 0, GEAR_DRIVE 100, GEAR_PARK 101, GEAR_REVERSE 102.
        "gear" => Msg::Gear(match it.next()? {
            "park" => 101,
            "reverse" => 102,
            "neutral" => 0,
            "drive" => 100,
            _ => return None,
        }),
        "rhd" => Msg::Rhd(it.next()?.parse::<u8>().ok()? != 0),
        "autoplay" => Msg::Autoplay(it.next()?.parse::<u8>().ok()? != 0),
        "btpaired" => Msg::BtPaired(it.next()?.parse::<u8>().ok()? != 0),
        "btauth" => Msg::BtAuth(
            Some(it.next()?)
                .filter(|k| !k.is_empty() && k.len() <= 16 && k.chars().all(|c| c.is_ascii_alphanumeric()))?
                .to_string(),
        ),
        "focus" => Msg::Focus(match it.next()? {
            "gain" => 1,
            "loss" => 3,
            "duck" => 4,
            "transient" => 5,
            _ => return None,
        }),
        "gps" => {
            let mut num = || -> Option<Option<f64>> {
                match it.next()? {
                    "-" => Some(None),
                    v => v.parse::<f64>().ok().filter(|v| v.is_finite()).map(Some),
                }
            };
            Msg::Gps(GpsFix {
                lat_deg: num()??,
                lng_deg: num()??,
                accuracy_m: num()?,
                altitude_m: num()?,
                speed_ms: num()?,
                bearing_deg: num()?,
            })
        }
        _ => return None,
    };
    Some(msg)
}

/// `AA:BB:CC:DD:EE:FF` (either case) in upper case; None for anything else, including the
/// all-zero address a unit reports while its Bluetooth is not up.
pub fn bt_address(s: &str) -> Option<String> {
    let parts: Vec<&str> = s.split(':').collect();
    let ok = parts.len() == 6 && parts.iter().all(|p| p.len() == 2 && p.chars().all(|c| c.is_ascii_hexdigit()));
    (ok && parts.iter().any(|p| *p != "00")).then(|| s.to_ascii_uppercase())
}

/// The driving status Android Auto gets, from the car's speed: unrestricted while stopped or
/// creeping, `moving` restrictions (AA_MOVING_RESTRICTIONS, default 27: no video, no keyboard,
/// no settings, short messages; voice stays allowed; 0 never restricts) from 8 km/h until the
/// car is below 3 km/h again, so crawling in traffic does not flip it back and forth. The Noname
/// head unit derives it from the speed with a hysteresis the same way.
pub struct Drive {
    moving_restrictions: i32,
    moving: bool,
}

impl Drive {
    pub fn new(moving_restrictions: i32) -> Self {
        Self { moving_restrictions, moving: false }
    }

    pub fn from_env() -> Self {
        Self::new(moving_restrictions_from_env())
    }

    /// The restrictions to send when the speed (m/s) changes them.
    pub fn update(&mut self, speed_ms: f64) -> Option<i32> {
        let kmh = speed_ms * 3.6;
        let moving = if self.moving { kmh >= 3.0 } else { kmh >= 8.0 };
        if moving == self.moving {
            return None;
        }
        self.moving = moving;
        Some(if moving { self.moving_restrictions } else { 0 })
    }
}

/// AA_MOVING_RESTRICTIONS (0..=31), default 27; see Drive.
pub fn moving_restrictions_from_env() -> i32 {
    std::env::var("AA_MOVING_RESTRICTIONS")
        .ok()
        .and_then(|v| v.trim().parse::<i32>().ok())
        .filter(|r| (0..=31).contains(r))
        .unwrap_or(27)
}

/// AA_HIDE_NATIVE_MS, default 60000; 0 never tells the phone (see Screen).
pub fn hide_native_from_env() -> Option<Duration> {
    let ms = std::env::var("AA_HIDE_NATIVE_MS")
        .ok()
        .and_then(|v| v.trim().parse::<u64>().ok())
        .unwrap_or(60000);
    (ms > 0).then(|| Duration::from_millis(ms))
}

/// Whether the phone's main video is on the car's screen. When appconnect hides Android Auto
/// (`show 0`: the car's own screen, a call screen, another app), the phone is told after
/// AA_HIDE_NATIVE_MS (default 60000; 0 never) with video focus NATIVE, as a head unit does for
/// its native UI: the phone stops encoding until `show 1` gives it PROJECTED again, and then
/// starts again from its Android Auto logo. The delay keeps quick flips (a pop-up, the ~1 s
/// call-screen blips seen in step 3 runs 39 and 41) and short visits to the car's own apps
/// (2 s showed the last frame, the logo, then the UI on every return; user, 2026-11-16) from
/// restarting the phone's projection.
#[derive(Clone)]
pub struct Screen {
    delay: Option<Duration>,
    /// Bumped by every show / hide: a pending hide only acts if nothing came after it.
    seq: Arc<AtomicU64>,
    /// The session told NATIVE, if any. Its lock also orders the delayed hide against a show.
    native: Arc<Mutex<Option<SessionId>>>,
}

impl Screen {
    pub fn new(delay: Option<Duration>) -> Self {
        Self { delay, seq: Arc::new(AtomicU64::new(0)), native: Arc::new(Mutex::new(None)) }
    }

    pub fn from_env() -> Self {
        Self::new(hide_native_from_env())
    }

    pub fn hidden(&self, handle: &AaHandle, id: Option<SessionId>) {
        let g = {
            let _n = self.native.lock().unwrap();
            self.seq.fetch_add(1, Ordering::SeqCst) + 1
        };
        let (Some(delay), Some(id)) = (self.delay, id) else {
            return;
        };
        let me = self.clone();
        let handle = handle.clone();
        tokio::spawn(async move {
            tokio::time::sleep(delay).await;
            let mut n = me.native.lock().unwrap();
            if me.seq.load(Ordering::SeqCst) == g && n.is_none() {
                *n = Some(id);
                handle.session(id, SessionCmd::MainProjected(false));
                println!("[ctl] Android Auto hidden for {} ms: the phone's video paused", delay.as_millis());
            }
        });
    }

    pub fn shown(&self, handle: &AaHandle) {
        let mut n = self.native.lock().unwrap();
        self.seq.fetch_add(1, Ordering::SeqCst);
        if let Some(id) = n.take() {
            handle.session(id, SessionCmd::MainProjected(true));
            println!("[ctl] Android Auto shown: the phone's video resumed");
        }
    }
}

/// Sends lines to the shim while it is connected (dropped otherwise: a state is only
/// meaningful to a shim that is listening now).
#[derive(Clone)]
pub struct Link {
    out: mpsc::UnboundedSender<String>,
    keys: Keys,
}

impl Link {
    pub fn state(&self, s: &str) {
        let _ = self.out.send(format!("state {s}"));
    }

    pub fn audio(&self, stream: AudioChannelType, active: bool) {
        if stream == AudioChannelType::Telephony {
            self.keys.telephony(active);
        }
        let name = match stream {
            AudioChannelType::Media => "media",
            AudioChannelType::Speech => "speech",
            AudioChannelType::System => "system",
            AudioChannelType::Telephony => "telephony",
        };
        let _ = self.out.send(format!("audio {name} {}", u8::from(active)));
    }

    /// The phone's call list changed (its length): the phone key leaves calls to the car.
    pub fn calls(&self, n: usize) {
        self.keys.calls(n);
    }

    /// The session the shim shows ended.
    pub fn session_ended(&self) {
        self.keys.session_ended();
    }

    pub fn mic(&self, active: bool) {
        let _ = self.out.send(format!("mic {}", u8::from(active)));
    }

    pub fn host_ui(&self) {
        let _ = self.out.send("hostui".to_string());
    }

    /// Ask the shim whether the car knows the phone (see bt_pair_take). The address goes on
    /// the line as the phone sent it; the caller checked it is one (bt_address).
    pub fn bt_pair(&self, id: SessionId, address: &str, method: i32) {
        *PAIR_PENDING.lock().unwrap() = Some(id);
        let _ = self.out.send(format!("btpair {address} {method}"));
    }
}

/// The session waiting for the answer to its BluetoothPairingRequest.
static PAIR_PENDING: Mutex<Option<SessionId>> = Mutex::new(None);

/// Whether `id` was still waiting for its pairing answer (and no longer is).
pub fn bt_pair_take(id: SessionId) -> bool {
    let mut p = PAIR_PENDING.lock().unwrap();
    if *p == Some(id) {
        *p = None;
        return true;
    }
    false
}

pub fn spawn(path: String, gate: Gate, handle: AaHandle, g: Geometry, config: Config) -> Link {
    let (out, rx) = mpsc::unbounded_channel();
    let keys = Keys::from_env();
    tokio::spawn(run(path, gate, handle, g, config, keys.clone(), rx));
    Link { out, keys }
}

/// One line from the shim: its text, or the length of one that was too long or not UTF-8.
type Line = Result<String, usize>;

/// Lines from the shim, each at most MAX_LINE bytes. A longer one is skipped as it streams
/// in, never buffered whole. Cancel-safe, as it sits in a select!: the partial line is kept
/// here, and the only await is fill_buf, which takes nothing when it is cancelled.
struct LineReader<R> {
    inner: BufReader<R>,
    line: Vec<u8>,
    /// Length of the current line so far, kept or not.
    len: usize,
}

impl<R: AsyncRead + Unpin> LineReader<R> {
    fn new(r: R) -> Self {
        Self { inner: BufReader::new(r), line: Vec::new(), len: 0 }
    }

    /// The next line, without its '\n'; None at the end of the stream (a partial last line
    /// is dropped).
    async fn next_line(&mut self) -> std::io::Result<Option<Line>> {
        loop {
            let buf = self.inner.fill_buf().await?;
            if buf.is_empty() {
                return Ok(None);
            }
            let end = buf.iter().position(|b| *b == b'\n');
            let chunk = &buf[..end.unwrap_or(buf.len())];
            self.len = self.len.saturating_add(chunk.len());
            if self.len <= MAX_LINE {
                self.line.extend_from_slice(chunk);
            }
            let used = end.map_or(buf.len(), |i| i + 1);
            self.inner.consume(used);
            if end.is_some() {
                let len = std::mem::take(&mut self.len);
                let bytes = std::mem::take(&mut self.line);
                if len > MAX_LINE {
                    return Ok(Some(Err(len)));
                }
                return Ok(Some(String::from_utf8(bytes).map_err(|_| len)));
            }
        }
    }
}

/// One line to the shim. False if it failed, or the shim did not take it within
/// WRITE_TIMEOUT: a shim that stops reading is as good as gone.
async fn send<W: AsyncWrite + Unpin>(wr: &mut W, line: &str) -> bool {
    let mut bytes = Vec::with_capacity(line.len() + 1);
    bytes.extend_from_slice(line.as_bytes());
    bytes.push(b'\n');
    matches!(tokio::time::timeout(WRITE_TIMEOUT, wr.write_all(&bytes)).await, Ok(Ok(())))
}

async fn run(
    path: String,
    gate: Gate,
    handle: AaHandle,
    g: Geometry,
    config: Config,
    keys: Keys,
    mut out: mpsc::UnboundedReceiver<String>,
) {
    let mut waiting_logged = false;
    let mut drive = Drive::from_env();
    let screen = Screen::from_env();
    loop {
        let sock = match UnixStream::connect(&path).await {
            Ok(s) => s,
            Err(e) => {
                if !std::mem::replace(&mut waiting_logged, true) {
                    println!("[ctl] waiting for the shim on {path}: {e}");
                }
                // Drop stale states while nobody listens.
                while out.try_recv().is_ok() {}
                tokio::time::sleep(RECONNECT).await;
                continue;
            }
        };
        waiting_logged = false;
        println!("[ctl] connected to the shim on {path}");
        let (rd, mut wr) = sock.into_split();
        let mut lines = LineReader::new(rd);
        let mut first = true;
        let mut unknown: HashSet<String> = HashSet::new();
        if send(&mut wr, &format!("hello {PROTOCOL}")).await {
            loop {
                tokio::select! {
                    line = lines.next_line() => match line {
                        Ok(Some(Ok(line))) => {
                            let line = line.trim_end_matches('\r');
                            let msg = parse(line, &g);
                            if std::mem::replace(&mut first, false) && !matches!(msg, Some(Msg::Hello(_))) {
                                println!("[ctl] no hello from the shim (an older one): carrying on");
                            }
                            match msg {
                                // Answered here, before anything else: the shim takes three
                                // missed pongs for a hung receiver.
                                Some(Msg::Ping(n)) => {
                                    if !send(&mut wr, &format!("pong {n}")).await {
                                        break;
                                    }
                                }
                                Some(Msg::Hello(v)) => println!("[ctl] the shim speaks protocol {v}"),
                                Some(msg) => handle_msg(msg, &gate, &handle, &config, &mut drive, &screen, &keys),
                                None => {
                                    let verb = line.split_whitespace().next().unwrap_or("");
                                    if !verb.is_empty()
                                        && unknown.len() < UNKNOWN_LOGGED
                                        && unknown.insert(verb.to_string())
                                    {
                                        eprintln!("[ctl] ignoring {line:?} (and later {verb:?} lines like it)");
                                    }
                                }
                            }
                        }
                        Ok(Some(Err(len))) => eprintln!("[ctl] dropping a malformed line ({len} bytes)"),
                        _ => break,
                    },
                    Some(s) = out.recv() => {
                        if !send(&mut wr, &s).await {
                            break;
                        }
                    }
                }
            }
        }
        // Without the shim nobody owns the CarLife slot: stop, so the platform and the
        // receiver do not fight over the phone.
        println!("[ctl] shim gone: disarming and ending any session");
        gate.stop(&handle);
    }
}

fn handle_msg(
    msg: Msg,
    gate: &Gate,
    handle: &AaHandle,
    config: &Config,
    drive: &mut Drive,
    screen: &Screen,
    keys: &Keys,
) {
    let id = gate.active();
    match msg {
        // Handled by the reader (run).
        Msg::Hello(_) | Msg::Ping(_) => {}
        Msg::Start => {
            println!("[ctl] start: armed, switching the phone to accessory mode");
            gate.start();
        }
        Msg::Stop => {
            println!("[ctl] stop: disarmed, ending the session");
            gate.stop(handle);
        }
        Msg::Touch { action, x, y } => {
            if let Some(id) = id {
                handle.session(id, SessionCmd::Touch { action, x, y });
            }
        }
        Msg::Knob(cmd, detents) => {
            if let Some(id) = id {
                for _ in 0..detents {
                    handle.session(id, SessionCmd::Command(cmd));
                }
            }
        }
        Msg::Commands(cmds) => {
            if let Some(id) = id {
                for cmd in cmds {
                    handle.session(id, SessionCmd::Command(cmd));
                }
            }
        }
        Msg::Key(cmd) => {
            if let Some(id) = id {
                handle.session(id, SessionCmd::Input(cmd));
            }
        }
        Msg::Held(key, action) => keys.event(key, action, handle, id),
        Msg::Show(n) => {
            println!("[ctl] show {n}");
            if n == 0 {
                screen.hidden(handle, id);
            } else {
                screen.shown(handle);
            }
        }
        Msg::Foreground => println!("[ctl] foreground"),
        // The manager sends a changed night mode to every session, and new sessions start
        // with it.
        // The shim repeats it every 30 s; only a change is news.
        Msg::Night(night) => {
            let changed = config.send_if_modified(|c| {
                let changed = c.initial_night_mode != Some(night);
                c.initial_night_mode = Some(night);
                changed
            });
            if changed {
                println!("[ctl] night {}", u8::from(night));
            }
        }
        Msg::Gps(fix) => handle.send(AaCmd::Sensor(Sensor::Gps(fix))),
        // Read at each session's service discovery. The shim repeats it every 60 s.
        Msg::BtMac(mac) => {
            let changed = config.send_if_modified(|c| {
                let changed = c.bt_mac_address.as_deref() != Some(mac.as_str());
                c.bt_mac_address = Some(mac.clone());
                changed
            });
            if changed {
                println!("[ctl] car Bluetooth address {mac}");
            }
        }
        Msg::Speed(ms) => {
            handle.send(AaCmd::Sensor(Sensor::Speed((ms * 1000.0).round() as i32)));
            if let Some(r) = drive.update(ms) {
                // Sessions that start later get it when the phone starts the sensor.
                config.send_modify(|c| c.initial_driving_status = Some(r));
                handle.send(AaCmd::Sensor(Sensor::DrivingStatus(r)));
                println!("[ctl] driving status {r} at {:.0} km/h", ms * 3.6);
            }
        }
        Msg::Gear(gear) => handle.send(AaCmd::Sensor(Sensor::Gear(gear))),
        Msg::BtPaired(paired) => {
            let pending = PAIR_PENDING.lock().unwrap().take();
            match pending {
                Some(pid) => {
                    println!("[ctl] the car's Bluetooth {} the phone", if paired { "knows" } else { "does not know" });
                    handle.session(pid, SessionCmd::BtPairingResponse { already_paired: paired });
                }
                None => println!("[ctl] late pairing answer ignored"),
            }
        }
        Msg::BtAuth(key) => {
            println!("[ctl] pairing key from the car's Bluetooth -> phone");
            if let Some(id) = id {
                handle.session(id, SessionCmd::BtAuthentication(key));
            }
        }
        Msg::Focus(state) => {
            println!("[ctl] audio focus {state}");
            if let Some(id) = id {
                handle.session(id, SessionCmd::AudioFocus(state));
            }
        }
        // Read at each session's service discovery (DriverPosition: 0 left, 1 right).
        Msg::Rhd(rhd) => {
            let changed = config.send_if_modified(|c| {
                let pos = u8::from(rhd);
                let changed = c.driver_position != pos;
                c.driver_position = pos;
                changed
            });
            if changed {
                println!("[ctl] driver's side {}", if rhd { "right" } else { "left" });
            }
        }
        Msg::Autoplay(on) => {
            let changed = config.send_if_modified(|c| {
                let changed = c.refuse_autoplay == on;
                c.refuse_autoplay = !on;
                changed
            });
            if changed {
                println!(
                    "[ctl] last mode: {}",
                    if on {
                        "Android Auto had the speakers, the phone may autoplay"
                    } else {
                        "the car's own audio had the speakers, the phone's autoplay is refused once"
                    }
                );
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use livi_aa::usb::Control;
    use tokio::io::{AsyncBufReadExt, Lines};
    use tokio::net::UnixListener;
    use tokio::net::unix::OwnedReadHalf;

    const G: Geometry = Geometry {
        width: 800,
        height: 480,
        knob_left: 7,
        knob_right: 6,
    };

    #[test]
    fn start_stop_and_display_lines() {
        assert_eq!(parse("start", &G), Some(Msg::Start));
        assert_eq!(parse("stop\n", &G), Some(Msg::Stop));
        assert_eq!(parse("show 1", &G), Some(Msg::Show(1)));
        assert_eq!(parse("foreground", &G), Some(Msg::Foreground));
        assert_eq!(parse("home", &G), Some(Msg::Commands(vec![Command::Home])));
    }

    #[test]
    fn hello_and_ping_lines() {
        assert_eq!(parse("hello 2", &G), Some(Msg::Hello(2)));
        assert_eq!(parse("ping 41", &G), Some(Msg::Ping(41)));
        assert_eq!(parse("ping", &G), None);
        assert_eq!(parse("ping x", &G), None);
        assert_eq!(parse("hello", &G), None);
        // Lines from a newer shim this side does not know are ignored, not errors.
        assert_eq!(parse("pong 3", &G), None);
        assert_eq!(parse("frobnicate 1 2", &G), None);
    }

    #[test]
    fn touch_is_scaled_to_the_panel() {
        assert_eq!(
            parse("touch down 400 240", &G),
            Some(Msg::Touch { action: TouchAction::Down, x: 0.5, y: 0.5 })
        );
        assert!(matches!(parse("touch move 80 48", &G), Some(Msg::Touch { action: TouchAction::Move, .. })));
        assert_eq!(parse("touch tap 1 2", &G), None);
        assert_eq!(parse("touch down 1", &G), None);
    }

    // The stack drops a point at 1.0 (commands::touch_point), and a dropped "up" leaves the
    // phone with a finger down: the edge is the last pixel.
    #[test]
    fn a_touch_on_the_edge_stays_on_the_panel() {
        assert_eq!(
            parse("touch up 0 480", &G),
            Some(Msg::Touch { action: TouchAction::Up, x: 0.0, y: 479.0 / 480.0 })
        );
        assert_eq!(
            parse("touch up 900 -3", &G),
            Some(Msg::Touch { action: TouchAction::Up, x: 799.0 / 800.0, y: 0.0 })
        );
    }

    #[test]
    fn knob_codes_from_the_unit() {
        assert_eq!(parse("rotary 6 1", &G), Some(Msg::Knob(Command::KnobRight, 1)));
        assert_eq!(parse("rotary 7 1", &G), Some(Msg::Knob(Command::KnobLeft, 1)));
        assert_eq!(parse("rotary 6 3", &G), Some(Msg::Knob(Command::KnobRight, 3)));
        assert_eq!(parse("rotary 6 0", &G), Some(Msg::Knob(Command::KnobRight, 1)));
        assert_eq!(parse("rotary 6 4000000000", &G), Some(Msg::Knob(Command::KnobRight, MAX_DETENTS)));
        assert_eq!(parse("rotary 9 1", &G), None);
    }

    #[test]
    fn knob_press_and_seek_keys() {
        assert_eq!(
            parse("rotary 20 1", &G),
            Some(Msg::Commands(vec![Command::SelectDown, Command::SelectUp]))
        );
        assert_eq!(parse("rotary 33 1", &G), Some(Msg::Key(InputCommand::VoiceAssistant)));
        assert_eq!(parse("rotary 34 1", &G), None);
        assert_eq!(parse("rotary 15 1", &G), Some(Msg::Key(InputCommand::Previous)));
        assert_eq!(parse("rotary 16 1", &G), Some(Msg::Key(InputCommand::Next)));
    }

    // The patched appconnect: 16 + the platform's action (3 press, 1 long, 2 click, 4 release).
    #[test]
    fn seek_and_phone_key_actions() {
        use HeldKey::*;
        use KeyAction::*;
        assert_eq!(parse("rotary 16 19", &G), Some(Msg::Held(Next, Press)));
        assert_eq!(parse("rotary 16 17", &G), Some(Msg::Held(Next, Long)));
        assert_eq!(parse("rotary 15 18", &G), Some(Msg::Held(Previous, Click)));
        assert_eq!(parse("rotary 15 20", &G), Some(Msg::Held(Previous, Release)));
        assert_eq!(parse("rotary 2 18", &G), Some(Msg::Held(Phone, Click)));
        assert_eq!(parse("rotary 2 19", &G), Some(Msg::Held(Phone, Press)));
        // Other actions, other values, and no stock form for the phone key.
        assert_eq!(parse("rotary 16 16", &G), None);
        assert_eq!(parse("rotary 16 21", &G), None);
        assert_eq!(parse("rotary 16 3", &G), None);
        assert_eq!(parse("rotary 2 1", &G), None);
    }

    #[test]
    fn keys_use_the_keyfeed_tokens() {
        assert_eq!(parse("key play", &G), Some(Msg::Key(InputCommand::Play)));
        assert_eq!(parse("key pause", &G), Some(Msg::Key(InputCommand::Pause)));
        assert_eq!(parse("key next", &G), Some(Msg::Key(InputCommand::Next)));
        assert_eq!(parse("key bogus", &G), None);
    }

    #[test]
    fn night_and_gps_lines() {
        assert_eq!(parse("night 1", &G), Some(Msg::Night(true)));
        assert_eq!(parse("night 0", &G), Some(Msg::Night(false)));
        assert_eq!(parse("night x", &G), None);
        assert_eq!(
            parse("gps 52.52 13.405 4.5 34 12.5 270", &G),
            Some(Msg::Gps(GpsFix {
                lat_deg: 52.52,
                lng_deg: 13.405,
                accuracy_m: Some(4.5),
                altitude_m: Some(34.0),
                speed_ms: Some(12.5),
                bearing_deg: Some(270.0),
            }))
        );
        assert_eq!(
            parse("gps -33.9 151.2 - - - -", &G),
            Some(Msg::Gps(GpsFix {
                lat_deg: -33.9,
                lng_deg: 151.2,
                accuracy_m: None,
                altitude_m: None,
                speed_ms: None,
                bearing_deg: None,
            }))
        );
        assert_eq!(parse("gps - 1 - - - -", &G), None);
    }

    #[test]
    fn btmac_lines() {
        assert_eq!(
            parse("btmac 02:00:00:bb:00:01", &G),
            Some(Msg::BtMac("02:00:00:BB:00:01".into()))
        );
        assert_eq!(parse("btmac 00:00:00:00:00:00", &G), None);
        assert_eq!(parse("btmac 02:00:00:bb:00", &G), None);
        assert_eq!(parse("btmac 02:00:00:bb:00:0g", &G), None);
        assert_eq!(parse("btmac 020000bb0001", &G), None);
        assert_eq!(parse("btmac", &G), None);
    }

    // The phone's pairing address goes onto the line protocol: anything that is not an
    // address (a space or a newline would split or add a line) must not get there.
    #[test]
    fn only_an_address_passes_for_one() {
        assert!(bt_address("02:00:00:AA:00:01").is_some());
        assert!(bt_address("02:00:00:aa:00:01").is_some());
        assert!(bt_address("02:00:00:AA:00:01\nhostui").is_none());
        assert!(bt_address("02:00:00:AA:00:01 1").is_none());
        assert!(bt_address("").is_none());
    }

    #[test]
    fn vehicle_lines() {
        assert_eq!(parse("speed 13.889", &G), Some(Msg::Speed(13.889)));
        assert_eq!(parse("speed -1", &G), None);
        assert_eq!(parse("speed nan", &G), None);
        assert_eq!(parse("gear park", &G), Some(Msg::Gear(101)));
        assert_eq!(parse("gear reverse", &G), Some(Msg::Gear(102)));
        assert_eq!(parse("gear neutral", &G), Some(Msg::Gear(0)));
        assert_eq!(parse("gear drive", &G), Some(Msg::Gear(100)));
        assert_eq!(parse("gear x", &G), None);
        assert_eq!(parse("rhd 1", &G), Some(Msg::Rhd(true)));
        assert_eq!(parse("rhd 0", &G), Some(Msg::Rhd(false)));
        assert_eq!(parse("autoplay 0", &G), Some(Msg::Autoplay(false)));
        assert_eq!(parse("autoplay 1", &G), Some(Msg::Autoplay(true)));
        assert_eq!(parse("autoplay", &G), None);
        assert_eq!(parse("focus loss", &G), Some(Msg::Focus(3)));
        assert_eq!(parse("focus transient", &G), Some(Msg::Focus(5)));
        assert_eq!(parse("focus duck", &G), Some(Msg::Focus(4)));
        assert_eq!(parse("focus gain", &G), Some(Msg::Focus(1)));
        assert_eq!(parse("focus x", &G), None);
        assert_eq!(parse("btpaired 1", &G), Some(Msg::BtPaired(true)));
        assert_eq!(parse("btpaired 0", &G), Some(Msg::BtPaired(false)));
        assert_eq!(parse("btauth 123456", &G), Some(Msg::BtAuth("123456".into())));
        assert_eq!(parse("btauth 12;34", &G), None);
        assert_eq!(parse("btauth", &G), None);
    }

    #[test]
    fn driving_status_follows_the_speed_with_a_hysteresis() {
        let kmh = |v: f64| v / 3.6;
        let mut d = Drive::new(27);
        assert_eq!(d.update(kmh(0.0)), None);
        assert_eq!(d.update(kmh(7.0)), None);
        assert_eq!(d.update(kmh(9.0)), Some(27));
        assert_eq!(d.update(kmh(50.0)), None);
        assert_eq!(d.update(kmh(4.0)), None);
        assert_eq!(d.update(kmh(2.0)), Some(0));
        assert_eq!(d.update(kmh(5.0)), None);
        assert_eq!(parse("gps 1 2 3", &G), None);
        assert_eq!(parse("gps nan 2 - - - -", &G), None);
    }

    #[test]
    fn audio_and_mic_lines() {
        let (out, mut rx) = mpsc::unbounded_channel();
        let link = Link { out, keys: Keys::new(None, None) };
        link.audio(AudioChannelType::Speech, true);
        link.audio(AudioChannelType::Media, false);
        link.mic(true);
        link.host_ui();
        assert_eq!(rx.try_recv().unwrap(), "audio speech 1");
        assert_eq!(rx.try_recv().unwrap(), "audio media 0");
        assert_eq!(rx.try_recv().unwrap(), "mic 1");
        assert_eq!(rx.try_recv().unwrap(), "hostui");
    }

    #[tokio::test]
    async fn hidden_for_the_delay_pauses_the_phone_video_and_shown_resumes_it() {
        let (tx, mut rx) = mpsc::unbounded_channel();
        let handle = AaHandle::from_sender(tx);
        let focus = |id, projected| AaCmd::Session { id, cmd: SessionCmd::MainProjected(projected) };
        let screen = Screen::new(Some(Duration::from_millis(50)));
        let settle = || tokio::time::sleep(Duration::from_millis(150));

        // A quick flip (a pop-up, a call-screen blip): the phone is not told.
        screen.hidden(&handle, Some(7));
        screen.shown(&handle);
        settle().await;
        assert!(rx.try_recv().is_err());

        // Hidden for longer: NATIVE once, then PROJECTED on show, to the same session.
        screen.hidden(&handle, Some(7));
        settle().await;
        assert_eq!(rx.try_recv().unwrap(), focus(7, false));
        screen.hidden(&handle, Some(7));
        settle().await;
        assert!(rx.try_recv().is_err());
        screen.shown(&handle);
        assert_eq!(rx.try_recv().unwrap(), focus(7, true));
        screen.shown(&handle);
        assert!(rx.try_recv().is_err());

        // No session, or switched off (AA_HIDE_NATIVE_MS=0): nothing.
        screen.hidden(&handle, None);
        settle().await;
        let off = Screen::new(None);
        off.hidden(&handle, Some(7));
        settle().await;
        off.shown(&handle);
        assert!(rx.try_recv().is_err());
    }

    #[test]
    fn junk_is_ignored() {
        assert_eq!(parse("", &G), None);
        assert_eq!(parse("hello", &G), None);
    }

    #[tokio::test]
    async fn the_line_reader_bounds_a_line_before_buffering_it() {
        let (mut shim, ours) = tokio::io::duplex(256);
        let mut lines = LineReader::new(ours);
        let writer = tokio::spawn(async move {
            shim.write_all(b"start\n").await.unwrap();
            // Far longer than MAX_LINE, in pieces smaller than the reader's buffer.
            for _ in 0..40 {
                shim.write_all(&[b'x'; 100]).await.unwrap();
            }
            shim.write_all(b"\n\xff\xfe\nping 7\r\npartial").await.unwrap();
        });
        assert_eq!(lines.next_line().await.unwrap(), Some(Ok("start".to_string())));
        assert_eq!(lines.next_line().await.unwrap(), Some(Err(4000)));
        assert_eq!(lines.next_line().await.unwrap(), Some(Err(2)));
        assert_eq!(lines.next_line().await.unwrap(), Some(Ok("ping 7\r".to_string())));
        writer.await.unwrap();
        // The writer is gone: a partial last line is dropped, the stream ends.
        assert_eq!(lines.next_line().await.unwrap(), None);
    }

    fn sock_path(tag: &str) -> String {
        let path = std::env::temp_dir().join(format!("aa-ctl-{tag}-{}.sock", std::process::id()));
        let _ = std::fs::remove_file(&path);
        path.display().to_string()
    }

    fn gated() -> Gate {
        let (notes, _) = mpsc::unbounded_channel();
        Gate::new(Control::default().gated(notes), true)
    }

    async fn next(lines: &mut Lines<BufReader<OwnedReadHalf>>) -> String {
        tokio::time::timeout(Duration::from_secs(5), lines.next_line())
            .await
            .expect("a line from the receiver")
            .unwrap()
            .expect("the receiver kept the link")
    }

    async fn accept(listener: &UnixListener) -> UnixStream {
        tokio::time::timeout(Duration::from_secs(5), listener.accept())
            .await
            .expect("the receiver connects")
            .unwrap()
            .0
    }

    // The protocol agreed with the shim: hello first, a pong for every ping at once, and
    // junk (unknown verbs, over-long or non-UTF-8 lines) ignored without dropping the link.
    #[tokio::test]
    async fn hello_ping_and_junk_on_the_link() {
        let path = sock_path("hello");
        let listener = UnixListener::bind(&path).unwrap();
        let (tx, _cmds) = mpsc::unbounded_channel();
        let gate = gated();
        let (cfg, _) = watch::channel(AaConfig::default());
        let link = spawn(path.clone(), gate.clone(), AaHandle::from_sender(tx), G, Arc::new(cfg));

        let (rd, mut shim) = accept(&listener).await.into_split();
        let mut lines = BufReader::new(rd).lines();
        assert_eq!(next(&mut lines).await, "hello 2");
        shim.write_all(b"hello 2\nping 41\n").await.unwrap();
        assert_eq!(next(&mut lines).await, "pong 41");
        shim.write_all(b"frobnicate 1 2\nping 42\n").await.unwrap();
        assert_eq!(next(&mut lines).await, "pong 42");
        shim.write_all(&[b'x'; 5000]).await.unwrap();
        shim.write_all(b"\n\xff\xfe\nping 43\n").await.unwrap();
        assert_eq!(next(&mut lines).await, "pong 43");

        // The lines the shim relies on still work, and states still go out.
        let before = gate.generation();
        shim.write_all(b"start\nping 44\n").await.unwrap();
        assert_eq!(next(&mut lines).await, "pong 44");
        assert_eq!(gate.generation(), before + 1);
        link.state("connecting");
        assert_eq!(next(&mut lines).await, "state connecting");

        // The shim goes away: stopped, and back with a hello once it listens again.
        drop(shim);
        drop(lines);
        let (rd, _shim) = accept(&listener).await.into_split();
        let mut lines = BufReader::new(rd).lines();
        assert_eq!(next(&mut lines).await, "hello 2");
        assert_eq!(gate.generation(), before + 2);
        let _ = std::fs::remove_file(&path);
    }

    // A shim that stops reading is taken for gone once a line waits WRITE_TIMEOUT: the
    // receiver stops and reconnects instead of queueing forever.
    #[tokio::test]
    async fn a_shim_that_stops_reading_is_dropped() {
        let path = sock_path("stuck");
        let listener = UnixListener::bind(&path).unwrap();
        let (tx, _cmds) = mpsc::unbounded_channel();
        let gate = gated();
        let (cfg, _) = watch::channel(AaConfig::default());
        let link = spawn(path.clone(), gate.clone(), AaHandle::from_sender(tx), G, Arc::new(cfg));

        let _stuck = accept(&listener).await;
        let before = gate.generation();
        // More than any socket buffer holds.
        let filler = "x".repeat(200);
        for _ in 0..20_000 {
            link.state(&filler);
        }
        let (rd, _shim) = accept(&listener).await.into_split();
        let mut lines = BufReader::new(rd).lines();
        assert_eq!(next(&mut lines).await, "hello 2");
        assert_eq!(gate.generation(), before + 1, "the stuck shim counted as gone (stop)");
        let _ = std::fs::remove_file(&path);
    }
}
