// SPDX-License-Identifier: GPL-3.0-or-later
//
// The RCF interfaces the shim serves and calls, as the C++ shim's IDL headers declare them
// (aa-carlife-shim/src/*_idl.hpp; see each header for how the IDs, types and ports were
// recovered from the stock binaries). The method tables (<name>.rs) are transcribed from the
// headers' RCF_METHOD lines; tests/idcheck.rs checks them against tools/stock_ids.txt, as
// build.sh's method-ID check does for the C++ build. Enums travel as int32, so their values
// are plain i32 constants here.

#![allow(dead_code)]

pub mod audio;
pub mod bt;
pub mod can;
pub mod carlife;
pub mod ilm;
pub mod location;
pub mod power;
pub mod rmt;
pub mod source;

use rcf_lite::sf_struct;

/// One method: its fnId, its name, and its parameter types as tools/rcf_ids.py prints them.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Method {
    pub id: i32,
    pub name: &'static str,
    pub sig: &'static str,
}

/// Every interface: its RCF name and its methods.
pub const TABLES: &[(&str, &[Method])] = &[
    (audio::AUDIO_SERVICE, audio::AUDIO_SERVICE_METHODS),
    (bt::BT_SERVICE, bt::BT_SERVICE_METHODS),
    (bt::BT_LISTENER, bt::BT_LISTENER_METHODS),
    (can::CAN_SERVICE, can::CAN_SERVICE_METHODS),
    (can::CAN_LISTENER, can::CAN_LISTENER_METHODS),
    (carlife::CAR_LIFE_SERVICE, carlife::CAR_LIFE_SERVICE_METHODS),
    (carlife::CAR_LIFE_LISTENER, carlife::CAR_LIFE_LISTENER_METHODS),
    (ilm::ILM_SERVICE, ilm::ILM_SERVICE_METHODS),
    (location::LOCATION_SERVICE, location::LOCATION_SERVICE_METHODS),
    (location::LOCATION_NOTIFY, location::LOCATION_NOTIFY_METHODS),
    (power::POWER_SERVICE, power::POWER_SERVICE_METHODS),
    (power::POWER_LISTENER, power::POWER_LISTENER_METHODS),
    (rmt::RMT_SERVICE, rmt::RMT_SERVICE_METHODS),
    (rmt::RMT_CALLBACK, rmt::RMT_CALLBACK_METHODS),
    (source::SOURCE_SERVICE, source::SOURCE_SERVICE_METHODS),
    (source::SOURCE_CALLBACK, source::SOURCE_CALLBACK_METHODS),
    (source::SOURCE_LISTENER, source::SOURCE_LISTENER_METHODS),
];

// ---- carlife_idl.hpp ---------------------------------------------------------------------
// Stock endpoints (SVPCarLifeService_Server::SVPCarLifeService_Server and
// SVPCarLifeService_Client::InitListener).
pub const CARLIFE_SERVICE_PORT: i32 = 30407; // RcfServer bound to 0.0.0.0 on stock
pub const CARLIFE_PUBLISHER_PORT: i32 = 30408; // RcfServer bound to 127.0.0.1 on stock

// What stock carlifeservice publishes, as (status, error) (carlife_idl.hpp has where each comes
// from). appconnect acts on 1..5 and ignores anything else.
pub const CL_ST_NONE: i32 = 0;
pub const CL_ST_CONNECT_FAILED: i32 = 1;
pub const CL_ST_CONNECTING: i32 = 2;
pub const CL_ST_VIDEO: i32 = 3;
pub const CL_ST_DISCONNECTED: i32 = 4;
pub const CL_ST_TIMEOUT: i32 = 5;
// Stock publishes 0 with 2, 3 and a normal 4, and 1 with every failure (1, 4 and 5).
pub const CL_ERR_NONE: i32 = 0;
pub const CL_ERR_FAILED: i32 = 1;

// ---- ilm_idl.hpp -------------------------------------------------------------------------
pub const ILM_SERVICE_PORT: i32 = 30005;
/// The ILM surface id a process's video gets: the unit's waylandsink creates it with
/// ilm_surfaceCreate(getpid() + 20000000), the same convention stock carlifeservice uses.
pub const ILM_VIDEO_SURFACE_BASE: u32 = 20000000;

// ---- audio_idl.hpp -----------------------------------------------------------------------
pub const AUDIO_SERVICE_PORT: i32 = 30203;
/// RegisterAudio's second argument as stock passes it (every process).
pub const REGISTER_ALL_PIDS: u32 = 0xffffffff;

// ---- power_idl.hpp -----------------------------------------------------------------------
pub const POWER_SERVICE_PORT: i32 = 30201;
pub const POWER_LISTENER_PORT: i32 = 30202;
pub const PS_USER_ON: i32 = 3;
pub const PS_USER_OFF: i32 = 4;
pub const PS_SHUT_DOWN: i32 = 9;
pub const PS_RESET: i32 = 10;
pub const PS_UNDEFINED: i32 = 11;

/// The power state's name, as libpower's and powermgr's table has it.
pub fn power_state_name(s: i32) -> &'static str {
    const NAMES: [&str; 12] = [
        "?",
        "FIRST_CONNECT_START",
        "FIRST_CONNECT_COMPLETE",
        "USER_ON",
        "USER_OFF",
        "LOGO_ON",
        "LOGO_OFF",
        "SCREEN_LOCK",
        "SCREEN_UNLOCK",
        "SHUT_DOWN",
        "RESET",
        "UNDEFINED",
    ];
    if (0..=PS_UNDEFINED).contains(&s) { NAMES[s as usize] } else { "?" }
}

// ---- source_idl.hpp ----------------------------------------------------------------------
pub const SOURCE_SERVICE_PORT: i32 = 30205;
pub const SOURCE_LISTENER_PORT: i32 = 30206;
// SourceStatus ordinals (libsource's SVPSource::ToString table).
pub const SS_ATTACHED: i32 = 1;
pub const SS_VALID: i32 = 2;
pub const SS_ACTIVED: i32 = 4;
pub const SS_DETACHED: i32 = 6;
pub const SS_RESTORED: i32 = 7;
pub const SS_INBACKGROUND: i32 = 8;
// SourceOp (SVPSourceService_Client::cbSourceOp's dispatch).
pub const OP_PLAY: i32 = 0;
pub const OP_PAUSE: i32 = 1;
pub const OP_STOP: i32 = 2;
// libsource's SVPSource::ToString(SourceID) table.
pub const SID_CARLIFE_MEDIA: i32 = 34;
pub const SID_CARLIFE_NAVI: i32 = 35;
pub const SID_CARLIFE_PROMPT: i32 = 36;
pub const SID_CARLIFE_ASR: i32 = 37;

// ---- can_idl.hpp -------------------------------------------------------------------------
pub const CAN_SERVICE_PORT: i32 = 30705; // RcfClient<SVP_CanService>
pub const CAN_LISTENER_PUBLISHER_PORT: i32 = 30706; // SubscriptionParms::setPublisherEndpoint

sf_struct!(
    /// GetCanCarplayInfo / OnCanCarplayInfoChanged. Wire: 3 x unsigned char, in this order.
    CanCarplayInfo { day_night: u8, transmission_state: u8, right_hand_drive: u8 }
);

// ---- bt_idl.hpp --------------------------------------------------------------------------
pub const BT_SERVICE_PORT: i32 = 30315; // RcfClient<SVP_BtService>
pub const BT_LISTENER_PORT: i32 = 30316; // SVP_BtListener publisher (127.0.0.1)
// ECNR_AUDIO_PATH: btservice rejects anything > 1.
pub const EAP_BT_MODE: i32 = 0; // default: the BT module keeps the mic for HFP
pub const EAP_WIRED_PHONE_MODE: i32 = 1; // wired projection (CarPlay, CarLife VR, MirrorLink ASR)
// GetDeviceNum / GetDevice lists.
pub const BT_DEVICE_PAIRED: i32 = 1;
// Connect / Disconnect: 1 HFP (+ phonebook, SMS), 2 A2DP/AVRCP, 3 all.
pub const BT_SERVICE_A2DP: i32 = 2;

sf_struct!(
    /// libbt SF::Serializer<SVPBtDevice>::serializeContents (0xd3c60), in this order; filled by
    /// SVPBtServer::GetDevice (0x183ee4). The flags and the last field are not named.
    BtDevice {
        name: String,
        address: String, // "xx:xx:xx:xx:xx:xx", lowercase
        b8: bool,
        b9: bool,
        b10: bool,
        svc12: i32,
        svc16: i32,
        u20: u32,
    }
);
