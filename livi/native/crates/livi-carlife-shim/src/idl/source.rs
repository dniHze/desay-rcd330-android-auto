// Generated from aa-carlife-shim/src/source_idl.hpp by gen_tables.py (method IDs and signatures, in
// declaration order: RCF numbers methods from 0, a placeholder keeps a position).
#![allow(dead_code)]

use super::Method;

pub const SOURCE_SERVICE: &str = "SVP_SourceService";
pub const SOURCE_SERVICE_REGISTER_CLIENT: i32 = 0;
pub const SOURCE_SERVICE_REQUEST_SOURCE: i32 = 1;
pub const SOURCE_SERVICE_RELEASE_SOURCE: i32 = 2;
pub const SOURCE_SERVICE_CONFIRM_SOURCE: i32 = 3;
pub const SOURCE_SERVICE_SET_SOURCE_STATUS: i32 = 4;
pub const SOURCE_SERVICE_GET_SOURCE_STATUS: i32 = 5;
pub const SOURCE_SERVICE_SET_IPOD_ATTATCH_FILTER_FLAG: i32 = 6;
pub const SOURCE_SERVICE_SET_DEVICE_ATTACH_FILTER_FLAG: i32 = 7;
pub const SOURCE_SERVICE_GET_LAST_BG_SOURCE: i32 = 8;
pub const SOURCE_SERVICE_METHODS: &[Method] = &[
    Method { id: SOURCE_SERVICE_REGISTER_CLIENT, name: "RegisterClient", sig: "sig<RCF::Void, int>" },
    Method { id: SOURCE_SERVICE_REQUEST_SOURCE, name: "RequestSource", sig: "sig<int, int, int>" },
    Method { id: SOURCE_SERVICE_RELEASE_SOURCE, name: "ReleaseSource", sig: "sig<int, int, int, int>" },
    Method { id: SOURCE_SERVICE_CONFIRM_SOURCE, name: "ConfirmSource", sig: "sig<int, int>" },
    Method { id: SOURCE_SERVICE_SET_SOURCE_STATUS, name: "SetSourceStatus", sig: "sig<int, int, SVPSource::SourceStatus>" },
    Method { id: SOURCE_SERVICE_GET_SOURCE_STATUS, name: "GetSourceStatus", sig: "sig<SVPSource::SourceStatus, int>" },
    Method { id: SOURCE_SERVICE_SET_IPOD_ATTATCH_FILTER_FLAG, name: "SetIpodAttatchFilterFlag", sig: "sig<RCF::Void>" },
    Method { id: SOURCE_SERVICE_SET_DEVICE_ATTACH_FILTER_FLAG, name: "SetDeviceAttachFilterFlag", sig: "sig<RCF::Void, int, int>" },
    Method { id: SOURCE_SERVICE_GET_LAST_BG_SOURCE, name: "GetLastBGSource", sig: "sig<int>" },
];

pub const SOURCE_CALLBACK: &str = "SVP_SourceCallback";
pub const SOURCE_CALLBACK_CB_SOURCE_OP: i32 = 0;
pub const SOURCE_CALLBACK_METHODS: &[Method] = &[
    Method { id: SOURCE_CALLBACK_CB_SOURCE_OP, name: "cbSourceOp", sig: "sig<int, int, SVPSource::SourceOp>" },
];

pub const SOURCE_LISTENER: &str = "SVP_SourceListener";
pub const SOURCE_LISTENER_PUB_SOURCE_STATUS: i32 = 0;
pub const SOURCE_LISTENER_METHODS: &[Method] = &[
    Method { id: SOURCE_LISTENER_PUB_SOURCE_STATUS, name: "pubSourceStatus", sig: "sig<RCF::Void, int, SVPSource::SourceStatus>" },
];
