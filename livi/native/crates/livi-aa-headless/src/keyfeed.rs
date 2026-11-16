// SPDX-License-Identifier: GPL-3.0-or-later
//! Key-socket feed: device steering-wheel / panel media keys -> AA InputCommand.
//!
//! The aa-bridge SVP shim registers as devicemgr's ISVPInputListener (exactly as
//! carplayservice does) and writes one line per event to AA_KEY_SOCK:
//!
//!     "<name> <down|up>\n"
//!
//! where <name> is a devicemgr KEY_* token (or a short alias). The headless maps it to a
//! `livi_aa_stack::commands::InputCommand` and injects it on the active session as
//! `SessionCmd::Input`, which the stack already turns into an AA KeyEvent on ch INPUT
//! (the same path CarPlay uses to reach the phone, via iAP2 HID, on the stock unit).
//!
//! Volume/mute are HU-local on this platform (the head unit owns the amp), so they are
//! mapped but should only be relayed when the caller opts in (AA_RELAY_VOLUME=1).
//!
//! Staging module: the run-loop wiring (key socket -> SessionCmd::Input) lands once its
//! design is settled; until then the mapping below is
//! self-contained and unit-tested.
#![allow(dead_code)]

use livi_aa_stack::commands::InputCommand;

/// True for commands that should only be relayed to the phone when volume relay is opted in.
pub fn is_volume(cmd: InputCommand) -> bool {
    matches!(
        cmd,
        InputCommand::VolumeUp | InputCommand::VolumeDown | InputCommand::Mute
    )
}

/// Parse one wire line into (command, down). Returns None for a blank line or an unknown /
/// non-relayed key token. `down` defaults to true when the second field is absent (a tap).
pub fn parse_line(line: &str) -> Option<(InputCommand, bool)> {
    let mut it = line.split_whitespace();
    let name = it.next()?;
    let down = !matches!(it.next(), Some("up") | Some("0") | Some("release"));
    Some((map_key(name)?, down))
}

/// devicemgr KEY_* token (or short alias) -> InputCommand.
pub fn map_key(name: &str) -> Option<InputCommand> {
    use InputCommand as C;
    Some(match name {
        "KEY_NEXT" | "next" => C::Next,
        "KEY_PREV" | "prev" => C::Previous,
        "KEY_PLAY" | "play" => C::Play,
        "KEY_PAUSE" | "pause" => C::Pause,
        "KEY_PLAY_PAUSE" | "playpause" => C::PlayPause,
        "KEY_FAST_FORWARD" | "ff" => C::FastForward,
        "KEY_FAST_REVERSE" | "rw" => C::Rewind,
        "KEY_MFL_PHONE" | "call" => C::AcceptCall,
        "KEY_PHONE_HANG_UP" | "hangup" => C::RejectCall,
        "KEY_MFL_PTT" | "voice" => C::VoiceAssistant,
        "KEY_MUTE" | "mute" => C::Mute,
        "KEY_VOLUME_UP" | "volup" => C::VolumeUp,
        "KEY_VOLUME_DOWN" | "voldn" => C::VolumeDown,
        _ => return None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use livi_aa_stack::commands::InputCommand as C;

    #[test]
    fn maps_swc_and_media_keys() {
        assert_eq!(parse_line("KEY_NEXT down"), Some((C::Next, true)));
        assert_eq!(parse_line("KEY_PREV up"), Some((C::Previous, false)));
        assert_eq!(parse_line("voice"), Some((C::VoiceAssistant, true)));
        assert_eq!(parse_line("KEY_MFL_PHONE down"), Some((C::AcceptCall, true)));
        assert_eq!(parse_line("KEY_PHONE_HANG_UP down"), Some((C::RejectCall, true)));
        assert_eq!(parse_line("playpause"), Some((C::PlayPause, true)));
    }

    #[test]
    fn tap_defaults_to_down_and_aliases_work() {
        assert_eq!(parse_line("next"), Some((C::Next, true)));
        assert_eq!(parse_line("next 1"), Some((C::Next, true)));
        assert_eq!(parse_line("next 0"), Some((C::Next, false)));
    }

    #[test]
    fn unknown_and_blank_are_ignored() {
        assert_eq!(parse_line("KEY_UNKNOW down"), None);
        assert_eq!(parse_line(""), None);
        assert_eq!(parse_line("   "), None);
    }

    #[test]
    fn volume_is_flagged_for_opt_in() {
        assert!(is_volume(C::VolumeUp));
        assert!(is_volume(C::Mute));
        assert!(!is_volume(C::Next));
        assert!(!is_volume(C::VoiceAssistant));
    }
}
