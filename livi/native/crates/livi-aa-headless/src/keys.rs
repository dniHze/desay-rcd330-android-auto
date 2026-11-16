// SPDX-License-Identifier: GPL-3.0-or-later
//! The car's seek keys and phone key, with every action the platform reports for them.
//!
//! The patched appconnect (aa-carlife-shim/src/appc_patch.hpp, "seekhold" and "callkey") sends
//! `rotary <code> <16 + action>` for each action of the seek keys (codes 15 / 16) and the phone
//! key (2): 19 press, 17 long press, 18 click, 20 release (libappfw's SVPKeyEventProcessor). A
//! stock appconnect sends only `rotary 15|16 1` on the press, which ctl.rs takes as a skip.
//!
//! - A seek key clicked skips a track. Held, it fast-forwards or rewinds: from AA_SEEK_HOLD_MS
//!   (default 700) after the press with no release yet, or from the platform's long press
//!   (about 1.6 s, vpanel's SVPKeyEventProcessor) if that comes first,
//!   KEYCODE_MEDIA_FAST_FORWARD / REWIND once at once, then every AA_SEEK_REPEAT_MS (default
//!   800; 0: once per hold) until the release, or SEEK_MAX if the release never comes. Each one
//!   is a step of the player's own size (Media3 players: 15 s forward, 5 s back). (CarPlay's
//!   iPhone times a hold itself from the press and the release.) Players without a
//!   fast-forward ignore those keys (seen in the car, rs2: the hold reaches us, nothing seeks):
//!   AA_SEEK_MODE=longpress sends KEYCODE_MEDIA_NEXT / PREVIOUS held instead (down with Android
//!   Auto's long_press flag, again at each repeat, up at the release): in the car (rs3) the
//!   player skipped tracks, as Android Auto turns the key into skip-to-next whatever the flag.
//!   AA_SEEK_MODE=ffhold sends one KEYCODE_MEDIA_FAST_FORWARD / REWIND down at the hold and its
//!   up at the release, as a car's held fast-forward key. AA_SEEK_MODE=nexthold (the image's
//!   default since Public Beta 1, the user's choice) sends the key itself held: one
//!   KEYCODE_MEDIA_NEXT / PREVIOUS down at the hold and its up at the release. Measured with the
//!   user's phone (2026-11-16): YouTube Music seeks 10 s forward or back on each such hold;
//!   Spotify changes track (Next skips; Previous does not restart, it changes track). Neither
//!   takes fast-forward, and Android Auto gives the car no seek-to.
//! - The phone key clicked with no call opens Android Auto's phone screen (KEYCODE_TEL).
//!   appconnect sends it only while Android Auto is on screen (NeedBlockBTPopup), after its own
//!   handling, which answers or ends a call through the car's Bluetooth: so with a call (the
//!   phone's call list, or its telephony audio), at the press or in the CALL_SETTLE before it,
//!   the key is left to that. Held, it is left to the car too.
//!
//! A click is taken from the click or, if none comes within CLICK_WAIT of the release, from
//! the release; a press whose release never comes does nothing.

use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use livi_aa_stack::commands::{input_key, Command, InputCommand};
use livi_aa_stack::driver::SessionCmd;
use livi_aa_stack::manager::{AaHandle, SessionId};

/// The longest a held seek key seeks without a release.
const SEEK_MAX: Duration = Duration::from_secs(60);
/// How long after a release a click still belongs to that press.
const CLICK_WAIT: Duration = Duration::from_millis(300);
/// A click this long after its press's release is a new one (its press was lost).
const SAME_PRESS: Duration = Duration::from_secs(1);
/// The phone key opens nothing for this long after a call ended (the key that ended it).
const CALL_SETTLE: Duration = Duration::from_secs(2);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HeldKey {
    Previous,
    Next,
    Phone,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum KeyAction {
    Press,
    Long,
    Click,
    Release,
}

impl KeyAction {
    /// The platform's action number (the `rotary` value minus 16).
    pub fn from_svp(action: i64) -> Option<Self> {
        Some(match action {
            3 => Self::Press,
            1 => Self::Long,
            2 => Self::Click,
            4 => Self::Release,
            _ => return None,
        })
    }
}

struct Hold {
    key: HeldKey,
    /// Bumped by every press and every release: a pending repeat or click acts only if
    /// nothing came after it.
    seq: u64,
    down: bool,
    long: bool,
    /// The click (or the long press) was acted on.
    done: bool,
    released: Option<Instant>,
    /// A call was on at the press.
    call: bool,
}

#[derive(Default)]
struct State {
    seq: u64,
    hold: Option<Hold>,
    calls: usize,
    telephony: bool,
    call_ended: Option<Instant>,
}

impl State {
    fn in_call(&self) -> bool {
        self.calls > 0 || self.telephony
    }

    fn call_settling(&self) -> bool {
        self.call_ended.is_some_and(|t| t.elapsed() < CALL_SETTLE)
    }

    fn set_call(&mut self, f: impl FnOnce(&mut Self)) {
        let was = self.in_call();
        f(self);
        if was && !self.in_call() {
            self.call_ended = Some(Instant::now());
        }
    }
}

/// What a held seek key sends (AA_SEEK_MODE).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SeekMode {
    /// KEYCODE_MEDIA_FAST_FORWARD / REWIND clicks ("ff", the default).
    FastForward,
    /// KEYCODE_MEDIA_NEXT / PREVIOUS held, flagged long_press ("longpress").
    LongPress,
    /// KEYCODE_MEDIA_FAST_FORWARD / REWIND held: down at the hold, up at the release ("ffhold").
    FfHold,
    /// KEYCODE_MEDIA_NEXT / PREVIOUS held: down at the hold, up at the release ("nexthold").
    NextHold,
}

impl SeekMode {
    /// A mode that holds one key from the hold to the release: that key's code.
    fn held_key(self, key: HeldKey) -> Option<u32> {
        match self {
            SeekMode::FfHold => Some(ff_code(key)),
            SeekMode::NextHold => Some(seek_code(key)),
            _ => None,
        }
    }
}

/// The fast-forward or rewind key for a seek key.
fn ff_code(key: HeldKey) -> u32 {
    input_key(if key == HeldKey::Next { InputCommand::FastForward } else { InputCommand::Rewind })
}

/// The media key a seek key is.
fn seek_code(key: HeldKey) -> u32 {
    input_key(if key == HeldKey::Next { InputCommand::Next } else { InputCommand::Previous })
}

#[derive(Clone)]
pub struct Keys {
    mode: SeekMode,
    repeat: Option<Duration>,
    /// A seek key still down this long after its press is held (None: the long press only).
    hold: Option<Duration>,
    state: Arc<Mutex<State>>,
}

impl Keys {
    pub fn new(repeat: Option<Duration>, hold: Option<Duration>) -> Self {
        Self { mode: SeekMode::FastForward, repeat, hold, state: Arc::new(Mutex::new(State::default())) }
    }

    pub fn with_mode(mut self, mode: SeekMode) -> Self {
        self.mode = mode;
        self
    }

    /// AA_SEEK_REPEAT_MS (0..=5000), default 800; 0 seeks one step per hold. AA_SEEK_HOLD_MS
    /// (0..=5000), default 700; 0 waits for the platform's long press.
    pub fn from_env() -> Self {
        let ms = |key: &str, default: u64| {
            std::env::var(key)
                .ok()
                .and_then(|v| v.trim().parse::<u64>().ok())
                .filter(|ms| *ms <= 5000)
                .unwrap_or(default)
        };
        let (repeat, hold) = (ms("AA_SEEK_REPEAT_MS", 800), ms("AA_SEEK_HOLD_MS", 700));
        let mode = match std::env::var("AA_SEEK_MODE").as_deref().map(str::trim) {
            Ok("longpress") => SeekMode::LongPress,
            Ok("ffhold") => SeekMode::FfHold,
            Ok("nexthold") => SeekMode::NextHold,
            _ => SeekMode::FastForward,
        };
        Self::new((repeat > 0).then(|| Duration::from_millis(repeat)), (hold > 0).then(|| Duration::from_millis(hold)))
            .with_mode(mode)
    }

    /// The phone's call list changed (its length).
    pub fn calls(&self, n: usize) {
        self.state.lock().unwrap().set_call(|s| s.calls = n);
    }

    /// The phone's telephony audio started or stopped.
    pub fn telephony(&self, active: bool) {
        self.state.lock().unwrap().set_call(|s| s.telephony = active);
    }

    /// The session ended: no call, no held key.
    pub fn session_ended(&self) {
        let mut s = self.state.lock().unwrap();
        s.set_call(|s| {
            s.calls = 0;
            s.telephony = false;
        });
        s.seq += 1;
        s.hold = None;
    }

    pub fn event(&self, key: HeldKey, action: KeyAction, handle: &AaHandle, id: Option<SessionId>) {
        let Some(id) = id else {
            return;
        };
        let mut s = self.state.lock().unwrap();
        match action {
            KeyAction::Press => {
                s.seq += 1;
                let seq = s.seq;
                let call = s.in_call() || s.call_settling();
                s.hold = Some(Hold { key, seq, down: true, long: false, done: false, released: None, call });
                // Held: the seek starts on our own timer, before the platform's long press.
                if let (Some(after), true) = (self.hold, key != HeldKey::Phone) {
                    let (keys, handle) = (self.clone(), handle.clone());
                    tokio::spawn(async move {
                        tokio::time::sleep(after).await;
                        let mut s = keys.state.lock().unwrap();
                        if s.hold.as_ref().is_some_and(|h| h.seq == seq && h.down && !h.done) {
                            keys.held(&mut s, key, "held", &handle, id);
                        }
                    });
                }
            }
            KeyAction::Long => self.held(&mut s, key, "long press", handle, id),
            KeyAction::Click => {
                let same = s.hold.as_ref().is_some_and(|h| {
                    h.key == key && (h.down || h.released.is_some_and(|t| t.elapsed() < SAME_PRESS))
                });
                if same {
                    let h = s.hold.as_mut().unwrap();
                    if std::mem::replace(&mut h.done, true) {
                        return;
                    }
                } else {
                    // Its press was lost: a click of its own.
                    s.hold = None;
                }
                click(&s, key, handle, id);
            }
            KeyAction::Release => {
                if !s.hold.as_ref().is_some_and(|h| h.key == key && h.down) {
                    return;
                }
                s.seq += 1;
                let seq = s.seq;
                let h = s.hold.as_mut().unwrap();
                h.down = false;
                h.released = Some(Instant::now());
                h.seq = seq;
                if h.long {
                    if key != HeldKey::Phone {
                        match self.mode {
                            SeekMode::LongPress => handle
                                .session(id, SessionCmd::Key { code: seek_code(key), down: false, long_press: true }),
                            m @ (SeekMode::FfHold | SeekMode::NextHold) => {
                                let code = m.held_key(key).unwrap();
                                handle.session(id, SessionCmd::Key { code, down: false, long_press: false })
                            }
                            SeekMode::FastForward => {}
                        }
                        println!("[ctl] {} released", name(key));
                    }
                    return;
                }
                if h.done {
                    return;
                }
                // No click yet: it may come after the release; if not, the release is the click.
                let (state, handle) = (self.state.clone(), handle.clone());
                tokio::spawn(async move {
                    tokio::time::sleep(CLICK_WAIT).await;
                    let mut s = state.lock().unwrap();
                    let Some(h) = s.hold.as_mut().filter(|h| h.seq == seq && !h.done) else {
                        return;
                    };
                    h.done = true;
                    click(&s, key, &handle, id);
                });
            }
        }
    }
}

impl Keys {
    /// The key is held (our timer or the platform's long press, whichever comes first): a seek
    /// key fast-forwards or rewinds until the release; the phone key is left to the car.
    fn held(&self, s: &mut State, key: HeldKey, by: &str, handle: &AaHandle, id: SessionId) {
        let Some(h) = s.hold.as_mut().filter(|h| h.key == key && h.down && !h.done) else {
            return;
        };
        h.long = true;
        h.done = true;
        let seq = h.seq;
        let cmd = match key {
            HeldKey::Next => InputCommand::FastForward,
            HeldKey::Previous => InputCommand::Rewind,
            HeldKey::Phone => {
                println!("[ctl] phone key held: left to the car");
                return;
            }
        };
        let step = match self.mode {
            SeekMode::FastForward => SessionCmd::Input(cmd),
            SeekMode::LongPress => SessionCmd::Key { code: seek_code(key), down: true, long_press: true },
            m @ (SeekMode::FfHold | SeekMode::NextHold) => {
                SessionCmd::Key { code: m.held_key(key).unwrap(), down: true, long_press: false }
            }
        };
        let what = match (self.mode, key) {
            (SeekMode::FastForward, HeldKey::Next) => "fast-forward",
            (SeekMode::FastForward, _) => "rewind",
            (SeekMode::LongPress, HeldKey::Next) => "next, long press",
            (SeekMode::LongPress, _) => "previous, long press",
            (SeekMode::FfHold, HeldKey::Next) => "fast-forward held",
            (SeekMode::FfHold, _) => "rewind held",
            (SeekMode::NextHold, HeldKey::Next) => "next held",
            (SeekMode::NextHold, _) => "previous held",
        };
        println!("[ctl] {} {}: {}", name(key), by, what);
        handle.session(id, step.clone());
        if let Some(code) = self.mode.held_key(key) {
            // One down until the release; its up after SEEK_MAX if the release never comes.
            let (state, handle) = (self.state.clone(), handle.clone());
            tokio::spawn(async move {
                tokio::time::sleep(SEEK_MAX).await;
                if state.lock().unwrap().seq == seq {
                    println!("[ctl] seek key held for {} s with no release: stopped", SEEK_MAX.as_secs());
                    handle.session(id, SessionCmd::Key { code, down: false, long_press: false });
                }
            });
            return;
        }
        if let Some(every) = self.repeat {
            let (state, handle) = (self.state.clone(), handle.clone());
            tokio::spawn(async move {
                let start = Instant::now();
                loop {
                    tokio::time::sleep(every).await;
                    if state.lock().unwrap().seq != seq {
                        return;
                    }
                    if start.elapsed() >= SEEK_MAX {
                        println!("[ctl] seek key held for {} s with no release: stopped", SEEK_MAX.as_secs());
                        if let SessionCmd::Key { code, .. } = step {
                            handle.session(id, SessionCmd::Key { code, down: false, long_press: true });
                        }
                        return;
                    }
                    handle.session(id, step.clone());
                }
            });
        }
    }
}

fn name(key: HeldKey) -> &'static str {
    match key {
        HeldKey::Previous => "seek -",
        HeldKey::Next => "seek +",
        HeldKey::Phone => "phone key",
    }
}

fn click(s: &State, key: HeldKey, handle: &AaHandle, id: SessionId) {
    match key {
        HeldKey::Previous => handle.session(id, SessionCmd::Input(InputCommand::Previous)),
        HeldKey::Next => handle.session(id, SessionCmd::Input(InputCommand::Next)),
        HeldKey::Phone => {
            if s.in_call() || s.call_settling() || s.hold.as_ref().is_some_and(|h| h.call) {
                println!("[ctl] phone key during a call: left to the car's Bluetooth");
            } else {
                println!("[ctl] phone key: Android Auto's phone screen");
                handle.session(id, SessionCmd::Command(Command::OpenPhone));
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use livi_aa_stack::manager::AaCmd;
    use tokio::sync::mpsc;

    fn input(cmd: InputCommand) -> AaCmd {
        AaCmd::Session { id: 7, cmd: SessionCmd::Input(cmd) }
    }

    fn drain(rx: &mut mpsc::UnboundedReceiver<AaCmd>) -> Vec<AaCmd> {
        std::iter::from_fn(|| rx.try_recv().ok()).collect()
    }

    async fn wait(ms: u64) {
        tokio::time::sleep(Duration::from_millis(ms)).await;
    }

    #[tokio::test]
    async fn nexthold_mode_holds_next_or_previous_until_the_release() {
        let (tx, mut rx) = mpsc::unbounded_channel();
        let h = AaHandle::from_sender(tx);
        let keys = Keys::new(Some(Duration::from_millis(40)), None).with_mode(SeekMode::NextHold);
        let key = |code, down| AaCmd::Session { id: 7, cmd: SessionCmd::Key { code, down, long_press: false } };
        let (next, prev) = (input_key(InputCommand::Next), input_key(InputCommand::Previous));
        keys.event(HeldKey::Next, KeyAction::Press, &h, Some(7));
        keys.event(HeldKey::Next, KeyAction::Long, &h, Some(7));
        assert_eq!(drain(&mut rx), vec![key(next, true)], "down at the hold");
        wait(150).await;
        assert!(drain(&mut rx).is_empty(), "no repeats: one held down");
        keys.event(HeldKey::Next, KeyAction::Release, &h, Some(7));
        keys.event(HeldKey::Next, KeyAction::Click, &h, Some(7));
        wait(CLICK_WAIT.as_millis() as u64 + 100).await;
        assert_eq!(drain(&mut rx), vec![key(next, false)], "up at the release, and no second skip");
        keys.event(HeldKey::Previous, KeyAction::Press, &h, Some(7));
        keys.event(HeldKey::Previous, KeyAction::Long, &h, Some(7));
        keys.event(HeldKey::Previous, KeyAction::Release, &h, Some(7));
        assert_eq!(drain(&mut rx), vec![key(prev, true), key(prev, false)]);
        // A click still skips once, as a click.
        wait(SAME_PRESS.as_millis() as u64).await;
        keys.event(HeldKey::Next, KeyAction::Press, &h, Some(7));
        keys.event(HeldKey::Next, KeyAction::Click, &h, Some(7));
        keys.event(HeldKey::Next, KeyAction::Release, &h, Some(7));
        wait(CLICK_WAIT.as_millis() as u64 + 100).await;
        assert_eq!(drain(&mut rx), vec![input(InputCommand::Next)]);
    }

    #[tokio::test]
    async fn ffhold_mode_holds_the_fast_forward_key_until_the_release() {
        let (tx, mut rx) = mpsc::unbounded_channel();
        let h = AaHandle::from_sender(tx);
        let keys = Keys::new(Some(Duration::from_millis(40)), None).with_mode(SeekMode::FfHold);
        let key = |code, down| AaCmd::Session { id: 7, cmd: SessionCmd::Key { code, down, long_press: false } };
        let (ff, rew) = (input_key(InputCommand::FastForward), input_key(InputCommand::Rewind));
        keys.event(HeldKey::Next, KeyAction::Press, &h, Some(7));
        keys.event(HeldKey::Next, KeyAction::Long, &h, Some(7));
        assert_eq!(drain(&mut rx), vec![key(ff, true)], "down at the hold");
        wait(150).await;
        assert!(drain(&mut rx).is_empty(), "no repeats: one held down");
        keys.event(HeldKey::Next, KeyAction::Release, &h, Some(7));
        keys.event(HeldKey::Next, KeyAction::Click, &h, Some(7));
        wait(CLICK_WAIT.as_millis() as u64 + 100).await;
        assert_eq!(drain(&mut rx), vec![key(ff, false)], "up at the release, and no skip");
        keys.event(HeldKey::Previous, KeyAction::Press, &h, Some(7));
        keys.event(HeldKey::Previous, KeyAction::Long, &h, Some(7));
        keys.event(HeldKey::Previous, KeyAction::Release, &h, Some(7));
        assert_eq!(drain(&mut rx), vec![key(rew, true), key(rew, false)]);
    }

    #[tokio::test]
    async fn longpress_mode_holds_the_media_key_flagged_long_press() {
        let (tx, mut rx) = mpsc::unbounded_channel();
        let h = AaHandle::from_sender(tx);
        let keys = Keys::new(Some(Duration::from_millis(40)), None).with_mode(SeekMode::LongPress);
        let key = |down| AaCmd::Session { id: 7, cmd: SessionCmd::Key { code: seek_code(HeldKey::Next), down, long_press: true } };
        keys.event(HeldKey::Next, KeyAction::Press, &h, Some(7));
        keys.event(HeldKey::Next, KeyAction::Long, &h, Some(7));
        assert_eq!(drain(&mut rx), vec![key(true)], "down, long press, at the hold");
        wait(100).await;
        let repeats = drain(&mut rx);
        assert!(!repeats.is_empty() && repeats.iter().all(|c| *c == key(true)), "{repeats:?}");
        keys.event(HeldKey::Next, KeyAction::Release, &h, Some(7));
        keys.event(HeldKey::Next, KeyAction::Click, &h, Some(7));
        wait(CLICK_WAIT.as_millis() as u64 + 100).await;
        assert_eq!(drain(&mut rx), vec![key(false)], "up at the release, and no skip");
        assert_eq!(seek_code(HeldKey::Next), input_key(InputCommand::Next));
        assert_ne!(seek_code(HeldKey::Previous), seek_code(HeldKey::Next));
    }

    #[tokio::test]
    async fn a_click_skips_once_whatever_the_order_of_click_and_release() {
        let (tx, mut rx) = mpsc::unbounded_channel();
        let h = AaHandle::from_sender(tx);
        let keys = Keys::new(Some(Duration::from_millis(40)), None);
        use KeyAction::*;
        for order in [[Press, Click, Release], [Press, Release, Click]] {
            for a in order {
                keys.event(HeldKey::Next, a, &h, Some(7));
            }
            wait(CLICK_WAIT.as_millis() as u64 + 100).await;
            assert_eq!(drain(&mut rx), vec![input(InputCommand::Next)], "{order:?}");
        }
        // No click at all: the release stands in for it.
        keys.event(HeldKey::Previous, Press, &h, Some(7));
        keys.event(HeldKey::Previous, Release, &h, Some(7));
        assert!(drain(&mut rx).is_empty());
        wait(CLICK_WAIT.as_millis() as u64 + 100).await;
        assert_eq!(drain(&mut rx), vec![input(InputCommand::Previous)]);
        // A click whose press was lost.
        wait(SAME_PRESS.as_millis() as u64).await;
        keys.event(HeldKey::Previous, Click, &h, Some(7));
        assert_eq!(drain(&mut rx), vec![input(InputCommand::Previous)]);
        // No session: nothing.
        keys.event(HeldKey::Next, Click, &h, None);
        assert!(drain(&mut rx).is_empty());
    }

    #[tokio::test]
    async fn a_held_seek_key_seeks_until_released() {
        let (tx, mut rx) = mpsc::unbounded_channel();
        let h = AaHandle::from_sender(tx);
        let keys = Keys::new(Some(Duration::from_millis(40)), None);
        keys.event(HeldKey::Next, KeyAction::Press, &h, Some(7));
        keys.event(HeldKey::Next, KeyAction::Long, &h, Some(7));
        keys.event(HeldKey::Next, KeyAction::Long, &h, Some(7)); // a repeated long press: no extra step
        wait(150).await;
        keys.event(HeldKey::Next, KeyAction::Release, &h, Some(7));
        let steps = drain(&mut rx);
        assert!(steps.len() >= 3, "{steps:?}");
        assert!(steps.iter().all(|c| *c == input(InputCommand::FastForward)));
        keys.event(HeldKey::Next, KeyAction::Click, &h, Some(7)); // a click after a hold: no skip
        wait(CLICK_WAIT.as_millis() as u64 + 100).await;
        assert!(drain(&mut rx).is_empty(), "stopped at the release, no skip");

        keys.event(HeldKey::Previous, KeyAction::Press, &h, Some(7));
        keys.event(HeldKey::Previous, KeyAction::Long, &h, Some(7));
        assert_eq!(drain(&mut rx), vec![input(InputCommand::Rewind)]);
        keys.event(HeldKey::Previous, KeyAction::Release, &h, Some(7));
        wait(100).await;
        assert!(drain(&mut rx).is_empty());

        // One step per hold.
        let once = Keys::new(None, None);
        once.event(HeldKey::Next, KeyAction::Press, &h, Some(7));
        once.event(HeldKey::Next, KeyAction::Long, &h, Some(7));
        wait(100).await;
        assert_eq!(drain(&mut rx), vec![input(InputCommand::FastForward)]);
        once.event(HeldKey::Next, KeyAction::Release, &h, Some(7));

        // The session ends mid-hold: the steps stop.
        keys.event(HeldKey::Next, KeyAction::Press, &h, Some(7));
        keys.event(HeldKey::Next, KeyAction::Long, &h, Some(7));
        keys.session_ended();
        wait(150).await;
        assert_eq!(drain(&mut rx), vec![input(InputCommand::FastForward)]);
    }

    #[tokio::test]
    async fn a_seek_key_still_down_after_the_hold_time_seeks_without_the_long_press() {
        let (tx, mut rx) = mpsc::unbounded_channel();
        let h = AaHandle::from_sender(tx);
        let keys = Keys::new(Some(Duration::from_millis(40)), Some(Duration::from_millis(100)));
        keys.event(HeldKey::Next, KeyAction::Press, &h, Some(7));
        wait(60).await;
        assert!(drain(&mut rx).is_empty(), "not yet");
        wait(100).await;
        keys.event(HeldKey::Next, KeyAction::Long, &h, Some(7)); // the platform's, later: nothing more
        keys.event(HeldKey::Next, KeyAction::Release, &h, Some(7));
        keys.event(HeldKey::Next, KeyAction::Click, &h, Some(7));
        wait(CLICK_WAIT.as_millis() as u64 + 100).await;
        let steps = drain(&mut rx);
        assert!(!steps.is_empty() && steps.iter().all(|c| *c == input(InputCommand::FastForward)), "{steps:?}");

        // Released before the hold time: a skip, no seek.
        keys.event(HeldKey::Previous, KeyAction::Press, &h, Some(7));
        wait(30).await;
        keys.event(HeldKey::Previous, KeyAction::Release, &h, Some(7));
        keys.event(HeldKey::Previous, KeyAction::Click, &h, Some(7));
        wait(200).await;
        assert_eq!(drain(&mut rx), vec![input(InputCommand::Previous)]);

        // The phone key held: left to the car, whatever the timer.
        keys.event(HeldKey::Phone, KeyAction::Press, &h, Some(7));
        wait(200).await;
        assert!(drain(&mut rx).is_empty());
        keys.event(HeldKey::Phone, KeyAction::Release, &h, Some(7));
    }

    #[tokio::test]
    async fn the_phone_key_opens_the_phone_screen_only_without_a_call() {
        let (tx, mut rx) = mpsc::unbounded_channel();
        let h = AaHandle::from_sender(tx);
        let keys = Keys::new(None, None);
        let phone = AaCmd::Session { id: 7, cmd: SessionCmd::Command(Command::OpenPhone) };
        let press = |keys: &Keys| {
            for a in [KeyAction::Press, KeyAction::Click, KeyAction::Release] {
                keys.event(HeldKey::Phone, a, &h, Some(7));
            }
        };
        press(&keys);
        assert_eq!(drain(&mut rx), vec![phone.clone()]);

        // A call in the list, or its audio: left to the car (it answers / ends it).
        keys.calls(1);
        press(&keys);
        keys.calls(0);
        keys.telephony(true);
        press(&keys);
        // The press that ends the call: the call is gone by the click.
        keys.event(HeldKey::Phone, KeyAction::Press, &h, Some(7));
        keys.telephony(false);
        keys.event(HeldKey::Phone, KeyAction::Click, &h, Some(7));
        keys.event(HeldKey::Phone, KeyAction::Release, &h, Some(7));
        // ... and right after it.
        press(&keys);
        wait(CLICK_WAIT.as_millis() as u64 + 100).await;
        assert!(drain(&mut rx).is_empty());

        // Held: left to the car.
        let fresh = Keys::new(None, None);
        fresh.event(HeldKey::Phone, KeyAction::Press, &h, Some(7));
        fresh.event(HeldKey::Phone, KeyAction::Long, &h, Some(7));
        fresh.event(HeldKey::Phone, KeyAction::Release, &h, Some(7));
        wait(CLICK_WAIT.as_millis() as u64 + 100).await;
        assert!(drain(&mut rx).is_empty());

        // Once the call has settled, it opens again.
        wait(CALL_SETTLE.as_millis() as u64).await;
        press(&keys);
        assert_eq!(drain(&mut rx), vec![phone]);
    }
}
