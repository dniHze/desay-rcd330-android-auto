// Generated from aa-carlife-shim/src/carlife_idl.hpp by gen_tables.py (method IDs and signatures, in
// declaration order: RCF numbers methods from 0, a placeholder keeps a position).
#![allow(dead_code)]

use super::Method;

pub const CAR_LIFE_SERVICE: &str = "ISVP_CarLifeService";
pub const CAR_LIFE_SERVICE_PLAY: i32 = 0;
pub const CAR_LIFE_SERVICE_PAUSE: i32 = 1;
pub const CAR_LIFE_SERVICE_SHOW: i32 = 2;
pub const CAR_LIFE_SERVICE_START_APP: i32 = 3;
pub const CAR_LIFE_SERVICE_CAR_LIFE_CONNECTION: i32 = 4;
pub const CAR_LIFE_SERVICE_CAR_LIFE_DIS_CONNECTION: i32 = 5;
pub const CAR_LIFE_SERVICE_SINGLE_TOUCH_INFO: i32 = 6;
pub const CAR_LIFE_SERVICE_ROTATE_KEY_EVENT: i32 = 7;
pub const CAR_LIFE_SERVICE_GET_CAR_LIFE_CONNECT_STATUS: i32 = 8;
pub const CAR_LIFE_SERVICE_GO_TO_FORE_GROUND: i32 = 9;
pub const CAR_LIFE_SERVICE_NEED_BLOCK_BT_POPUP: i32 = 10;
pub const CAR_LIFE_SERVICE_NEED_SWITCH_CAR_LIFE_APP_TO_FG: i32 = 11;
pub const CAR_LIFE_SERVICE_GET_MODULE_STATUS: i32 = 12;
pub const CAR_LIFE_SERVICE_SET_SOURCE_AVAILABLE: i32 = 13;
pub const CAR_LIFE_SERVICE_SEND_MODULE_CONTROL_CMD: i32 = 14;
pub const CAR_LIFE_SERVICE_GET_CONNECT_CARLIFE_PHONE_BT_ADDRESS: i32 = 15;
pub const CAR_LIFE_SERVICE_CONNECT_CARLIFE_PHONE_ISI_OS: i32 = 16;
pub const CAR_LIFE_SERVICE_CLEAR_CAR_LIFE_STATUS: i32 = 17;
pub const CAR_LIFE_SERVICE_NEED_CHANGE_HAND_FREE_MODE: i32 = 18;
pub const CAR_LIFE_SERVICE_METHODS: &[Method] = &[
    Method { id: CAR_LIFE_SERVICE_PLAY, name: "Play", sig: "sig<int, int>" },
    Method { id: CAR_LIFE_SERVICE_PAUSE, name: "Pause", sig: "sig<int, int>" },
    Method { id: CAR_LIFE_SERVICE_SHOW, name: "Show", sig: "sig<int, int>" },
    Method { id: CAR_LIFE_SERVICE_START_APP, name: "StartApp", sig: "sig<int, int>" },
    Method { id: CAR_LIFE_SERVICE_CAR_LIFE_CONNECTION, name: "CarLifeConnection", sig: "sig<int>" },
    Method { id: CAR_LIFE_SERVICE_CAR_LIFE_DIS_CONNECTION, name: "CarLifeDisConnection", sig: "sig<int>" },
    Method { id: CAR_LIFE_SERVICE_SINGLE_TOUCH_INFO, name: "SingleTouchInfo", sig: "sig<RCF::Void, unsigned int, unsigned int, S_CL_TOUCH_EVENT>" },
    Method { id: CAR_LIFE_SERVICE_ROTATE_KEY_EVENT, name: "RotateKeyEvent", sig: "sig<RCF::Void, unsigned int, int>" },
    Method { id: CAR_LIFE_SERVICE_GET_CAR_LIFE_CONNECT_STATUS, name: "GetCarLifeConnectStatus", sig: "sig<S_CL_CONNECT_STATUS>" },
    Method { id: CAR_LIFE_SERVICE_GO_TO_FORE_GROUND, name: "GoToForeGround", sig: "sig<RCF::Void>" },
    Method { id: CAR_LIFE_SERVICE_NEED_BLOCK_BT_POPUP, name: "NeedBlockBTPopup", sig: "sig<int>" },
    Method { id: CAR_LIFE_SERVICE_NEED_SWITCH_CAR_LIFE_APP_TO_FG, name: "NeedSwitchCarLifeAppToFG", sig: "sig<int>" },
    Method { id: CAR_LIFE_SERVICE_GET_MODULE_STATUS, name: "GetModuleStatus", sig: "sig<unsigned int, unsigned int>" },
    Method { id: CAR_LIFE_SERVICE_SET_SOURCE_AVAILABLE, name: "SetSourceAvailable", sig: "sig<RCF::Void, int>" },
    Method { id: CAR_LIFE_SERVICE_SEND_MODULE_CONTROL_CMD, name: "SendModuleControlCmd", sig: "sig<RCF::Void, unsigned int, unsigned int>" },
    Method { id: CAR_LIFE_SERVICE_GET_CONNECT_CARLIFE_PHONE_BT_ADDRESS, name: "GetConnectCarlifePhoneBtAddress", sig: "sig<std::string>" },
    Method { id: CAR_LIFE_SERVICE_CONNECT_CARLIFE_PHONE_ISI_OS, name: "ConnectCarlifePhoneIsiOS", sig: "sig<int>" },
    Method { id: CAR_LIFE_SERVICE_CLEAR_CAR_LIFE_STATUS, name: "ClearCarLifeStatus", sig: "sig<RCF::Void>" },
    Method { id: CAR_LIFE_SERVICE_NEED_CHANGE_HAND_FREE_MODE, name: "NeedChangeHandFreeMode", sig: "sig<int>" },
];

pub const CAR_LIFE_LISTENER: &str = "ISVP_CarLifeListener";
pub const CAR_LIFE_LISTENER_ON_APP_STATUS_CHANGE: i32 = 0;
pub const CAR_LIFE_LISTENER_ON_CAR_LIFE_CONNECT_STATUS_CHANGE: i32 = 1;
pub const CAR_LIFE_LISTENER_ON_CONNECTING_STATUS_PERCENTAGE: i32 = 2;
pub const CAR_LIFE_LISTENER_ON_PHONE_STATUS_CHANGE: i32 = 3;
pub const CAR_LIFE_LISTENER_ON_TELEPHONE_STATUS_CHANGE: i32 = 4;
pub const CAR_LIFE_LISTENER_ON_SCREEN_STATUS_CHANGE: i32 = 5;
pub const CAR_LIFE_LISTENER_ON_CONNECT_EXCEPTION: i32 = 6;
pub const CAR_LIFE_LISTENER_ON_GO_TO_DESK_TOP: i32 = 7;
pub const CAR_LIFE_LISTENER_ON_MODULE_STATUS_CHANGE: i32 = 8;
pub const CAR_LIFE_LISTENER_ON_BT_CALLING_AND_PAUSE_CARLIFE_CONNECT: i32 = 9;
pub const CAR_LIFE_LISTENER_METHODS: &[Method] = &[
    Method { id: CAR_LIFE_LISTENER_ON_APP_STATUS_CHANGE, name: "OnAppStatusChange", sig: "sig<RCF::Void, S_CL_APP_STATUS>" },
    Method { id: CAR_LIFE_LISTENER_ON_CAR_LIFE_CONNECT_STATUS_CHANGE, name: "OnCarLifeConnectStatusChange", sig: "sig<RCF::Void, S_CL_CONNECT_STATUS, S_CL_CONNECT_ERROR>" },
    Method { id: CAR_LIFE_LISTENER_ON_CONNECTING_STATUS_PERCENTAGE, name: "OnConnectingStatusPercentage", sig: "sig<RCF::Void, int>" },
    Method { id: CAR_LIFE_LISTENER_ON_PHONE_STATUS_CHANGE, name: "OnPhoneStatusChange", sig: "sig<RCF::Void, S_CL_PHONE_STATUS>" },
    Method { id: CAR_LIFE_LISTENER_ON_TELEPHONE_STATUS_CHANGE, name: "OnTelephoneStatusChange", sig: "sig<RCF::Void, S_CL_TELEPHONE_STATUS>" },
    Method { id: CAR_LIFE_LISTENER_ON_SCREEN_STATUS_CHANGE, name: "OnScreenStatusChange", sig: "sig<RCF::Void, S_CL_SCREEN_STATUS>" },
    Method { id: CAR_LIFE_LISTENER_ON_CONNECT_EXCEPTION, name: "OnConnectException", sig: "sig<RCF::Void, S_CL_EXCEPTION_TYPE>" },
    Method { id: CAR_LIFE_LISTENER_ON_GO_TO_DESK_TOP, name: "OnGoToDeskTop", sig: "sig<RCF::Void>" },
    Method { id: CAR_LIFE_LISTENER_ON_MODULE_STATUS_CHANGE, name: "OnModuleStatusChange", sig: "sig<RCF::Void, unsigned int, unsigned int>" },
    Method { id: CAR_LIFE_LISTENER_ON_BT_CALLING_AND_PAUSE_CARLIFE_CONNECT, name: "OnBtCallingAndPauseCarlifeConnect", sig: "sig<RCF::Void, int>" },
];
