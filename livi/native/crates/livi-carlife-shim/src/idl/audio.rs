// Generated from aa-carlife-shim/src/audio_idl.hpp by gen_tables.py (method IDs and signatures, in
// declaration order: RCF numbers methods from 0, a placeholder keeps a position).
#![allow(dead_code)]

use super::Method;

pub const AUDIO_SERVICE: &str = "ISVP_AudioService";
pub const AUDIO_SERVICE_REGISTER_AUDIO: i32 = 14;
pub const AUDIO_SERVICE_METHODS: &[Method] = &[
    Method { id: AUDIO_SERVICE_REGISTER_AUDIO, name: "RegisterAudio", sig: "sig<int, std::string, unsigned int>" },
];
