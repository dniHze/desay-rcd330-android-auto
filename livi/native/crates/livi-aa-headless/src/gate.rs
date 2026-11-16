// SPDX-License-Identifier: GPL-3.0-or-later
//! The gate: when the USB watcher may switch a phone to accessory mode, which session the
//! shim's input goes to, and what a `stop` ends.
//!
//! The shim arms the gate with `start` (one per CarLifeConnection: a phone was plugged in or
//! Android Auto was picked) and disarms it with `stop`. In between, the end of a session
//! decides:
//!
//!   * A session that projected ends: disarmed, unless the shim sent another `start` after
//!     the session projected. That `start` is for the next connection, and the old session
//!     can end a few seconds after it (3 s after the phone's end of the link closed, 5 s on a
//!     ping timeout); disarming then would leave the next phone alone.
//!   * The end of a session a newer one has already replaced: the gate is left alone; the
//!     newer session's own end decides (step 3 run 49: a dropped first accessory reported its
//!     end after the phone's next accessory was up, and the idle timer then disarmed the gate
//!     under the running session).
//!   * Any other end (a bring-up that never projected, a failed switch): disarmed if nothing
//!     new happens within GATE_IDLE and no session is running then. The first accessory after a switch is routinely dropped
//!     by the platform (connectionservice resets the USB port about 1.5 s after the switch) and comes back 2-3 s later; it must find the
//!     gate open. Left open for good, the next phone plugged in hours later would be switched
//!     before the platform asked for it.
//!
//! `stop` ends the session that was current when it came, and only that: its USB side is
//! cancelled after STOP_GRACE unless the shim has spoken again since (a new `start` brings
//! up a new session, which the old cancel must not take down). A `stop` before any session
//! was spawned cancels the bring-up in flight at once.
//!
//! `State` is the bookkeeping without side effects (unit-tested); `Gate` adds the USB control,
//! the session commands and the timers.

use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use livi_aa::usb::Control;
use livi_aa_stack::driver::SessionCmd;
use livi_aa_stack::manager::{AaHandle, SessionId};

/// After a session ends without projecting (or a switch fails), how long the gate stays open
/// for the phone to come back on its own.
pub const GATE_IDLE: Duration = Duration::from_secs(10);
/// After `stop`, how long the phone gets to close the session itself before its USB side is
/// cancelled.
pub const STOP_GRACE: Duration = Duration::from_secs(2);

struct Tracked {
    /// The phone's USB serial (empty: unknown).
    serial: String,
    /// The shim generation when the session first projected; None while it has not.
    projected_at: Option<u64>,
}

/// A point in the gate's history. Nothing new since means no `start` or `stop` and no new
/// session.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Mark {
    generation: u64,
    spawns: u64,
}

/// What a session's end does to the gate.
#[derive(Debug, PartialEq, Eq)]
pub enum AfterEnd {
    Disarm,
    /// A newer session is running: its end decides.
    Keep,
    /// Disarm after GATE_IDLE if the gate is still at this mark.
    DisarmIfIdle(Mark),
}

#[derive(Debug, PartialEq, Eq)]
pub struct Ended {
    /// It was the session the shim knows (the most recent one): the shim hears it ended.
    pub current: bool,
    pub then: AfterEnd,
}

/// What a `stop` does.
#[derive(Debug, PartialEq, Eq)]
pub struct StopPlan {
    /// The session to end with a ByeBye.
    pub disconnect: Option<SessionId>,
    /// No session yet: cancel the accessory bring-up in flight now.
    pub cancel_now: bool,
    /// After STOP_GRACE, cancel the USB side of that session: its serial (empty: every
    /// accessory), if the gate is still at this generation then.
    pub cancel_later: Option<(String, u64)>,
}

/// The gate's bookkeeping.
#[derive(Default)]
pub struct State {
    /// Bumped by every `start` and `stop`: a decision taken before the latest one is stale.
    generation: u64,
    /// Bumped by every session the manager spawns.
    spawns: u64,
    /// The session the shim's input goes to: the most recent one spawned.
    active: Option<SessionId>,
    sessions: HashMap<SessionId, Tracked>,
}

impl State {
    pub fn started(&mut self) {
        self.generation += 1;
    }

    pub fn active(&self) -> Option<SessionId> {
        self.active
    }

    pub fn spawned(&mut self, id: SessionId, serial: Option<String>) {
        self.spawns += 1;
        self.active = Some(id);
        self.sessions.insert(
            id,
            Tracked {
                serial: serial.unwrap_or_default(),
                projected_at: None,
            },
        );
    }

    /// The session's main video is on screen. True the first time only: the shim hears
    /// "projecting" once per session.
    pub fn projected(&mut self, id: SessionId) -> bool {
        match self.sessions.get_mut(&id) {
            Some(t) if t.projected_at.is_none() => {
                t.projected_at = Some(self.generation);
                true
            }
            _ => false,
        }
    }

    pub fn ended(&mut self, id: SessionId) -> Ended {
        let current = self.active.is_none() || self.active == Some(id);
        if self.active == Some(id) {
            self.active = None;
        }
        let projected_at = self.sessions.remove(&id).and_then(|t| t.projected_at);
        let then = if self.active.is_some() {
            AfterEnd::Keep
        } else {
            match projected_at {
                Some(at) if at == self.generation => AfterEnd::Disarm,
                _ => AfterEnd::DisarmIfIdle(self.mark()),
            }
        };
        Ended { current, then }
    }

    pub fn mark(&self) -> Mark {
        Mark {
            generation: self.generation,
            spawns: self.spawns,
        }
    }

    /// Nothing new since `mark`, and no session running.
    pub fn idle_since(&self, mark: Mark) -> bool {
        self.mark() == mark && self.active.is_none()
    }

    pub fn stop(&mut self) -> StopPlan {
        self.generation += 1;
        match self.active.take() {
            Some(id) => {
                let serial = self.sessions.get(&id).map(|t| t.serial.clone()).unwrap_or_default();
                StopPlan {
                    disconnect: Some(id),
                    cancel_now: false,
                    cancel_later: Some((serial, self.generation)),
                }
            }
            None => StopPlan {
                disconnect: None,
                cancel_now: true,
                cancel_later: None,
            },
        }
    }

    /// No `start` or `stop` came after the one that left the gate at `generation`.
    pub fn current(&self, generation: u64) -> bool {
        self.generation == generation
    }

    #[cfg(test)]
    pub fn generation(&self) -> u64 {
        self.generation
    }
}

/// The gate with its side effects: the USB watcher's arm state, session commands, timers.
#[derive(Clone)]
pub struct Gate {
    control: Control,
    state: Arc<Mutex<State>>,
    /// With the shim (AA_CTL_SOCK). Without it the watcher is not gated and session ends
    /// leave it alone.
    gated: bool,
}

impl Gate {
    pub fn new(control: Control, gated: bool) -> Self {
        Self {
            control,
            state: Arc::new(Mutex::new(State::default())),
            gated,
        }
    }

    pub fn active(&self) -> Option<SessionId> {
        self.state.lock().unwrap().active()
    }

    /// The shim's `start`: arm, and switch the phones already attached.
    pub fn start(&self) {
        self.state.lock().unwrap().started();
        self.control.arm(true);
    }

    /// The shim's `stop` (or the shim going away): disarm, and end the session that was
    /// current; see the module notes.
    pub fn stop(&self, handle: &AaHandle) {
        self.control.arm(false);
        let plan = self.state.lock().unwrap().stop();
        if let Some(id) = plan.disconnect {
            handle.session(id, SessionCmd::Disconnect);
        }
        if plan.cancel_now {
            let n = self.control.restart("");
            if n > 0 {
                println!("[ctl] stop before the session was up: its accessory cancelled");
            }
        }
        if let Some((serial, generation)) = plan.cancel_later {
            let gate = self.clone();
            tokio::spawn(async move {
                tokio::time::sleep(STOP_GRACE).await;
                let current = gate.state.lock().unwrap().current(generation);
                if current {
                    gate.control.restart(&serial);
                } else {
                    println!("[ctl] the shim spoke again after its stop: the stopped session is not cancelled");
                }
            });
        }
    }

    pub fn spawned(&self, id: SessionId, serial: Option<String>) {
        self.state.lock().unwrap().spawned(id, serial);
    }

    /// See State::projected.
    pub fn projected(&self, id: SessionId) -> bool {
        self.state.lock().unwrap().projected(id)
    }

    /// A session ended. Returns whether the shim should hear about it (State::ended).
    pub fn ended(&self, id: SessionId) -> bool {
        let end = self.state.lock().unwrap().ended(id);
        if self.gated {
            match end.then {
                AfterEnd::Disarm => {
                    self.control.arm(false);
                    println!("[ctl] session {id} ended: disarmed until the shim's next start");
                }
                AfterEnd::DisarmIfIdle(mark) => self.disarm_if_idle(mark, format!("session {id} ended")),
                AfterEnd::Keep => {}
            }
        }
        end.current
    }

    /// The accessory-mode switch failed: the gate closes if nothing new follows.
    pub fn switch_failed(&self) {
        if self.gated {
            let mark = self.state.lock().unwrap().mark();
            self.disarm_if_idle(mark, "the switch failed".to_string());
        }
    }

    fn disarm_if_idle(&self, mark: Mark, why: String) {
        let gate = self.clone();
        tokio::spawn(async move {
            tokio::time::sleep(GATE_IDLE).await;
            let idle = gate.state.lock().unwrap().idle_since(mark);
            if idle {
                gate.control.arm(false);
                println!(
                    "[ctl] {why} and nothing followed in {} s: disarmed until the shim's next start",
                    GATE_IDLE.as_secs()
                );
            }
        });
    }

    #[cfg(test)]
    pub fn generation(&self) -> u64 {
        self.state.lock().unwrap().generation()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn serial(s: &str) -> Option<String> {
        Some(s.to_string())
    }

    #[test]
    fn a_projected_session_ending_disarms() {
        let mut s = State::default();
        s.started();
        s.spawned(1, serial("A"));
        assert!(s.projected(1));
        assert!(!s.projected(1), "projecting is news once per session");
        assert_eq!(s.ended(1), Ended { current: true, then: AfterEnd::Disarm });
        assert_eq!(s.active(), None);
    }

    // The platform drops the first accessory after a switch; the phone comes back 2-3 s
    // later, and the shim's next `start` (for that re-attach) arrives before it projects.
    #[test]
    fn the_platform_reset_after_a_switch_keeps_the_gate_open() {
        let mut s = State::default();
        s.started();
        s.spawned(1, serial("A"));
        let first = s.ended(1);
        assert!(first.current);
        let AfterEnd::DisarmIfIdle(mark) = first.then else {
            panic!("a bring-up that never projected must not disarm at once");
        };
        s.spawned(2, serial("A"));
        assert!(!s.idle_since(mark), "the phone came back: the idle disarm is off");
        s.started();
        assert!(s.projected(2));
        assert_eq!(s.ended(2).then, AfterEnd::Disarm);
    }

    #[test]
    fn a_bring_up_that_never_comes_back_disarms_when_idle() {
        let mut s = State::default();
        s.started();
        s.spawned(1, serial("A"));
        let AfterEnd::DisarmIfIdle(mark) = s.ended(1).then else {
            panic!("expected an idle disarm");
        };
        assert!(s.idle_since(mark));
        // A new `start` within the idle time keeps it open for that request.
        s.started();
        assert!(!s.idle_since(mark));
    }

    // The old session lingers (its link closed, the grace not over) while the shim already
    // sent `start` for the next connection: its end must not close the gate on that one.
    #[test]
    fn a_late_end_does_not_disarm_a_newer_start() {
        let mut s = State::default();
        s.started();
        s.spawned(1, serial("A"));
        assert!(s.projected(1));
        s.started();
        let end = s.ended(1);
        assert!(end.current);
        assert!(matches!(end.then, AfterEnd::DisarmIfIdle(_)));
    }

    // Step 3 run 49: the first accessory after a switch was dropped and its end came after
    // the phone's next accessory had spawned. That end must leave the gate to the new session.
    #[test]
    fn a_superseded_session_ending_leaves_the_gate_to_the_newer_one() {
        let mut s = State::default();
        s.started();
        s.spawned(6, serial("A"));
        s.spawned(7, serial("A"));
        let end = s.ended(6);
        assert!(!end.current);
        assert_eq!(end.then, AfterEnd::Keep);
        assert!(s.projected(7));
        assert_eq!(s.ended(7), Ended { current: true, then: AfterEnd::Disarm });
    }

    #[test]
    fn the_idle_disarm_never_fires_under_a_running_session() {
        let mut s = State::default();
        s.started();
        s.spawned(1, serial("A"));
        let AfterEnd::DisarmIfIdle(mark) = s.ended(1).then else {
            panic!("expected an idle disarm");
        };
        assert!(s.idle_since(mark));
        s.spawned(2, serial("A"));
        assert!(!s.idle_since(mark));
    }

    #[test]
    fn an_older_session_ending_is_not_news_to_the_shim() {
        let mut s = State::default();
        s.started();
        s.spawned(1, serial("A"));
        s.spawned(2, serial("A"));
        assert_eq!(s.active(), Some(2));
        assert!(!s.ended(1).current);
        assert_eq!(s.active(), Some(2));
        assert!(s.ended(2).current);
    }

    #[test]
    fn stop_is_bound_to_the_session_it_was_meant_for() {
        let mut s = State::default();
        s.started();
        s.spawned(4, serial("SER4"));
        let plan = s.stop();
        let generation = s.generation();
        assert_eq!(
            plan,
            StopPlan {
                disconnect: Some(4),
                cancel_now: false,
                cancel_later: Some(("SER4".to_string(), generation)),
            }
        );
        assert_eq!(s.active(), None);
        assert!(s.current(generation));
        // The shim starts again within the grace: the delayed cancel must not run, it would
        // take down the new bring-up.
        s.started();
        assert!(!s.current(generation));
    }

    #[test]
    fn stop_before_any_session_cancels_the_bring_up_at_once() {
        let mut s = State::default();
        s.started();
        assert_eq!(
            s.stop(),
            StopPlan {
                disconnect: None,
                cancel_now: true,
                cancel_later: None,
            }
        );
    }

    #[test]
    fn a_session_without_a_serial_is_cancelled_by_the_catch_all() {
        let mut s = State::default();
        s.spawned(1, None);
        assert_eq!(s.stop().cancel_later.map(|(serial, _)| serial), Some(String::new()));
    }

    #[test]
    fn the_stopped_session_ending_reports_and_leaves_the_gate_to_idle() {
        let mut s = State::default();
        s.started();
        s.spawned(1, serial("A"));
        assert!(s.projected(1));
        let _ = s.stop();
        let end = s.ended(1);
        // The shim hears "disconnected" (no newer session); stop already disarmed.
        assert!(end.current);
        assert!(matches!(end.then, AfterEnd::DisarmIfIdle(_)));
    }
}
