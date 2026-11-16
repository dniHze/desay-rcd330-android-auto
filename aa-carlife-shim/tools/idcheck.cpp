// SPDX-License-Identifier: GPL-2.0-only
//
// Compiled (never linked or run) by build.sh at -O0 -fno-inline. RCF_BEGIN makes
// RcfClient<I> a full specialization whose stubs are inline members, so they are only
// emitted when used: this function uses every one, once, through the CallOptions overload.
// tools/rcf_ids.py then reads the method ID each stub sends and build.sh compares the table
// with tools/stock_ids.txt (read from the stock libcarlife.so and carlifeservice, and for
// SVP_iLMService and SVP_Source* from the stock libilm.so, libsource.so and sourcemgr stubs,
// SVP_Can* from libcan.so and canservice, SVP_Location* from liblocation.so and locationservice,
// SVP_BtService from libbt.so and btservice, SVP_Rmt* from libdevice.so and devicemgr, SVP_Power*
// from the unit's libpower.so and powermgr:
// the fnId each passes to FutureImplBase (RCF 2.0: to ClientStub::init) and its parameter
// types; see the IDL headers).

#include "audio_idl.hpp"
#include "bt_idl.hpp"
#include "can_idl.hpp"
#include "carlife_idl.hpp"
#include "ilm_idl.hpp"
#include "location_idl.hpp"
#include "power_idl.hpp"
#include "rmt_idl.hpp"
#include "source_idl.hpp"

#include <RCF/RCF.hpp>

using namespace SVPCarLife;

void idcheckUseAllStubs(RcfClient<ISVP_CarLifeService>& s, RcfClient<ISVP_CarLifeListener>& l,
                        const RCF::CallOptions& o) {
    s.Play(o, 0);
    s.Pause(o, 0);
    s.Show(o, 0);
    s.StartApp(o, 0);
    s.CarLifeConnection(o);
    s.CarLifeDisConnection(o);
    s.SingleTouchInfo(o, 0u, 0u, CL_TOUCH_DOWN);
    s.RotateKeyEvent(o, 0u, 0);
    s.GetCarLifeConnectStatus(o);
    s.GoToForeGround(o);
    s.NeedBlockBTPopup(o);
    s.NeedSwitchCarLifeAppToFG(o);
    s.GetModuleStatus(o, 0u);
    s.SetSourceAvailable(o, 0);
    s.SendModuleControlCmd(o, 0u, 0u);
    s.GetConnectCarlifePhoneBtAddress(o);
    s.ConnectCarlifePhoneIsiOS(o);
    s.ClearCarLifeStatus(o);
    s.NeedChangeHandFreeMode(o);

    l.OnAppStatusChange(o, S_CL_APP_STATUS());
    l.OnCarLifeConnectStatusChange(o, CL_ST_NONE, CL_ERR_NONE);
    l.OnConnectingStatusPercentage(o, 0);
    l.OnPhoneStatusChange(o, S_CL_PHONE_STATUS());
    l.OnTelephoneStatusChange(o, CL_TEL_IDLE);
    l.OnScreenStatusChange(o, S_CL_SCREEN_STATUS());
    l.OnConnectException(o, S_CL_EXCEPTION_TYPE());
    l.OnGoToDeskTop(o);
    l.OnModuleStatusChange(o, 0u, 0u);
    l.OnBtCallingAndPauseCarlifeConnect(o, 0);
}

void idcheckUseIlmStubs(RcfClient<SVP_iLMService>& i, const RCF::CallOptions& o) {
    i.WaitSurface(o, 0u);
    i.ShowSurface(o, 0u, 0);
    i.SetSurfacePos(o, 0u, 0u, 0u, 0u, 0u);
    i.Screenshot(o, 0u, std::string());
}

void idcheckUseSourceStubs(RcfClient<SVP_SourceService>& s, RcfClient<SVP_SourceCallback>& c,
                           const RCF::CallOptions& o) {
    s.RegisterClient(o, 0);
    s.RequestSource(o, 0, 0);
    s.ReleaseSource(o, 0, 0, 0);
    s.ConfirmSource(o, 0);
    s.SetSourceStatus(o, 0, SVPSource::SS_NULL);
    s.GetSourceStatus(o, 0);
    s.SetIpodAttatchFilterFlag(o);
    s.SetDeviceAttachFilterFlag(o, 0, 0);
    s.GetLastBGSource(o);
    c.cbSourceOp(o, 0, SVPSource::OP_PLAY);
}

void idcheckUseCanStubs(RcfClient<SVP_CanService>& s, RcfClient<SVP_CanListener>& l,
                        const RCF::CallOptions& o) {
    using namespace SVPCanType;
    SVPCanIgnModeType ign = SVPCanIgnModeType(0);
    SVPCanCarplayInfoType cp;
    SVPDescAdaptionBtSettingType abt;
    SVPDescAdaptionVehicleConfigType avc;
    SVPDescCodingVehicleImageType cvi;
    SVPDescCodingBtSettingType cbt;
    SVPBapClockFormatType clk;
    SVPBapRvcOpsStateType rvc = SVPBapRvcOpsStateType(0);
    unsigned short us = 0;
    unsigned char uc = 0;
    std::vector<unsigned char> v;

    s.GetCanIgnitionMode(o);
    s.GetCanIgnitionMode(o, ign);
    s.GetCanVehicleSpeed(o);
    s.GetCanCarplayInfo(o, cp);
    s.GetCanAudioLoweringPresent(o);
    s.GetDescAdaptionBtSetting(o, abt);
    s.GetDescAdaptionVehicleCfg(o, avc);
    s.GetDescAdaptionStickerBits(o, us);
    s.GetDescCodingVehicleImage(o, cvi);
    s.GetDescCodingBtSetting(o, cbt);
    s.GetDescCodingHmiSkin(o, uc);
    s.GetDescCodingStartScreen(o, uc);
    s.SetDescActuatorTestDone(o, SVPDescActuatorIdType(0), SVPDescActuatorCtrlType(0));
    s.SetDescDatasetDownloadDone(o, SVPDescDatasetIdType(0));
    s.SetDescRoutineTestDone(o, SVPDescRoutineIdType(0), SVPDescRoutineCtrlType(0), uc);
    s.SetDescMeasuredValue(o, SVPDescMeasuredIdType(0), v);
    s.GetDescCarFuncAdaption(o, v);
    s.SetBapScreenViewRequest(o, SVPBapScreenViewIdType(0), SVPBapScreenCtrlType(0));
    s.SetBapScreenViewOptionRequest(o, SVPBapScreenViewIdType(0));
    s.SetBapScreenVauleRequest(o, SVPBapScreenViewIdType(0));
    s.SetBapPhoneDataInfo(o, SVPBapPhoneDataInfoType(0), v);
    s.GetBapStatusBarVal(o);
    s.SetBapScreenItemVal(o, SVPBapScreenSetIdType(0), uc);
    s.SetBapScreenItemParams(o, SVPBapScreenSetParamIdType(0), v);
    s.GetBapClockFormat(o, clk);
    s.SetBapWakeupFlag(o, SVPBapWakeupFlagType(0), SVPBapWakeupStateType(0));
    s.SetBapScreenOffTimer(o, SVPBapScreenOffFuncType(0), SVPBapScreenOffTimerType(0));
    s.GetBapRvcOpsState(o, rvc);
    s.SendCanServiceMessage(o, us, v);

    l.OnCanIgnitionModeChanged(o, ign);
    l.OnCanVehicleSpeedChanged(o, us);
    l.OnCanCarplayInfoChanged(o, cp);
    l.OnCanAudioLoweringChange(o, 0);
    l.OnDescAdaptionBtSettingChanged(o, abt);
    l.OnDescAdaptionVehicleCfgChanged(o, avc);
    l.OnDescAdaptionStickerBitsChanged(o, us);
    l.OnDescCodingVehicleImageChanged(o, cvi);
    l.OnDescCodingBtSettingChanged(o, cbt);
    l.OnDescActuatorTestEvent(o, SVPDescActuatorIdType(0), SVPDescActuatorCtrlType(0));
    l.OnDescDatasetDownloadEvent(o, SVPDescDatasetIdType(0), v);
    l.OnDescRoutineTestEvent(o, SVPDescRoutineIdType(0), SVPDescRoutineCtrlType(0), uc);
    l.OnDescMeasuredValueEvent(o, SVPDescMeasuredIdType(0));
    l.OnBapScreenViewOptionEvent(o, SVPBapScreenViewIdType(0), v);
    l.OnBapScreenViewDataEvent(o, SVPBapScreenViewIdType(0), v);
    l.OnBapScreenViewRequestEvent(o, SVPBapScreenViewIdType(0), SVPBapScreenCtrlType(0));
    l.OnBapPhoneCallActionChange(o, SVPBapPhoneCallActionType(0), uc);
    l.OnBapStatusBarChanged(o, v);
    l.OnBapFsgStateChanged(o, SVPBapFsgStateType(0), 0);
    l.OnBapGeneralPopupChanged(o, SVPBapGeneralPopupType(0));
    l.OnBapStartStopValueChange(o, SVPStartStopValueIdType(0), v);
    l.OnBapScreenOffTimerEvent(o, SVPBapScreenOffFuncType(0), SVPBapScreenOffTimerType(0));
    l.OnCanServiceMessage(o, us, v);
}

void idcheckUseLocationStubs(RcfClient<SVP_LocationService>& s, RcfClient<SVP_LocationNotify>& n,
                             const RCF::CallOptions& o) {
    SVPTypeMmFeedback fb;
    SVPTypeGpsTime t;
    SVPTypeGpsLocation loc;
    SVPTypeGpsChannel ch;
    SVPTypeDrLocation dr;
    SVPTypeDrSensorData ds;
    unsigned short ttff = 0;

    s.Feedback(o, fb);
    s.RestartGPS(o, 0);

    n.OnGpsTimeUpdate(o, t);
    n.OnGpsLocationUpdate(o, loc);
    n.OnGpsChannelUpdate(o, ch);
    n.OnDrLocationUpdate(o, dr);
    n.OnDrSensorDataUpdate(o, ds);
    n.OnGpsTTFFUpdate(o, ttff);
}

void idcheckUseBtStubs(RcfClient<SVP_BtService>& s, const RCF::CallOptions& o) {
    using namespace SVPBtType;
    std::string str;
    std::vector<unsigned char> v;

    s.OpenBt(o, 0);
    s.ShutdownBt(o, 0);
    s.GetBtStatus(o);
    s.GetLastBtEnable(o);
    s.GetBtVersion(o);
    s.GetAttribute(o, Attribute(0), str);
    s.SetAttribute(o, Attribute(0), str);
    s.GetCapability(o, Capability(0));
    s.SetCapability(o, Capability(0), 0);
    s.ConfigTrace(o, (unsigned short)0);
    s.SetECNRParams(o, v);
    s.SetECNRAudioPath(o, EAP_BT_MODE);
    s.SetECNRParamsMode(o, NARROW_BAND);
    s.GetECNRParamsMode(o);
}

void idcheckUseRmtStubs(RcfClient<SVP_RmtService>& s, RcfClient<SVP_RmtCallback>& c,
                        const RCF::CallOptions& o) {
    std::vector<unsigned char> v;

    s.SetMessageFilter(o, 0u);
    s.SendMessage(o, v);
    c.ProcessMessage(o, v);
}

void idcheckUseAudioStubs(RcfClient<ISVP_AudioService>& s, const RCF::CallOptions& o) {
    s.RegisterAudio(o, std::string("sv-as-carlife-prompt"), 0u);
}

void idcheckUsePowerStubs(RcfClient<SVP_PowerService>& s, RcfClient<SVP_PowerListener>& l, const RCF::CallOptions& o) {
    SVPPower::PowerState st = SVPPower::PS_USER_ON;
    unsigned char sub = 0;
    s.GetPowerState(o, st, sub);
    l.OnScreenOffChanged(o, 0);
    l.OnDisplayoffChanged(o, 0);
    l.OnPowerStateChanged(o, SVPPower::PS_USER_ON, 0, 0);
    l.OnBkLightLevelChanged(o, SVPPower::BkLight::BkLightMode(0), SVPPower::BkLight::BkLightType(0), 0);
}

void idcheckUseSourceListenerStubs(RcfClient<SVP_SourceListener>& l, const RCF::CallOptions& o) {
    l.pubSourceStatus(o, 0, SVPSource::SS_NULL);
}

void idcheckUseBtPairingStubs(RcfClient<SVP_BtService>& s, RcfClient<SVP_BtListener>& l, const RCF::CallOptions& o) {
    using namespace SVPBtType;
    SVPBtDevice d;
    std::string str;
    s.AcceptSspRequest(o, 0);
    s.Connect(o, str, BT_SERVICE_A2DP);
    s.Disconnect(o, BT_SERVICE_A2DP);
    s.GetHfpConnectAddr(o, str);
    s.GetAvpConnectAddr(o, str);
    s.AcceptPinCodeRequest(o, str);
    s.SetAutoAcceptPairing(o, 0);
    s.SetAutoAcceptConnect(o, 0);
    s.GetAutoAcceptConnect(o);
    s.GetDeviceNum(o, BT_DEVICE_PAIRED);
    s.GetDevice(o, 0, d, BT_DEVICE_PAIRED);
    l.OnBtOpenStatusChanged(o, SVPBtType::BT_STATUS(0), 0);
    l.OnBtResetFinish(o);
    l.OnSetVisibleStatusChanged(o, 0);
    l.OnSetBtNameStatusChanged(o, 0);
    l.OnECNRAudioPathChanged(o, SVPBtType::ECNR_AUDIO_PATH(0));
    l.OnECNRParamsModeChanged(o, SVPBtType::ECNR_PARAMS_MODE(0));
    l.OnSearchDeviceInd(o);
    l.OnBtUartDebugStatusChanged(o);
    l.OnSearchDeviceCmp(o);
    l.OnSearchDeviceStart(o);
    l.OnDeviceNameChanged(o, 0u);
    l.OnInqureProfileCmp(o, 0);
    l.OnPairingComfirmRequest(o, 0u, str);
    l.OnPairingPinCodeRequest(o, 0u, str, str);
    l.OnRemoteConnectRequest(o, str);
    l.OnReturnRemoteDeviceName(o, str, str);
    l.OnPairStatusChanged(o, SVPBtType::PAIR_STATUS(0), str);
    l.OnDeviceNeedPair(o, str);
    l.OnSupportProfileChanged(o, 0, 0);
    l.OnPairListChanged(o);
    l.OnSearchDeviceStop(o);
    l.OnReplaceConnectRequest(o);
    l.OnFavoriteChanged(o);
    l.OnUserProfileChanged(o);
    l.OnAudioProfileEnable(o);
    l.OnPhoneReminderEnable(o);
    l.OnConnectStatusChanged(o, SVPBtType::BT_PROFILE_CONNECT(0), 0, str);
    l.OnConnectCmp(o);
    l.OnBtConnnectDisableStatusChanged(o, (unsigned char)0);
    l.OnDisconnectStatusChanged(o, SVPBtType::BT_PROFILE_CONNECT(0), 0);
    l.OnDisconnectCmp(o);
    l.OnRemoteSignalChanged(o);
    l.OnRemoteRoamingChanged(o);
    l.OnRemoteBatteryChanged(o);
    l.OnAudioPathChanged(o, SVPBtType::AUDIO_PATH_STATUS(0));
    l.OnServiceStatusChanged(o);
    l.OnServiceNameChanged(o);
    l.OnServiceRegistrationStatusChanged(o);
    l.OnVoiceRecognitionStatusChanged(o);
    l.OnExResponseStatusChanged(o);
    l.OnCallStatusChanged(o, SVPBtType::COMBINE_CALL_STATUS(0));
    l.OnConferenceCallCountChanged(o);
    l.OnCallTimeChanged(o, 0u);
    l.OnCallSupportActionChanged(o);
    l.OnSpeakerPrivacyModeChanged(o);
    l.OnMicrophoneStatus(o, 0);
    l.OnPhoneBookNumDownloadCmp(o, 0, 0u);
    l.OnPhoneBookDownloadCmp(o, 0, 0);
    l.OnPhoneBookImportCmp(o);
    l.OnPbDownloadCountChanged(o, 0, 0);
    l.OnCallHistoryDonwloadCmp(o, 0);
    l.OnClearMissedCall(o);
    l.OnMusicPlayStatusChanged(o, SVPBtType::PLAY_STATUS(0), 0);
    l.OnMusicPlayStatusBeforeDisconnect(o, SVPBtType::PLAY_STATUS(0));
    l.OnMusicPlayModeChanged(o, (unsigned char)0, (unsigned char)0);
    l.OnMusicPlaytimeChanged(o, 0u);
    l.OnMusicPlayTrackChanged(o);
    l.OnMusicID3Changed(o);
    l.OnMusicPlayingListChanged(o);
    l.OnMusicPlaytingListCmp(o);
    l.OnMusicBrowsingChanged(o);
    l.OnBrowserMediaInfoChanged(o);
    l.OnMusicPlayerChanged(o);
    l.OnAvrcpVersionChanged(o);
    l.OnCurBrowserPathChanged(o, 0, 0u);
    l.OnReturnUidTitle(o, str, str);
    l.OnBtAudioValidChanged(o, 0);
    l.OnMessageDownloadStatus(o, SVPBtType::MsgLoadStatus(0));
    l.OnDeleteSms(o);
    l.OnSmsStatusChanged(o, 0);
    l.OnClearUnreadMessage(o);
    l.OnBtUpdateStatusChanged(o, SVPBtType::BT_UPDATE_STATUS(0));
    l.OnBtUpdateProgress(o, (unsigned short)0);
    l.OnMusicChanged(o);
    l.OnCallActionStatus(o, 0u, SVPBtType::CALL_ACTION_STATUS(0), str);
    l.OnReconnectStatusChanged(o, 0);
    l.OnAvpAutoConnectStatus(o, SVPBtType::AVP_AUTO_CONNECT_STATUS(0));
    l.OnErrorLogUpdate(o);
}
