// Generated from aa-carlife-shim/src/rmt_idl.hpp by gen_tables.py (method IDs and signatures, in
// declaration order: RCF numbers methods from 0, a placeholder keeps a position).
#![allow(dead_code)]

use super::Method;

pub const RMT_SERVICE: &str = "SVP_RmtService";
pub const RMT_SERVICE_SET_MESSAGE_FILTER: i32 = 0;
pub const RMT_SERVICE_SEND_MESSAGE: i32 = 1;
pub const RMT_SERVICE_METHODS: &[Method] = &[
    Method { id: RMT_SERVICE_SET_MESSAGE_FILTER, name: "SetMessageFilter", sig: "sig<RCF::Void, unsigned int>" },
    Method { id: RMT_SERVICE_SEND_MESSAGE, name: "SendMessage", sig: "sig<int, std::vector<unsigned char, std::allocator<unsigned char> >&>" },
];

pub const RMT_CALLBACK: &str = "SVP_RmtCallback";
pub const RMT_CALLBACK_PROCESS_MESSAGE: i32 = 0;
pub const RMT_CALLBACK_METHODS: &[Method] = &[
    Method { id: RMT_CALLBACK_PROCESS_MESSAGE, name: "ProcessMessage", sig: "sig<RCF::Void, std::vector<unsigned char, std::allocator<unsigned char> >&>" },
];
