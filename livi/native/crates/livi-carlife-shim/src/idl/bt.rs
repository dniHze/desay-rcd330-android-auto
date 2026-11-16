// Generated from aa-carlife-shim/src/bt_idl.hpp by gen_tables.py (method IDs and signatures, in
// declaration order: RCF numbers methods from 0, a placeholder keeps a position).
#![allow(dead_code)]

use super::Method;

pub const BT_SERVICE: &str = "SVP_BtService";
pub const BT_SERVICE_OPEN_BT: i32 = 0;
pub const BT_SERVICE_SHUTDOWN_BT: i32 = 1;
pub const BT_SERVICE_GET_BT_STATUS: i32 = 2;
pub const BT_SERVICE_GET_LAST_BT_ENABLE: i32 = 3;
pub const BT_SERVICE_GET_BT_VERSION: i32 = 4;
pub const BT_SERVICE_GET_ATTRIBUTE: i32 = 5;
pub const BT_SERVICE_SET_ATTRIBUTE: i32 = 6;
pub const BT_SERVICE_GET_CAPABILITY: i32 = 7;
pub const BT_SERVICE_SET_CAPABILITY: i32 = 8;
pub const BT_SERVICE_CONFIG_TRACE: i32 = 9;
pub const BT_SERVICE_SET_ECNR_PARAMS: i32 = 10;
pub const BT_SERVICE_SET_ECNR_AUDIO_PATH: i32 = 11;
pub const BT_SERVICE_SET_ECNR_PARAMS_MODE: i32 = 12;
pub const BT_SERVICE_GET_ECNR_PARAMS_MODE: i32 = 13;
pub const BT_SERVICE_CONNECT: i32 = 35;
pub const BT_SERVICE_DISCONNECT: i32 = 37;
pub const BT_SERVICE_ACCEPT_SSP_REQUEST: i32 = 39;
pub const BT_SERVICE_ACCEPT_PIN_CODE_REQUEST: i32 = 40;
pub const BT_SERVICE_SET_AUTO_ACCEPT_PAIRING: i32 = 43;
pub const BT_SERVICE_SET_AUTO_ACCEPT_CONNECT: i32 = 44;
pub const BT_SERVICE_GET_AUTO_ACCEPT_CONNECT: i32 = 45;
pub const BT_SERVICE_GET_DEVICE_NUM: i32 = 52;
pub const BT_SERVICE_GET_DEVICE: i32 = 55;
pub const BT_SERVICE_GET_HFP_CONNECT_ADDR: i32 = 60;
pub const BT_SERVICE_GET_AVP_CONNECT_ADDR: i32 = 62;
pub const BT_SERVICE_METHODS: &[Method] = &[
    Method { id: BT_SERVICE_OPEN_BT, name: "OpenBt", sig: "sig<RCF::Void, int>" },
    Method { id: BT_SERVICE_SHUTDOWN_BT, name: "ShutdownBt", sig: "sig<RCF::Void, int>" },
    Method { id: BT_SERVICE_GET_BT_STATUS, name: "GetBtStatus", sig: "sig<SVPBtType::BT_STATUS>" },
    Method { id: BT_SERVICE_GET_LAST_BT_ENABLE, name: "GetLastBtEnable", sig: "sig<int>" },
    Method { id: BT_SERVICE_GET_BT_VERSION, name: "GetBtVersion", sig: "sig<unsigned int>" },
    Method { id: BT_SERVICE_GET_ATTRIBUTE, name: "GetAttribute", sig: "sig<int, SVPBtType::Attribute, std::string&>" },
    Method { id: BT_SERVICE_SET_ATTRIBUTE, name: "SetAttribute", sig: "sig<int, SVPBtType::Attribute, std::string const&>" },
    Method { id: BT_SERVICE_GET_CAPABILITY, name: "GetCapability", sig: "sig<int, SVPBtType::Capability>" },
    Method { id: BT_SERVICE_SET_CAPABILITY, name: "SetCapability", sig: "sig<int, SVPBtType::Capability, int>" },
    Method { id: BT_SERVICE_CONFIG_TRACE, name: "ConfigTrace", sig: "sig<RCF::Void, unsigned short>" },
    Method { id: BT_SERVICE_SET_ECNR_PARAMS, name: "SetECNRParams", sig: "sig<int, std::vector<unsigned char, std::allocator<unsigned char> >&>" },
    Method { id: BT_SERVICE_SET_ECNR_AUDIO_PATH, name: "SetECNRAudioPath", sig: "sig<RCF::Void, SVPBtType::ECNR_AUDIO_PATH>" },
    Method { id: BT_SERVICE_SET_ECNR_PARAMS_MODE, name: "SetECNRParamsMode", sig: "sig<RCF::Void, SVPBtType::ECNR_PARAMS_MODE>" },
    Method { id: BT_SERVICE_GET_ECNR_PARAMS_MODE, name: "GetECNRParamsMode", sig: "sig<SVPBtType::ECNR_PARAMS_MODE>" },
    Method { id: BT_SERVICE_CONNECT, name: "Connect", sig: "sig<RCF::Void, std::string, SVPBtType::BT_SERVICE>" },
    Method { id: BT_SERVICE_DISCONNECT, name: "Disconnect", sig: "sig<RCF::Void, SVPBtType::BT_SERVICE>" },
    Method { id: BT_SERVICE_ACCEPT_SSP_REQUEST, name: "AcceptSspRequest", sig: "sig<RCF::Void, int>" },
    Method { id: BT_SERVICE_ACCEPT_PIN_CODE_REQUEST, name: "AcceptPinCodeRequest", sig: "sig<RCF::Void, std::string>" },
    Method { id: BT_SERVICE_SET_AUTO_ACCEPT_PAIRING, name: "SetAutoAcceptPairing", sig: "sig<int, int>" },
    Method { id: BT_SERVICE_SET_AUTO_ACCEPT_CONNECT, name: "SetAutoAcceptConnect", sig: "sig<RCF::Void, int>" },
    Method { id: BT_SERVICE_GET_AUTO_ACCEPT_CONNECT, name: "GetAutoAcceptConnect", sig: "sig<int>" },
    Method { id: BT_SERVICE_GET_DEVICE_NUM, name: "GetDeviceNum", sig: "sig<int, SVPBtType::BT_DEVICE_TYPE>" },
    Method { id: BT_SERVICE_GET_DEVICE, name: "GetDevice", sig: "sig<int, int, SVPBtType::SVPBtDevice&, SVPBtType::BT_DEVICE_TYPE>" },
    Method { id: BT_SERVICE_GET_HFP_CONNECT_ADDR, name: "GetHfpConnectAddr", sig: "sig<int, std::string&>" },
    Method { id: BT_SERVICE_GET_AVP_CONNECT_ADDR, name: "GetAvpConnectAddr", sig: "sig<int, std::string&>" },
];

pub const BT_LISTENER: &str = "SVP_BtListener";
pub const BT_LISTENER_ON_BT_OPEN_STATUS_CHANGED: i32 = 0;
pub const BT_LISTENER_ON_BT_RESET_FINISH: i32 = 1;
pub const BT_LISTENER_ON_SET_VISIBLE_STATUS_CHANGED: i32 = 2;
pub const BT_LISTENER_ON_SET_BT_NAME_STATUS_CHANGED: i32 = 3;
pub const BT_LISTENER_ON_ECNR_AUDIO_PATH_CHANGED: i32 = 4;
pub const BT_LISTENER_ON_ECNR_PARAMS_MODE_CHANGED: i32 = 5;
pub const BT_LISTENER_ON_SEARCH_DEVICE_IND: i32 = 6;
pub const BT_LISTENER_ON_BT_UART_DEBUG_STATUS_CHANGED: i32 = 7;
pub const BT_LISTENER_ON_SEARCH_DEVICE_CMP: i32 = 8;
pub const BT_LISTENER_ON_SEARCH_DEVICE_START: i32 = 9;
pub const BT_LISTENER_ON_DEVICE_NAME_CHANGED: i32 = 10;
pub const BT_LISTENER_ON_INQURE_PROFILE_CMP: i32 = 11;
pub const BT_LISTENER_ON_PAIRING_COMFIRM_REQUEST: i32 = 12;
pub const BT_LISTENER_ON_PAIRING_PIN_CODE_REQUEST: i32 = 13;
pub const BT_LISTENER_ON_REMOTE_CONNECT_REQUEST: i32 = 14;
pub const BT_LISTENER_ON_RETURN_REMOTE_DEVICE_NAME: i32 = 15;
pub const BT_LISTENER_ON_PAIR_STATUS_CHANGED: i32 = 16;
pub const BT_LISTENER_ON_DEVICE_NEED_PAIR: i32 = 17;
pub const BT_LISTENER_ON_SUPPORT_PROFILE_CHANGED: i32 = 18;
pub const BT_LISTENER_ON_PAIR_LIST_CHANGED: i32 = 19;
pub const BT_LISTENER_ON_SEARCH_DEVICE_STOP: i32 = 20;
pub const BT_LISTENER_ON_REPLACE_CONNECT_REQUEST: i32 = 21;
pub const BT_LISTENER_ON_FAVORITE_CHANGED: i32 = 22;
pub const BT_LISTENER_ON_USER_PROFILE_CHANGED: i32 = 23;
pub const BT_LISTENER_ON_AUDIO_PROFILE_ENABLE: i32 = 24;
pub const BT_LISTENER_ON_PHONE_REMINDER_ENABLE: i32 = 25;
pub const BT_LISTENER_ON_CONNECT_STATUS_CHANGED: i32 = 26;
pub const BT_LISTENER_ON_CONNECT_CMP: i32 = 27;
pub const BT_LISTENER_ON_BT_CONNNECT_DISABLE_STATUS_CHANGED: i32 = 28;
pub const BT_LISTENER_ON_DISCONNECT_STATUS_CHANGED: i32 = 29;
pub const BT_LISTENER_ON_DISCONNECT_CMP: i32 = 30;
pub const BT_LISTENER_ON_REMOTE_SIGNAL_CHANGED: i32 = 31;
pub const BT_LISTENER_ON_REMOTE_ROAMING_CHANGED: i32 = 32;
pub const BT_LISTENER_ON_REMOTE_BATTERY_CHANGED: i32 = 33;
pub const BT_LISTENER_ON_AUDIO_PATH_CHANGED: i32 = 34;
pub const BT_LISTENER_ON_SERVICE_STATUS_CHANGED: i32 = 35;
pub const BT_LISTENER_ON_SERVICE_NAME_CHANGED: i32 = 36;
pub const BT_LISTENER_ON_SERVICE_REGISTRATION_STATUS_CHANGED: i32 = 37;
pub const BT_LISTENER_ON_VOICE_RECOGNITION_STATUS_CHANGED: i32 = 38;
pub const BT_LISTENER_ON_EX_RESPONSE_STATUS_CHANGED: i32 = 39;
pub const BT_LISTENER_ON_CALL_STATUS_CHANGED: i32 = 40;
pub const BT_LISTENER_ON_CONFERENCE_CALL_COUNT_CHANGED: i32 = 41;
pub const BT_LISTENER_ON_CALL_TIME_CHANGED: i32 = 42;
pub const BT_LISTENER_ON_CALL_SUPPORT_ACTION_CHANGED: i32 = 43;
pub const BT_LISTENER_ON_SPEAKER_PRIVACY_MODE_CHANGED: i32 = 44;
pub const BT_LISTENER_ON_MICROPHONE_STATUS: i32 = 45;
pub const BT_LISTENER_ON_PHONE_BOOK_NUM_DOWNLOAD_CMP: i32 = 46;
pub const BT_LISTENER_ON_PHONE_BOOK_DOWNLOAD_CMP: i32 = 47;
pub const BT_LISTENER_ON_PHONE_BOOK_IMPORT_CMP: i32 = 48;
pub const BT_LISTENER_ON_PB_DOWNLOAD_COUNT_CHANGED: i32 = 49;
pub const BT_LISTENER_ON_CALL_HISTORY_DONWLOAD_CMP: i32 = 50;
pub const BT_LISTENER_ON_CLEAR_MISSED_CALL: i32 = 51;
pub const BT_LISTENER_ON_MUSIC_PLAY_STATUS_CHANGED: i32 = 52;
pub const BT_LISTENER_ON_MUSIC_PLAY_STATUS_BEFORE_DISCONNECT: i32 = 53;
pub const BT_LISTENER_ON_MUSIC_PLAY_MODE_CHANGED: i32 = 54;
pub const BT_LISTENER_ON_MUSIC_PLAYTIME_CHANGED: i32 = 55;
pub const BT_LISTENER_ON_MUSIC_PLAY_TRACK_CHANGED: i32 = 56;
pub const BT_LISTENER_ON_MUSIC_ID3_CHANGED: i32 = 57;
pub const BT_LISTENER_ON_MUSIC_PLAYING_LIST_CHANGED: i32 = 58;
pub const BT_LISTENER_ON_MUSIC_PLAYTING_LIST_CMP: i32 = 59;
pub const BT_LISTENER_ON_MUSIC_BROWSING_CHANGED: i32 = 60;
pub const BT_LISTENER_ON_BROWSER_MEDIA_INFO_CHANGED: i32 = 61;
pub const BT_LISTENER_ON_MUSIC_PLAYER_CHANGED: i32 = 62;
pub const BT_LISTENER_ON_AVRCP_VERSION_CHANGED: i32 = 63;
pub const BT_LISTENER_ON_CUR_BROWSER_PATH_CHANGED: i32 = 64;
pub const BT_LISTENER_ON_RETURN_UID_TITLE: i32 = 65;
pub const BT_LISTENER_ON_BT_AUDIO_VALID_CHANGED: i32 = 66;
pub const BT_LISTENER_ON_MESSAGE_DOWNLOAD_STATUS: i32 = 67;
pub const BT_LISTENER_ON_DELETE_SMS: i32 = 68;
pub const BT_LISTENER_ON_SMS_STATUS_CHANGED: i32 = 69;
pub const BT_LISTENER_ON_CLEAR_UNREAD_MESSAGE: i32 = 70;
pub const BT_LISTENER_ON_BT_UPDATE_STATUS_CHANGED: i32 = 71;
pub const BT_LISTENER_ON_BT_UPDATE_PROGRESS: i32 = 72;
pub const BT_LISTENER_ON_MUSIC_CHANGED: i32 = 73;
pub const BT_LISTENER_ON_CALL_ACTION_STATUS: i32 = 74;
pub const BT_LISTENER_ON_RECONNECT_STATUS_CHANGED: i32 = 75;
pub const BT_LISTENER_ON_AVP_AUTO_CONNECT_STATUS: i32 = 76;
pub const BT_LISTENER_ON_ERROR_LOG_UPDATE: i32 = 77;
pub const BT_LISTENER_METHODS: &[Method] = &[
    Method { id: BT_LISTENER_ON_BT_OPEN_STATUS_CHANGED, name: "OnBtOpenStatusChanged", sig: "sig<RCF::Void, SVPBtType::BT_STATUS, int>" },
    Method { id: BT_LISTENER_ON_BT_RESET_FINISH, name: "OnBtResetFinish", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_SET_VISIBLE_STATUS_CHANGED, name: "OnSetVisibleStatusChanged", sig: "sig<RCF::Void, int>" },
    Method { id: BT_LISTENER_ON_SET_BT_NAME_STATUS_CHANGED, name: "OnSetBtNameStatusChanged", sig: "sig<RCF::Void, int>" },
    Method { id: BT_LISTENER_ON_ECNR_AUDIO_PATH_CHANGED, name: "OnECNRAudioPathChanged", sig: "sig<RCF::Void, SVPBtType::ECNR_AUDIO_PATH>" },
    Method { id: BT_LISTENER_ON_ECNR_PARAMS_MODE_CHANGED, name: "OnECNRParamsModeChanged", sig: "sig<RCF::Void, SVPBtType::ECNR_PARAMS_MODE>" },
    Method { id: BT_LISTENER_ON_SEARCH_DEVICE_IND, name: "OnSearchDeviceInd", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_BT_UART_DEBUG_STATUS_CHANGED, name: "OnBtUartDebugStatusChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_SEARCH_DEVICE_CMP, name: "OnSearchDeviceCmp", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_SEARCH_DEVICE_START, name: "OnSearchDeviceStart", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_DEVICE_NAME_CHANGED, name: "OnDeviceNameChanged", sig: "sig<RCF::Void, unsigned int>" },
    Method { id: BT_LISTENER_ON_INQURE_PROFILE_CMP, name: "OnInqureProfileCmp", sig: "sig<RCF::Void, int>" },
    Method { id: BT_LISTENER_ON_PAIRING_COMFIRM_REQUEST, name: "OnPairingComfirmRequest", sig: "sig<RCF::Void, unsigned int, std::string>" },
    Method { id: BT_LISTENER_ON_PAIRING_PIN_CODE_REQUEST, name: "OnPairingPinCodeRequest", sig: "sig<RCF::Void, unsigned int, std::string, std::string>" },
    Method { id: BT_LISTENER_ON_REMOTE_CONNECT_REQUEST, name: "OnRemoteConnectRequest", sig: "sig<RCF::Void, std::string>" },
    Method { id: BT_LISTENER_ON_RETURN_REMOTE_DEVICE_NAME, name: "OnReturnRemoteDeviceName", sig: "sig<RCF::Void, std::string, std::string>" },
    Method { id: BT_LISTENER_ON_PAIR_STATUS_CHANGED, name: "OnPairStatusChanged", sig: "sig<RCF::Void, SVPBtType::PAIR_STATUS, std::string>" },
    Method { id: BT_LISTENER_ON_DEVICE_NEED_PAIR, name: "OnDeviceNeedPair", sig: "sig<RCF::Void, std::string>" },
    Method { id: BT_LISTENER_ON_SUPPORT_PROFILE_CHANGED, name: "OnSupportProfileChanged", sig: "sig<RCF::Void, int, int>" },
    Method { id: BT_LISTENER_ON_PAIR_LIST_CHANGED, name: "OnPairListChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_SEARCH_DEVICE_STOP, name: "OnSearchDeviceStop", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_REPLACE_CONNECT_REQUEST, name: "OnReplaceConnectRequest", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_FAVORITE_CHANGED, name: "OnFavoriteChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_USER_PROFILE_CHANGED, name: "OnUserProfileChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_AUDIO_PROFILE_ENABLE, name: "OnAudioProfileEnable", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_PHONE_REMINDER_ENABLE, name: "OnPhoneReminderEnable", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_CONNECT_STATUS_CHANGED, name: "OnConnectStatusChanged", sig: "sig<RCF::Void, SVPBtType::BT_PROFILE_CONNECT, int, std::string>" },
    Method { id: BT_LISTENER_ON_CONNECT_CMP, name: "OnConnectCmp", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_BT_CONNNECT_DISABLE_STATUS_CHANGED, name: "OnBtConnnectDisableStatusChanged", sig: "sig<RCF::Void, unsigned char>" },
    Method { id: BT_LISTENER_ON_DISCONNECT_STATUS_CHANGED, name: "OnDisconnectStatusChanged", sig: "sig<RCF::Void, SVPBtType::BT_PROFILE_CONNECT, int>" },
    Method { id: BT_LISTENER_ON_DISCONNECT_CMP, name: "OnDisconnectCmp", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_REMOTE_SIGNAL_CHANGED, name: "OnRemoteSignalChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_REMOTE_ROAMING_CHANGED, name: "OnRemoteRoamingChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_REMOTE_BATTERY_CHANGED, name: "OnRemoteBatteryChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_AUDIO_PATH_CHANGED, name: "OnAudioPathChanged", sig: "sig<RCF::Void, SVPBtType::AUDIO_PATH_STATUS>" },
    Method { id: BT_LISTENER_ON_SERVICE_STATUS_CHANGED, name: "OnServiceStatusChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_SERVICE_NAME_CHANGED, name: "OnServiceNameChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_SERVICE_REGISTRATION_STATUS_CHANGED, name: "OnServiceRegistrationStatusChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_VOICE_RECOGNITION_STATUS_CHANGED, name: "OnVoiceRecognitionStatusChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_EX_RESPONSE_STATUS_CHANGED, name: "OnExResponseStatusChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_CALL_STATUS_CHANGED, name: "OnCallStatusChanged", sig: "sig<RCF::Void, SVPBtType::COMBINE_CALL_STATUS>" },
    Method { id: BT_LISTENER_ON_CONFERENCE_CALL_COUNT_CHANGED, name: "OnConferenceCallCountChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_CALL_TIME_CHANGED, name: "OnCallTimeChanged", sig: "sig<RCF::Void, unsigned int>" },
    Method { id: BT_LISTENER_ON_CALL_SUPPORT_ACTION_CHANGED, name: "OnCallSupportActionChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_SPEAKER_PRIVACY_MODE_CHANGED, name: "OnSpeakerPrivacyModeChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_MICROPHONE_STATUS, name: "OnMicrophoneStatus", sig: "sig<RCF::Void, int>" },
    Method { id: BT_LISTENER_ON_PHONE_BOOK_NUM_DOWNLOAD_CMP, name: "OnPhoneBookNumDownloadCmp", sig: "sig<RCF::Void, int, unsigned int>" },
    Method { id: BT_LISTENER_ON_PHONE_BOOK_DOWNLOAD_CMP, name: "OnPhoneBookDownloadCmp", sig: "sig<RCF::Void, int, int>" },
    Method { id: BT_LISTENER_ON_PHONE_BOOK_IMPORT_CMP, name: "OnPhoneBookImportCmp", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_PB_DOWNLOAD_COUNT_CHANGED, name: "OnPbDownloadCountChanged", sig: "sig<RCF::Void, int, int>" },
    Method { id: BT_LISTENER_ON_CALL_HISTORY_DONWLOAD_CMP, name: "OnCallHistoryDonwloadCmp", sig: "sig<RCF::Void, int>" },
    Method { id: BT_LISTENER_ON_CLEAR_MISSED_CALL, name: "OnClearMissedCall", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_MUSIC_PLAY_STATUS_CHANGED, name: "OnMusicPlayStatusChanged", sig: "sig<RCF::Void, SVPBtType::PLAY_STATUS, int>" },
    Method { id: BT_LISTENER_ON_MUSIC_PLAY_STATUS_BEFORE_DISCONNECT, name: "OnMusicPlayStatusBeforeDisconnect", sig: "sig<RCF::Void, SVPBtType::PLAY_STATUS>" },
    Method { id: BT_LISTENER_ON_MUSIC_PLAY_MODE_CHANGED, name: "OnMusicPlayModeChanged", sig: "sig<RCF::Void, unsigned char, unsigned char>" },
    Method { id: BT_LISTENER_ON_MUSIC_PLAYTIME_CHANGED, name: "OnMusicPlaytimeChanged", sig: "sig<RCF::Void, unsigned int>" },
    Method { id: BT_LISTENER_ON_MUSIC_PLAY_TRACK_CHANGED, name: "OnMusicPlayTrackChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_MUSIC_ID3_CHANGED, name: "OnMusicID3Changed", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_MUSIC_PLAYING_LIST_CHANGED, name: "OnMusicPlayingListChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_MUSIC_PLAYTING_LIST_CMP, name: "OnMusicPlaytingListCmp", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_MUSIC_BROWSING_CHANGED, name: "OnMusicBrowsingChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_BROWSER_MEDIA_INFO_CHANGED, name: "OnBrowserMediaInfoChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_MUSIC_PLAYER_CHANGED, name: "OnMusicPlayerChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_AVRCP_VERSION_CHANGED, name: "OnAvrcpVersionChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_CUR_BROWSER_PATH_CHANGED, name: "OnCurBrowserPathChanged", sig: "sig<RCF::Void, int, unsigned int>" },
    Method { id: BT_LISTENER_ON_RETURN_UID_TITLE, name: "OnReturnUidTitle", sig: "sig<RCF::Void, std::string, std::string>" },
    Method { id: BT_LISTENER_ON_BT_AUDIO_VALID_CHANGED, name: "OnBtAudioValidChanged", sig: "sig<RCF::Void, int>" },
    Method { id: BT_LISTENER_ON_MESSAGE_DOWNLOAD_STATUS, name: "OnMessageDownloadStatus", sig: "sig<RCF::Void, SVPBtType::MsgLoadStatus>" },
    Method { id: BT_LISTENER_ON_DELETE_SMS, name: "OnDeleteSms", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_SMS_STATUS_CHANGED, name: "OnSmsStatusChanged", sig: "sig<RCF::Void, int>" },
    Method { id: BT_LISTENER_ON_CLEAR_UNREAD_MESSAGE, name: "OnClearUnreadMessage", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_BT_UPDATE_STATUS_CHANGED, name: "OnBtUpdateStatusChanged", sig: "sig<RCF::Void, SVPBtType::BT_UPDATE_STATUS>" },
    Method { id: BT_LISTENER_ON_BT_UPDATE_PROGRESS, name: "OnBtUpdateProgress", sig: "sig<RCF::Void, unsigned short>" },
    Method { id: BT_LISTENER_ON_MUSIC_CHANGED, name: "OnMusicChanged", sig: "sig<RCF::Void>" },
    Method { id: BT_LISTENER_ON_CALL_ACTION_STATUS, name: "OnCallActionStatus", sig: "sig<RCF::Void, unsigned int, SVPBtType::CALL_ACTION_STATUS, std::string>" },
    Method { id: BT_LISTENER_ON_RECONNECT_STATUS_CHANGED, name: "OnReconnectStatusChanged", sig: "sig<RCF::Void, int>" },
    Method { id: BT_LISTENER_ON_AVP_AUTO_CONNECT_STATUS, name: "OnAvpAutoConnectStatus", sig: "sig<RCF::Void, SVPBtType::AVP_AUTO_CONNECT_STATUS>" },
    Method { id: BT_LISTENER_ON_ERROR_LOG_UPDATE, name: "OnErrorLogUpdate", sig: "sig<RCF::Void>" },
];
