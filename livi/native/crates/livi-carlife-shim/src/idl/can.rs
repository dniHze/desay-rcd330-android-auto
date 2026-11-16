// Generated from aa-carlife-shim/src/can_idl.hpp by gen_tables.py (method IDs and signatures, in
// declaration order: RCF numbers methods from 0, a placeholder keeps a position).
#![allow(dead_code)]

use super::Method;

pub const CAN_SERVICE: &str = "SVP_CanService";
pub const CAN_SERVICE_GET_CAN_IGNITION_MODE: i32 = 0;
pub const CAN_SERVICE_GET_CAN_IGNITION_MODE_1: i32 = 1;
pub const CAN_SERVICE_GET_CAN_VEHICLE_SPEED: i32 = 2;
pub const CAN_SERVICE_GET_CAN_CARPLAY_INFO: i32 = 3;
pub const CAN_SERVICE_GET_CAN_AUDIO_LOWERING_PRESENT: i32 = 4;
pub const CAN_SERVICE_GET_DESC_ADAPTION_BT_SETTING: i32 = 6;
pub const CAN_SERVICE_GET_DESC_ADAPTION_VEHICLE_CFG: i32 = 7;
pub const CAN_SERVICE_GET_DESC_ADAPTION_STICKER_BITS: i32 = 8;
pub const CAN_SERVICE_GET_DESC_CODING_VEHICLE_IMAGE: i32 = 10;
pub const CAN_SERVICE_GET_DESC_CODING_BT_SETTING: i32 = 11;
pub const CAN_SERVICE_GET_DESC_CODING_HMI_SKIN: i32 = 12;
pub const CAN_SERVICE_GET_DESC_CODING_START_SCREEN: i32 = 13;
pub const CAN_SERVICE_SET_DESC_ACTUATOR_TEST_DONE: i32 = 14;
pub const CAN_SERVICE_SET_DESC_DATASET_DOWNLOAD_DONE: i32 = 15;
pub const CAN_SERVICE_SET_DESC_ROUTINE_TEST_DONE: i32 = 16;
pub const CAN_SERVICE_SET_DESC_MEASURED_VALUE: i32 = 17;
pub const CAN_SERVICE_GET_DESC_CAR_FUNC_ADAPTION: i32 = 18;
pub const CAN_SERVICE_SET_BAP_SCREEN_VIEW_REQUEST: i32 = 19;
pub const CAN_SERVICE_SET_BAP_SCREEN_VIEW_OPTION_REQUEST: i32 = 20;
pub const CAN_SERVICE_SET_BAP_SCREEN_VAULE_REQUEST: i32 = 21;
pub const CAN_SERVICE_SET_BAP_PHONE_DATA_INFO: i32 = 22;
pub const CAN_SERVICE_GET_BAP_STATUS_BAR_VAL: i32 = 23;
pub const CAN_SERVICE_SET_BAP_SCREEN_ITEM_VAL: i32 = 24;
pub const CAN_SERVICE_SET_BAP_SCREEN_ITEM_PARAMS: i32 = 25;
pub const CAN_SERVICE_GET_BAP_CLOCK_FORMAT: i32 = 26;
pub const CAN_SERVICE_SET_BAP_WAKEUP_FLAG: i32 = 27;
pub const CAN_SERVICE_SET_BAP_SCREEN_OFF_TIMER: i32 = 28;
pub const CAN_SERVICE_GET_BAP_RVC_OPS_STATE: i32 = 29;
pub const CAN_SERVICE_SEND_CAN_SERVICE_MESSAGE: i32 = 30;
pub const CAN_SERVICE_METHODS: &[Method] = &[
    Method { id: CAN_SERVICE_GET_CAN_IGNITION_MODE, name: "GetCanIgnitionMode", sig: "sig<RCF::Void>" },
    Method { id: CAN_SERVICE_GET_CAN_IGNITION_MODE_1, name: "GetCanIgnitionMode", sig: "sig<RCF::Void, SVPCanType::SVPCanIgnModeType&>" },
    Method { id: CAN_SERVICE_GET_CAN_VEHICLE_SPEED, name: "GetCanVehicleSpeed", sig: "sig<RCF::Void>" },
    Method { id: CAN_SERVICE_GET_CAN_CARPLAY_INFO, name: "GetCanCarplayInfo", sig: "sig<RCF::Void, SVPCanType::SVPCanCarplayInfoType&>" },
    Method { id: CAN_SERVICE_GET_CAN_AUDIO_LOWERING_PRESENT, name: "GetCanAudioLoweringPresent", sig: "sig<int>" },
    Method { id: CAN_SERVICE_GET_DESC_ADAPTION_BT_SETTING, name: "GetDescAdaptionBtSetting", sig: "sig<RCF::Void, SVPCanType::SVPDescAdaptionBtSettingType&>" },
    Method { id: CAN_SERVICE_GET_DESC_ADAPTION_VEHICLE_CFG, name: "GetDescAdaptionVehicleCfg", sig: "sig<RCF::Void, SVPCanType::SVPDescAdaptionVehicleConfigType&>" },
    Method { id: CAN_SERVICE_GET_DESC_ADAPTION_STICKER_BITS, name: "GetDescAdaptionStickerBits", sig: "sig<RCF::Void, unsigned short&>" },
    Method { id: CAN_SERVICE_GET_DESC_CODING_VEHICLE_IMAGE, name: "GetDescCodingVehicleImage", sig: "sig<RCF::Void, SVPCanType::SVPDescCodingVehicleImageType&>" },
    Method { id: CAN_SERVICE_GET_DESC_CODING_BT_SETTING, name: "GetDescCodingBtSetting", sig: "sig<RCF::Void, SVPCanType::SVPDescCodingBtSettingType&>" },
    Method { id: CAN_SERVICE_GET_DESC_CODING_HMI_SKIN, name: "GetDescCodingHmiSkin", sig: "sig<RCF::Void, unsigned char&>" },
    Method { id: CAN_SERVICE_GET_DESC_CODING_START_SCREEN, name: "GetDescCodingStartScreen", sig: "sig<RCF::Void, unsigned char&>" },
    Method { id: CAN_SERVICE_SET_DESC_ACTUATOR_TEST_DONE, name: "SetDescActuatorTestDone", sig: "sig<RCF::Void, SVPCanType::SVPDescActuatorIdType, SVPCanType::SVPDescActuatorCtrlType>" },
    Method { id: CAN_SERVICE_SET_DESC_DATASET_DOWNLOAD_DONE, name: "SetDescDatasetDownloadDone", sig: "sig<RCF::Void, SVPCanType::SVPDescDatasetIdType>" },
    Method { id: CAN_SERVICE_SET_DESC_ROUTINE_TEST_DONE, name: "SetDescRoutineTestDone", sig: "sig<RCF::Void, SVPCanType::SVPDescRoutineIdType, SVPCanType::SVPDescRoutineCtrlType, unsigned char>" },
    Method { id: CAN_SERVICE_SET_DESC_MEASURED_VALUE, name: "SetDescMeasuredValue", sig: "sig<RCF::Void, SVPCanType::SVPDescMeasuredIdType, std::vector<unsigned char, std::allocator<unsigned char> >&>" },
    Method { id: CAN_SERVICE_GET_DESC_CAR_FUNC_ADAPTION, name: "GetDescCarFuncAdaption", sig: "sig<RCF::Void, std::vector<unsigned char, std::allocator<unsigned char> >&>" },
    Method { id: CAN_SERVICE_SET_BAP_SCREEN_VIEW_REQUEST, name: "SetBapScreenViewRequest", sig: "sig<RCF::Void, SVPCanType::SVPBapScreenViewIdType, SVPCanType::SVPBapScreenCtrlType>" },
    Method { id: CAN_SERVICE_SET_BAP_SCREEN_VIEW_OPTION_REQUEST, name: "SetBapScreenViewOptionRequest", sig: "sig<RCF::Void, SVPCanType::SVPBapScreenViewIdType>" },
    Method { id: CAN_SERVICE_SET_BAP_SCREEN_VAULE_REQUEST, name: "SetBapScreenVauleRequest", sig: "sig<RCF::Void, SVPCanType::SVPBapScreenViewIdType>" },
    Method { id: CAN_SERVICE_SET_BAP_PHONE_DATA_INFO, name: "SetBapPhoneDataInfo", sig: "sig<RCF::Void, SVPCanType::SVPBapPhoneDataInfoType, std::vector<unsigned char, std::allocator<unsigned char> >&>" },
    Method { id: CAN_SERVICE_GET_BAP_STATUS_BAR_VAL, name: "GetBapStatusBarVal", sig: "sig<RCF::Void>" },
    Method { id: CAN_SERVICE_SET_BAP_SCREEN_ITEM_VAL, name: "SetBapScreenItemVal", sig: "sig<RCF::Void, SVPCanType::SVPBapScreenSetIdType, unsigned char>" },
    Method { id: CAN_SERVICE_SET_BAP_SCREEN_ITEM_PARAMS, name: "SetBapScreenItemParams", sig: "sig<RCF::Void, SVPCanType::SVPBapScreenSetParamIdType, std::vector<unsigned char, std::allocator<unsigned char> >&>" },
    Method { id: CAN_SERVICE_GET_BAP_CLOCK_FORMAT, name: "GetBapClockFormat", sig: "sig<RCF::Void, SVPCanType::SVPBapClockFormatType&>" },
    Method { id: CAN_SERVICE_SET_BAP_WAKEUP_FLAG, name: "SetBapWakeupFlag", sig: "sig<RCF::Void, SVPCanType::SVPBapWakeupFlagType, SVPCanType::SVPBapWakeupStateType>" },
    Method { id: CAN_SERVICE_SET_BAP_SCREEN_OFF_TIMER, name: "SetBapScreenOffTimer", sig: "sig<RCF::Void, SVPCanType::SVPBapScreenOffFuncType, SVPCanType::SVPBapScreenOffTimerType>" },
    Method { id: CAN_SERVICE_GET_BAP_RVC_OPS_STATE, name: "GetBapRvcOpsState", sig: "sig<RCF::Void, SVPCanType::SVPBapRvcOpsStateType&>" },
    Method { id: CAN_SERVICE_SEND_CAN_SERVICE_MESSAGE, name: "SendCanServiceMessage", sig: "sig<int, unsigned short, std::vector<unsigned char, std::allocator<unsigned char> >&>" },
];

pub const CAN_LISTENER: &str = "SVP_CanListener";
pub const CAN_LISTENER_ON_CAN_IGNITION_MODE_CHANGED: i32 = 0;
pub const CAN_LISTENER_ON_CAN_VEHICLE_SPEED_CHANGED: i32 = 1;
pub const CAN_LISTENER_ON_CAN_CARPLAY_INFO_CHANGED: i32 = 2;
pub const CAN_LISTENER_ON_CAN_AUDIO_LOWERING_CHANGE: i32 = 3;
pub const CAN_LISTENER_ON_DESC_ADAPTION_BT_SETTING_CHANGED: i32 = 4;
pub const CAN_LISTENER_ON_DESC_ADAPTION_VEHICLE_CFG_CHANGED: i32 = 5;
pub const CAN_LISTENER_ON_DESC_ADAPTION_STICKER_BITS_CHANGED: i32 = 6;
pub const CAN_LISTENER_ON_DESC_CODING_VEHICLE_IMAGE_CHANGED: i32 = 7;
pub const CAN_LISTENER_ON_DESC_CODING_BT_SETTING_CHANGED: i32 = 8;
pub const CAN_LISTENER_ON_DESC_ACTUATOR_TEST_EVENT: i32 = 9;
pub const CAN_LISTENER_ON_DESC_DATASET_DOWNLOAD_EVENT: i32 = 10;
pub const CAN_LISTENER_ON_DESC_ROUTINE_TEST_EVENT: i32 = 11;
pub const CAN_LISTENER_ON_DESC_MEASURED_VALUE_EVENT: i32 = 12;
pub const CAN_LISTENER_ON_BAP_SCREEN_VIEW_OPTION_EVENT: i32 = 13;
pub const CAN_LISTENER_ON_BAP_SCREEN_VIEW_DATA_EVENT: i32 = 14;
pub const CAN_LISTENER_ON_BAP_SCREEN_VIEW_REQUEST_EVENT: i32 = 15;
pub const CAN_LISTENER_ON_BAP_PHONE_CALL_ACTION_CHANGE: i32 = 16;
pub const CAN_LISTENER_ON_BAP_STATUS_BAR_CHANGED: i32 = 17;
pub const CAN_LISTENER_ON_BAP_FSG_STATE_CHANGED: i32 = 18;
pub const CAN_LISTENER_ON_BAP_GENERAL_POPUP_CHANGED: i32 = 19;
pub const CAN_LISTENER_ON_BAP_START_STOP_VALUE_CHANGE: i32 = 20;
pub const CAN_LISTENER_ON_BAP_SCREEN_OFF_TIMER_EVENT: i32 = 21;
pub const CAN_LISTENER_ON_CAN_SERVICE_MESSAGE: i32 = 22;
pub const CAN_LISTENER_METHODS: &[Method] = &[
    Method { id: CAN_LISTENER_ON_CAN_IGNITION_MODE_CHANGED, name: "OnCanIgnitionModeChanged", sig: "sig<RCF::Void, SVPCanType::SVPCanIgnModeType>" },
    Method { id: CAN_LISTENER_ON_CAN_VEHICLE_SPEED_CHANGED, name: "OnCanVehicleSpeedChanged", sig: "sig<RCF::Void, unsigned short>" },
    Method { id: CAN_LISTENER_ON_CAN_CARPLAY_INFO_CHANGED, name: "OnCanCarplayInfoChanged", sig: "sig<RCF::Void, SVPCanType::SVPCanCarplayInfoType>" },
    Method { id: CAN_LISTENER_ON_CAN_AUDIO_LOWERING_CHANGE, name: "OnCanAudioLoweringChange", sig: "sig<RCF::Void, int>" },
    Method { id: CAN_LISTENER_ON_DESC_ADAPTION_BT_SETTING_CHANGED, name: "OnDescAdaptionBtSettingChanged", sig: "sig<RCF::Void, SVPCanType::SVPDescAdaptionBtSettingType>" },
    Method { id: CAN_LISTENER_ON_DESC_ADAPTION_VEHICLE_CFG_CHANGED, name: "OnDescAdaptionVehicleCfgChanged", sig: "sig<RCF::Void, SVPCanType::SVPDescAdaptionVehicleConfigType>" },
    Method { id: CAN_LISTENER_ON_DESC_ADAPTION_STICKER_BITS_CHANGED, name: "OnDescAdaptionStickerBitsChanged", sig: "sig<RCF::Void, unsigned short>" },
    Method { id: CAN_LISTENER_ON_DESC_CODING_VEHICLE_IMAGE_CHANGED, name: "OnDescCodingVehicleImageChanged", sig: "sig<RCF::Void, SVPCanType::SVPDescCodingVehicleImageType>" },
    Method { id: CAN_LISTENER_ON_DESC_CODING_BT_SETTING_CHANGED, name: "OnDescCodingBtSettingChanged", sig: "sig<RCF::Void, SVPCanType::SVPDescCodingBtSettingType>" },
    Method { id: CAN_LISTENER_ON_DESC_ACTUATOR_TEST_EVENT, name: "OnDescActuatorTestEvent", sig: "sig<RCF::Void, SVPCanType::SVPDescActuatorIdType, SVPCanType::SVPDescActuatorCtrlType>" },
    Method { id: CAN_LISTENER_ON_DESC_DATASET_DOWNLOAD_EVENT, name: "OnDescDatasetDownloadEvent", sig: "sig<RCF::Void, SVPCanType::SVPDescDatasetIdType, std::vector<unsigned char, std::allocator<unsigned char> >&>" },
    Method { id: CAN_LISTENER_ON_DESC_ROUTINE_TEST_EVENT, name: "OnDescRoutineTestEvent", sig: "sig<RCF::Void, SVPCanType::SVPDescRoutineIdType, SVPCanType::SVPDescRoutineCtrlType, unsigned char>" },
    Method { id: CAN_LISTENER_ON_DESC_MEASURED_VALUE_EVENT, name: "OnDescMeasuredValueEvent", sig: "sig<RCF::Void, SVPCanType::SVPDescMeasuredIdType>" },
    Method { id: CAN_LISTENER_ON_BAP_SCREEN_VIEW_OPTION_EVENT, name: "OnBapScreenViewOptionEvent", sig: "sig<RCF::Void, SVPCanType::SVPBapScreenViewIdType, std::vector<unsigned char, std::allocator<unsigned char> >&>" },
    Method { id: CAN_LISTENER_ON_BAP_SCREEN_VIEW_DATA_EVENT, name: "OnBapScreenViewDataEvent", sig: "sig<RCF::Void, SVPCanType::SVPBapScreenViewIdType, std::vector<unsigned char, std::allocator<unsigned char> >&>" },
    Method { id: CAN_LISTENER_ON_BAP_SCREEN_VIEW_REQUEST_EVENT, name: "OnBapScreenViewRequestEvent", sig: "sig<RCF::Void, SVPCanType::SVPBapScreenViewIdType, SVPCanType::SVPBapScreenCtrlType>" },
    Method { id: CAN_LISTENER_ON_BAP_PHONE_CALL_ACTION_CHANGE, name: "OnBapPhoneCallActionChange", sig: "sig<RCF::Void, SVPCanType::SVPBapPhoneCallActionType, unsigned char>" },
    Method { id: CAN_LISTENER_ON_BAP_STATUS_BAR_CHANGED, name: "OnBapStatusBarChanged", sig: "sig<RCF::Void, std::vector<unsigned char, std::allocator<unsigned char> >&>" },
    Method { id: CAN_LISTENER_ON_BAP_FSG_STATE_CHANGED, name: "OnBapFsgStateChanged", sig: "sig<RCF::Void, SVPCanType::SVPBapFsgStateType, int>" },
    Method { id: CAN_LISTENER_ON_BAP_GENERAL_POPUP_CHANGED, name: "OnBapGeneralPopupChanged", sig: "sig<RCF::Void, SVPCanType::SVPBapGeneralPopupType>" },
    Method { id: CAN_LISTENER_ON_BAP_START_STOP_VALUE_CHANGE, name: "OnBapStartStopValueChange", sig: "sig<RCF::Void, SVPCanType::SVPStartStopValueIdType, std::vector<unsigned char, std::allocator<unsigned char> >&>" },
    Method { id: CAN_LISTENER_ON_BAP_SCREEN_OFF_TIMER_EVENT, name: "OnBapScreenOffTimerEvent", sig: "sig<RCF::Void, SVPCanType::SVPBapScreenOffFuncType, SVPCanType::SVPBapScreenOffTimerType>" },
    Method { id: CAN_LISTENER_ON_CAN_SERVICE_MESSAGE, name: "OnCanServiceMessage", sig: "sig<RCF::Void, unsigned short, std::vector<unsigned char, std::allocator<unsigned char> >&>" },
];
