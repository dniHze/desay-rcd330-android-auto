// SPDX-License-Identifier: GPL-2.0-only
//
// The SVP Bluetooth service (svp/bin/btservice, registry svp.svc.bluetooth, started by
// svp/etc/init.sh): only the head of SVP_BtService, up to the ECNR methods. Recovered from the
// stock client svp/lib/libbt.so.1.0.0 (SVPBtClient, RCF 2.2) and checked against btservice
// (SVPBtServer):
//
//   * service "SVP_BtService" (SVP_BtService::getInterfaceName, libbt 0xdb63c) on TCP 30315:
//     SVPBtClient::SVPBtClient (0xb91b0) builds RcfClient<SVP_BtService>(TcpEndpoint("0.0.0.0",
//     30315), "") twice (calls go through the one at this+88, under the mutex at this+112);
//     btservice serves RcfServer(TcpEndpoint("0.0.0.0", 30315)) (SVPBtServer ctor 0x1859fc).
//     SVPBtClient::InitInstance (0xb8cb0) only waits for the registry name:
//     SVP_WaitService("svp.svc.bluetooth", ...). SVP_CreateBlueTooth(const char*) (0xb95a4) is
//     SVPSingleton<SVPBtClient>::GetInstance(); its argument is ignored.
//   * method IDs: the fnId each libbt RcfClient<SVP_BtService>::M(CallOptions&, ...) stub passes
//     to FutureImplBase; types: the AllocateClientParameters<> each stub uses. Cross-checked in
//     btservice: StubAccess::invoke<RcfClient<SVP_BtService>, int_<N>> calls SVPBtServer vtable
//     slot 336 + 4*N, and _ZTV11SVPBtServer (0x328218) holds, from slot 336: OpenBt, ShutdownBt,
//     GetBtStatus, GetLastBtEnable, GetBtVersion, GetAttribute, SetAttribute, GetCapability,
//     SetCapability, ConfigTrace, SetECNRParams, SetECNRAudioPath, SetECNRParamsMode,
//     GetECNRParamsMode (IDs 0..13). libbt's stubs run to ID 168; IDs are positional, and
//     nothing past 13 is declared here, so no later method can be called.
//     IDs past 13: only what the AA pairing loop calls (placeholders keep the positions), as the
//     UNIT's libbt (step 3 capture, md5 4df5dcb3) numbers them: it has RemoveConnectListCmd at 51,
//     which the firmware dump's libbt (.scratch/re/bt_stock.txt) lacks, so from there on the
//     unit's IDs are one higher (GetDeviceNum 52, GetDevice 55).
//   * listener "SVP_BtListener" is RCF publish/subscribe: btservice runs a second
//     RcfServer(TcpEndpoint("127.0.0.1", 30316)) (SVPBtServer ctor 0x185a78) and
//     createPublisher<SVP_BtListener>(PublisherParms()) (0x18e07c), so the topic is the
//     interface name; libbt subscribes with setPublisherEndpoint(TcpEndpoint("127.0.0.1",
//     30316)) (SVPBtClient::InitListener 0xc45bc). IDs from the UNIT's btservice publisher
//     stubs (md5 2e9e64f0; 78 events): it adds OnDeviceNeedPair (17), OnSupportProfileChanged
//     (18) and OnBtConnnectDisableStatusChanged (28) to the dump's, and OnConnectStatusChanged
//     carries the address. All 78 are declared with their types (the shim's servant must have a
//     method for every event btservice sends: RCF asserts on an id without one, which aborted the
//     shim in step 3 runs 34-36), and SVPBtListenerNoop gives each a no-op.
//
// Pairing (btservice, for the AA pairing loop in shim.cpp):
//   * a phone that is not paired connects: the BT module asks for a link key;
//     CBtSettingService::OnHandleLinkeyInd (0x16a204) answers at once if SetAutoAcceptConnect(1)
//     is on, else publishes OnRemoteConnectRequest and waits for AcceptExternConnectRequest; the
//     Bluetooth app (storage/app/sv.g5.mib2.bluetooth) answers that itself and shows its
//     "pairing request" dialog (SVPTipBtConfirmExtReqlDlg, BTMgr 0xcdc00), which never closes by
//     itself;
//   * then SSP numeric comparison: OnHandleSSPNumericCfmInd (0x160ccc) confirms internally if
//     SetAutoAcceptPairing(1) is on (and publishes nothing), else publishes
//     OnPairingComfirmRequest(passkey, address) and waits for AcceptSspRequest(1 / 0), which
//     answers for the address of the last indication (0x17e278). The app shows "%06u" of the
//     passkey (SVPTipBtBodingSSPDlg, BTMgr 0xd0c6c) and closes that dialog on
//     OnPairStatusChanged(0);
//   * success: OnPairListChanged, then OnPairStatusChanged(0, "xx:xx:..."); failure:
//     OnPairStatusChanged(module code, "") (14 timeout; 4, 7, 11 errors too);
//   * both auto flags are 0 at boot and not saved; no stock program sets them;
//   * capability 10 (SetCapability(10, on), SVPBtServer::SetCapability 0x17bf70) is
//     CBtSettingService::SetAutoEjectExternConnect: while it is on, OnHandleLinkeyInd refuses
//     every link key (SendLinkKeyRes(0)) before anything else, paired devices included, and
//     publishes nothing, so no device can pair or connect; links that are up stay up. The
//     Bluetooth app sets it during a CarPlay session (BTCarPlay::OnStatusChange) and clears it
//     after. GetCapability cannot read it back (it knows capabilities 1 to 8 only);
//   * GetHfpConnectAddr (60 on the unit: its libbt's stub passes 60) returns 1 and the address
//     of the device holding the phone link (HFP), 0 when none does (SVPBtServer 0x17be34);
//   * paired phones: GetDeviceNum(BT_DEVICE_PAIRED), GetDevice(i, dev, BT_DEVICE_PAIRED);
//     dev.address is lowercase "xx:xx:xx:xx:xx:xx". (GetDevice(std::string) also searches the
//     list of devices seen connecting, so it would call a phone that only tried "paired".)
//
// All SVPBtType types used here are enums (libbt SF::Serializer<T>::serializeContents calls
// SF::serializeEnum: int32 on the wire).
//
// ECNR, what the stock firmware does with it (the BT module's echo-cancel/noise-reduction DSP
// sits between the car microphone and the SoC; MIB2BtOptIn / MIB2BtOptOut are its "optional
// audio" PCM links):
//   * SetECNRAudioPath(path) -> CBtSettingService::SendSetAudioPathCommand(path) (btservice
//     0x16379c): ignored if CBtManager::GetBtStatus() == 1; if path equals the current path or
//     is > 1 it only republishes OnECNRAudioPathChanged(current); otherwise it sends BT module
//     command 0x0343 ("%u" path). The module's confirm (OnHandleSetAudioPathCfm 0x1539c0, result
//     0 = ok) makes it current and publishes OnECNRAudioPathChanged(current). There is no
//     getter. btservice itself forces path 0 on HFP connect (CBtManager::HfpConnectProc(true)
//     0x1383c8), on A2DP connect (BtAudioConnectProc(true) 0x138854) and at BT init
//     (OnBtInitInd 0x153308).
//   * SetECNRParamsMode(mode) (CBtSettingService::SetECNRParamsMode 0x15f17c) only stores the
//     mode (GetECNRParamsMode returns it) and publishes OnECNRParamsModeChanged(mode); nothing
//     is sent to the module there.
//   * Who sets the path to EAP_WIRED_PHONE_MODE (1), the setting under which a wired phone's
//     voice audio reaches the SoC:
//       - CarPlay: appconnect CCarplayDataMgr::OnDisableBtaddrChange (0x40d78, at session start,
//         after dropping the iPhone's BT HFP/A2DP) and OnBtResetFinish (0x3e2b8);
//         OnECNRAudioPathChanged(0) (0x3e418) re-sets 1 while GetCarPlayConnectStatus() is 1 or
//         4, logging "ECNR_AUDIO_PATH Switch EAP_WIRED_PHONE_MODE failed" (up to 3 tries).
//         CarPlay never sets it per stream (Siri streams do nothing ECNR-related).
//       - CarLife: appconnect CCarlifeDataMgr::OnModuleStatusChange (0x471ac), on the
//         ISVP_CarLifeListener event (module 4 = VR): status 1 "VR is running" -> path 1,
//         status 0 "VR is idle" -> path 0. CCarlifeDataMgr::OnECNRAudioPathChanged (0x45a3c),
//         while GetCarLifeConnectStatus() == 3 (CL_ST_VIDEO), forces the path back to
//         GetModuleStatus(4): path 1 with module status 0 -> SetECNRAudioPath(0); path 0 with
//         status 1 -> SetECNRAudioPath(1); other statuses leave it alone.
//       - MirrorLink: QRtpTmServerWrapper::initMicrophone (mirrorlinkservice 0xc6710)
//         SetECNRAudioPath(1) then opens MIB2BtOptIn at 48 kHz; deinitMicrophone (0xc68ac)
//         closes it and SetECNRAudioPath(0).
//   * CarPlay telephony (libcarplayserver AudioStreamPrepare_Init, type 3 only) calls
//     SetECNRParamsMode(0) for an 8 kHz stream, else 1 (logs "...,NARROW_BAND" / "...,BROAD_BAND";
//     appconnect's OnECNRParamsModeChanged logs the same names for 0 / 1).
#ifndef AA_CARLIFE_SHIM_BT_IDL_HPP
#define AA_CARLIFE_SHIM_BT_IDL_HPP

#include <string>
#include <vector>

#include <RCF/Idl.hpp>
#include <SF/string.hpp>
#include <SF/vector.hpp>

namespace SVPBtType {

// Values not recovered; GetBtStatus() == 1 makes SetECNRAudioPath a silent no-op.
enum BT_STATUS        { BT_STATUS_MAX_  = 0x7fffffff };
enum Attribute        { BT_ATTRIB_MAX_  = 0x7fffffff };
// SVPBtServer::SetCapability (0x17bf70) accepts 1..10.
enum Capability       { BT_CAPAB_MAX_   = 0x7fffffff };

// Names from appconnect's log strings (see header); btservice rejects anything > 1.
enum ECNR_AUDIO_PATH {
    EAP_BT_MODE          = 0,   // default: the BT module keeps the mic for HFP
    EAP_WIRED_PHONE_MODE = 1,   // wired projection (CarPlay, CarLife VR, MirrorLink ASR)
    EAP_MAX_             = 0x7fffffff
};

// GetDeviceNum / GetDevice lists (libbt GetADevice 0x155a30, GetDeviceNum 0x154d24).
enum BT_DEVICE_TYPE {
    BT_DEVICE_CONNECTING = 0,
    BT_DEVICE_PAIRED     = 1,
    BT_DEVICE_SEARCHED   = 2,
    BT_DEVICE_TYPE_MAX_  = 0x7fffffff
};
// OnPairStatusChanged: 0 success, else the BT module's failure code (14 timeout).
enum PAIR_STATUS      { PAIR_SUCCESS = 0, PAIR_TIMEOUT = 14, PAIR_STATUS_MAX_ = 0x7fffffff };
// Connect / Disconnect: 1 HFP (+ phonebook, SMS), 2 A2DP/AVRCP, 3 all (unit btservice 0x185424,
// 0x1875fc). OnConnectStatusChanged's profiles: 1 HFP, 2 A2DP/AVRCP, 3 phonebook (PBAP), 4 SMS
// (MAP) (CBtSettingService::OnHandleConnectInd 0x170cac).
enum BT_SERVICE       { BT_SERVICE_HFP = 1, BT_SERVICE_A2DP = 2, BT_SERVICE_ALL = 3, BT_SERVICE_MAX_ = 0x7fffffff };
enum BT_PROFILE_CONNECT { BT_PROFILE_CONNECT_MAX_ = 0x7fffffff }; // 1, 2: probably HFP, A2DP
// Listener argument enums (int32 on the wire); values not needed, not recovered.
enum AUDIO_PATH_STATUS       { AUDIO_PATH_STATUS_MAX_       = 0x7fffffff };
enum AVP_AUTO_CONNECT_STATUS { AVP_AUTO_CONNECT_STATUS_MAX_ = 0x7fffffff };
enum BT_UPDATE_STATUS        { BT_UPDATE_STATUS_MAX_        = 0x7fffffff };
enum CALL_ACTION_STATUS      { CALL_ACTION_STATUS_MAX_      = 0x7fffffff };
enum COMBINE_CALL_STATUS     { COMBINE_CALL_STATUS_MAX_     = 0x7fffffff };
enum MsgLoadStatus           { MSG_LOAD_STATUS_MAX_         = 0x7fffffff };
enum PLAY_STATUS             { PLAY_STATUS_MAX_             = 0x7fffffff };

// libbt SF::Serializer<SVPBtDevice>::serializeContents (0xd3c60), in this order; filled by
// SVPBtServer::GetDevice (0x183ee4). The flags and the last field are not named.
struct SVPBtDevice {
    std::string name;
    std::string address;        // "xx:xx:xx:xx:xx:xx", lowercase
    bool b8 = false, b9 = false, b10 = false;
    BT_SERVICE svc12 = BT_SERVICE(0);
    BT_SERVICE svc16 = BT_SERVICE(0);
    unsigned int u20 = 0;

    void serialize(SF::Archive& ar) { ar & name & address & b8 & b9 & b10 & svc12 & svc16 & u20; }
};

// Names from libcarplayserver's / appconnect's log strings (see header).
enum ECNR_PARAMS_MODE {
    NARROW_BAND = 0,            // 8 kHz voice
    BROAD_BAND  = 1,            // wideband (CarPlay uses it for every rate except 8000)
    EPM_MAX_    = 0x7fffffff
};

} // namespace SVPBtType

RCF_BEGIN(SVP_BtService, "SVP_BtService")
    RCF_METHOD_V1(void, OpenBt, int)                                                       // 0
    RCF_METHOD_V1(void, ShutdownBt, int)                                                   // 1
    RCF_METHOD_R0(SVPBtType::BT_STATUS, GetBtStatus)                                       // 2
    RCF_METHOD_R0(int, GetLastBtEnable)                                                    // 3
    RCF_METHOD_R0(unsigned int, GetBtVersion)                                              // 4
    RCF_METHOD_R2(int, GetAttribute, SVPBtType::Attribute, std::string&)                   // 5
    RCF_METHOD_R2(int, SetAttribute, SVPBtType::Attribute, const std::string&)             // 6
    RCF_METHOD_R1(int, GetCapability, SVPBtType::Capability)                               // 7
    RCF_METHOD_R2(int, SetCapability, SVPBtType::Capability, int)                          // 8
    RCF_METHOD_V1(void, ConfigTrace, unsigned short)                                       // 9
    RCF_METHOD_R1(int, SetECNRParams, std::vector<unsigned char>&)                         // 10
    RCF_METHOD_V1(void, SetECNRAudioPath, SVPBtType::ECNR_AUDIO_PATH)                      // 11
    RCF_METHOD_V1(void, SetECNRParamsMode, SVPBtType::ECNR_PARAMS_MODE)                    // 12
    RCF_METHOD_R0(SVPBtType::ECNR_PARAMS_MODE, GetECNRParamsMode)                          // 13
    RCF_METHOD_PLACEHOLDER()                                                             // 14
    RCF_METHOD_PLACEHOLDER()                                                             // 15
    RCF_METHOD_PLACEHOLDER()                                                             // 16
    RCF_METHOD_PLACEHOLDER()                                                             // 17
    RCF_METHOD_PLACEHOLDER()                                                             // 18
    RCF_METHOD_PLACEHOLDER()                                                             // 19
    RCF_METHOD_PLACEHOLDER()                                                             // 20
    RCF_METHOD_PLACEHOLDER()                                                             // 21
    RCF_METHOD_PLACEHOLDER()                                                             // 22
    RCF_METHOD_PLACEHOLDER()                                                             // 23
    RCF_METHOD_PLACEHOLDER()                                                             // 24
    RCF_METHOD_PLACEHOLDER()                                                             // 25
    RCF_METHOD_PLACEHOLDER()                                                             // 26
    RCF_METHOD_PLACEHOLDER()                                                             // 27
    RCF_METHOD_PLACEHOLDER()                                                             // 28
    RCF_METHOD_PLACEHOLDER()                                                             // 29
    RCF_METHOD_PLACEHOLDER()                                                             // 30
    RCF_METHOD_PLACEHOLDER()                                                             // 31
    RCF_METHOD_PLACEHOLDER()                                                             // 32
    RCF_METHOD_PLACEHOLDER()                                                             // 33
    RCF_METHOD_PLACEHOLDER()                                                             // 34
    RCF_METHOD_V2(void, Connect, std::string, SVPBtType::BT_SERVICE)                     // 35
    RCF_METHOD_PLACEHOLDER()                                                             // 36
    RCF_METHOD_V1(void, Disconnect, SVPBtType::BT_SERVICE)                               // 37
    RCF_METHOD_PLACEHOLDER()                                                             // 38 DisconnectForce
    RCF_METHOD_V1(void, AcceptSspRequest, int)                                           // 39
    RCF_METHOD_V1(void, AcceptPinCodeRequest, std::string)                               // 40
    RCF_METHOD_PLACEHOLDER()                                                             // 41
    RCF_METHOD_PLACEHOLDER()                                                             // 42
    RCF_METHOD_R1(int, SetAutoAcceptPairing, int)                                        // 43
    RCF_METHOD_V1(void, SetAutoAcceptConnect, int)                                       // 44
    RCF_METHOD_R0(int, GetAutoAcceptConnect)                                             // 45
    RCF_METHOD_PLACEHOLDER()                                                             // 46
    RCF_METHOD_PLACEHOLDER()                                                             // 47
    RCF_METHOD_PLACEHOLDER()                                                             // 48
    RCF_METHOD_PLACEHOLDER()                                                             // 49
    RCF_METHOD_PLACEHOLDER()                                                             // 50
    RCF_METHOD_PLACEHOLDER()                                                             // 51 RemoveConnectListCmd (the unit's; not in the dump)
    RCF_METHOD_R1(int, GetDeviceNum, SVPBtType::BT_DEVICE_TYPE)                          // 52
    RCF_METHOD_PLACEHOLDER()                                                             // 53
    RCF_METHOD_PLACEHOLDER()                                                             // 54
    RCF_METHOD_R3(int, GetDevice, int, SVPBtType::SVPBtDevice&, SVPBtType::BT_DEVICE_TYPE) // 55
    RCF_METHOD_PLACEHOLDER()                                                             // 56
    RCF_METHOD_PLACEHOLDER()                                                             // 57
    RCF_METHOD_PLACEHOLDER()                                                             // 58
    RCF_METHOD_PLACEHOLDER()                                                             // 59
    RCF_METHOD_R1(int, GetHfpConnectAddr, std::string&)                                  // 60
    RCF_METHOD_PLACEHOLDER()                                                             // 61
    RCF_METHOD_R1(int, GetAvpConnectAddr, std::string&)                                  // 62
RCF_END(SVP_BtService)

RCF_BEGIN(SVP_BtListener, "SVP_BtListener")
    RCF_METHOD_V2(void, OnBtOpenStatusChanged, SVPBtType::BT_STATUS, int)                           // 0
    RCF_METHOD_V0(void, OnBtResetFinish)                                                            // 1
    RCF_METHOD_V1(void, OnSetVisibleStatusChanged, int)                                             // 2
    RCF_METHOD_V1(void, OnSetBtNameStatusChanged, int)                                              // 3
    RCF_METHOD_V1(void, OnECNRAudioPathChanged, SVPBtType::ECNR_AUDIO_PATH)                         // 4
    RCF_METHOD_V1(void, OnECNRParamsModeChanged, SVPBtType::ECNR_PARAMS_MODE)                       // 5
    RCF_METHOD_V0(void, OnSearchDeviceInd)                                                          // 6
    RCF_METHOD_V0(void, OnBtUartDebugStatusChanged)                                                 // 7
    RCF_METHOD_V0(void, OnSearchDeviceCmp)                                                          // 8
    RCF_METHOD_V0(void, OnSearchDeviceStart)                                                        // 9
    RCF_METHOD_V1(void, OnDeviceNameChanged, unsigned int)                                          // 10
    RCF_METHOD_V1(void, OnInqureProfileCmp, int)                                                    // 11
    RCF_METHOD_V2(void, OnPairingComfirmRequest, unsigned int, std::string)                         // 12
    RCF_METHOD_V3(void, OnPairingPinCodeRequest, unsigned int, std::string, std::string)            // 13
    RCF_METHOD_V1(void, OnRemoteConnectRequest, std::string)                                        // 14
    RCF_METHOD_V2(void, OnReturnRemoteDeviceName, std::string, std::string)                         // 15
    RCF_METHOD_V2(void, OnPairStatusChanged, SVPBtType::PAIR_STATUS, std::string)                   // 16
    RCF_METHOD_V1(void, OnDeviceNeedPair, std::string)                                              // 17
    RCF_METHOD_V2(void, OnSupportProfileChanged, int, int)                                          // 18
    RCF_METHOD_V0(void, OnPairListChanged)                                                          // 19
    RCF_METHOD_V0(void, OnSearchDeviceStop)                                                         // 20
    RCF_METHOD_V0(void, OnReplaceConnectRequest)                                                    // 21
    RCF_METHOD_V0(void, OnFavoriteChanged)                                                          // 22
    RCF_METHOD_V0(void, OnUserProfileChanged)                                                       // 23
    RCF_METHOD_V0(void, OnAudioProfileEnable)                                                       // 24
    RCF_METHOD_V0(void, OnPhoneReminderEnable)                                                      // 25
    RCF_METHOD_V3(void, OnConnectStatusChanged, SVPBtType::BT_PROFILE_CONNECT, int, std::string)    // 26
    RCF_METHOD_V0(void, OnConnectCmp)                                                               // 27
    RCF_METHOD_V1(void, OnBtConnnectDisableStatusChanged, unsigned char)                            // 28
    RCF_METHOD_V2(void, OnDisconnectStatusChanged, SVPBtType::BT_PROFILE_CONNECT, int)              // 29
    RCF_METHOD_V0(void, OnDisconnectCmp)                                                            // 30
    RCF_METHOD_V0(void, OnRemoteSignalChanged)                                                      // 31
    RCF_METHOD_V0(void, OnRemoteRoamingChanged)                                                     // 32
    RCF_METHOD_V0(void, OnRemoteBatteryChanged)                                                     // 33
    RCF_METHOD_V1(void, OnAudioPathChanged, SVPBtType::AUDIO_PATH_STATUS)                           // 34
    RCF_METHOD_V0(void, OnServiceStatusChanged)                                                     // 35
    RCF_METHOD_V0(void, OnServiceNameChanged)                                                       // 36
    RCF_METHOD_V0(void, OnServiceRegistrationStatusChanged)                                         // 37
    RCF_METHOD_V0(void, OnVoiceRecognitionStatusChanged)                                            // 38
    RCF_METHOD_V0(void, OnExResponseStatusChanged)                                                  // 39
    RCF_METHOD_V1(void, OnCallStatusChanged, SVPBtType::COMBINE_CALL_STATUS)                        // 40
    RCF_METHOD_V0(void, OnConferenceCallCountChanged)                                               // 41
    RCF_METHOD_V1(void, OnCallTimeChanged, unsigned int)                                            // 42
    RCF_METHOD_V0(void, OnCallSupportActionChanged)                                                 // 43
    RCF_METHOD_V0(void, OnSpeakerPrivacyModeChanged)                                                // 44
    RCF_METHOD_V1(void, OnMicrophoneStatus, int)                                                    // 45
    RCF_METHOD_V2(void, OnPhoneBookNumDownloadCmp, int, unsigned int)                               // 46
    RCF_METHOD_V2(void, OnPhoneBookDownloadCmp, int, int)                                           // 47
    RCF_METHOD_V0(void, OnPhoneBookImportCmp)                                                       // 48
    RCF_METHOD_V2(void, OnPbDownloadCountChanged, int, int)                                         // 49
    RCF_METHOD_V1(void, OnCallHistoryDonwloadCmp, int)                                              // 50
    RCF_METHOD_V0(void, OnClearMissedCall)                                                          // 51
    RCF_METHOD_V2(void, OnMusicPlayStatusChanged, SVPBtType::PLAY_STATUS, int)                      // 52
    RCF_METHOD_V1(void, OnMusicPlayStatusBeforeDisconnect, SVPBtType::PLAY_STATUS)                  // 53
    RCF_METHOD_V2(void, OnMusicPlayModeChanged, unsigned char, unsigned char)                       // 54
    RCF_METHOD_V1(void, OnMusicPlaytimeChanged, unsigned int)                                       // 55
    RCF_METHOD_V0(void, OnMusicPlayTrackChanged)                                                    // 56
    RCF_METHOD_V0(void, OnMusicID3Changed)                                                          // 57
    RCF_METHOD_V0(void, OnMusicPlayingListChanged)                                                  // 58
    RCF_METHOD_V0(void, OnMusicPlaytingListCmp)                                                     // 59
    RCF_METHOD_V0(void, OnMusicBrowsingChanged)                                                     // 60
    RCF_METHOD_V0(void, OnBrowserMediaInfoChanged)                                                  // 61
    RCF_METHOD_V0(void, OnMusicPlayerChanged)                                                       // 62
    RCF_METHOD_V0(void, OnAvrcpVersionChanged)                                                      // 63
    RCF_METHOD_V2(void, OnCurBrowserPathChanged, int, unsigned int)                                 // 64
    RCF_METHOD_V2(void, OnReturnUidTitle, std::string, std::string)                                 // 65
    RCF_METHOD_V1(void, OnBtAudioValidChanged, int)                                                 // 66
    RCF_METHOD_V1(void, OnMessageDownloadStatus, SVPBtType::MsgLoadStatus)                          // 67
    RCF_METHOD_V0(void, OnDeleteSms)                                                                // 68
    RCF_METHOD_V1(void, OnSmsStatusChanged, int)                                                    // 69
    RCF_METHOD_V0(void, OnClearUnreadMessage)                                                       // 70
    RCF_METHOD_V1(void, OnBtUpdateStatusChanged, SVPBtType::BT_UPDATE_STATUS)                       // 71
    RCF_METHOD_V1(void, OnBtUpdateProgress, unsigned short)                                         // 72
    RCF_METHOD_V0(void, OnMusicChanged)                                                             // 73
    RCF_METHOD_V3(void, OnCallActionStatus, unsigned int, SVPBtType::CALL_ACTION_STATUS, std::string)// 74
    RCF_METHOD_V1(void, OnReconnectStatusChanged, int)                                              // 75
    RCF_METHOD_V1(void, OnAvpAutoConnectStatus, SVPBtType::AVP_AUTO_CONNECT_STATUS)                 // 76
    RCF_METHOD_V0(void, OnErrorLogUpdate)                                                           // 77
RCF_END(SVP_BtListener)

// Every SVP_BtListener event as a no-op: a servant derives from this and declares the ones it
// wants (hiding these), so an event it ignores still has a method (RCF asserts on an id with
// none: step 3 runs 34-36).
struct SVPBtListenerNoop {
    void OnBtOpenStatusChanged(SVPBtType::BT_STATUS, int) {}
    void OnBtResetFinish() {}
    void OnSetVisibleStatusChanged(int) {}
    void OnSetBtNameStatusChanged(int) {}
    void OnECNRAudioPathChanged(SVPBtType::ECNR_AUDIO_PATH) {}
    void OnECNRParamsModeChanged(SVPBtType::ECNR_PARAMS_MODE) {}
    void OnSearchDeviceInd() {}
    void OnBtUartDebugStatusChanged() {}
    void OnSearchDeviceCmp() {}
    void OnSearchDeviceStart() {}
    void OnDeviceNameChanged(unsigned int) {}
    void OnInqureProfileCmp(int) {}
    void OnPairingComfirmRequest(unsigned int, const std::string&) {}
    void OnPairingPinCodeRequest(unsigned int, const std::string&, const std::string&) {}
    void OnRemoteConnectRequest(const std::string&) {}
    void OnReturnRemoteDeviceName(const std::string&, const std::string&) {}
    void OnPairStatusChanged(SVPBtType::PAIR_STATUS, const std::string&) {}
    void OnDeviceNeedPair(const std::string&) {}
    void OnSupportProfileChanged(int, int) {}
    void OnPairListChanged() {}
    void OnSearchDeviceStop() {}
    void OnReplaceConnectRequest() {}
    void OnFavoriteChanged() {}
    void OnUserProfileChanged() {}
    void OnAudioProfileEnable() {}
    void OnPhoneReminderEnable() {}
    void OnConnectStatusChanged(SVPBtType::BT_PROFILE_CONNECT, int, const std::string&) {}
    void OnConnectCmp() {}
    void OnBtConnnectDisableStatusChanged(unsigned char) {}
    void OnDisconnectStatusChanged(SVPBtType::BT_PROFILE_CONNECT, int) {}
    void OnDisconnectCmp() {}
    void OnRemoteSignalChanged() {}
    void OnRemoteRoamingChanged() {}
    void OnRemoteBatteryChanged() {}
    void OnAudioPathChanged(SVPBtType::AUDIO_PATH_STATUS) {}
    void OnServiceStatusChanged() {}
    void OnServiceNameChanged() {}
    void OnServiceRegistrationStatusChanged() {}
    void OnVoiceRecognitionStatusChanged() {}
    void OnExResponseStatusChanged() {}
    void OnCallStatusChanged(SVPBtType::COMBINE_CALL_STATUS) {}
    void OnConferenceCallCountChanged() {}
    void OnCallTimeChanged(unsigned int) {}
    void OnCallSupportActionChanged() {}
    void OnSpeakerPrivacyModeChanged() {}
    void OnMicrophoneStatus(int) {}
    void OnPhoneBookNumDownloadCmp(int, unsigned int) {}
    void OnPhoneBookDownloadCmp(int, int) {}
    void OnPhoneBookImportCmp() {}
    void OnPbDownloadCountChanged(int, int) {}
    void OnCallHistoryDonwloadCmp(int) {}
    void OnClearMissedCall() {}
    void OnMusicPlayStatusChanged(SVPBtType::PLAY_STATUS, int) {}
    void OnMusicPlayStatusBeforeDisconnect(SVPBtType::PLAY_STATUS) {}
    void OnMusicPlayModeChanged(unsigned char, unsigned char) {}
    void OnMusicPlaytimeChanged(unsigned int) {}
    void OnMusicPlayTrackChanged() {}
    void OnMusicID3Changed() {}
    void OnMusicPlayingListChanged() {}
    void OnMusicPlaytingListCmp() {}
    void OnMusicBrowsingChanged() {}
    void OnBrowserMediaInfoChanged() {}
    void OnMusicPlayerChanged() {}
    void OnAvrcpVersionChanged() {}
    void OnCurBrowserPathChanged(int, unsigned int) {}
    void OnReturnUidTitle(const std::string&, const std::string&) {}
    void OnBtAudioValidChanged(int) {}
    void OnMessageDownloadStatus(SVPBtType::MsgLoadStatus) {}
    void OnDeleteSms() {}
    void OnSmsStatusChanged(int) {}
    void OnClearUnreadMessage() {}
    void OnBtUpdateStatusChanged(SVPBtType::BT_UPDATE_STATUS) {}
    void OnBtUpdateProgress(unsigned short) {}
    void OnMusicChanged() {}
    void OnCallActionStatus(unsigned int, SVPBtType::CALL_ACTION_STATUS, const std::string&) {}
    void OnReconnectStatusChanged(int) {}
    void OnAvpAutoConnectStatus(SVPBtType::AVP_AUTO_CONNECT_STATUS) {}
    void OnErrorLogUpdate() {}
};

const int kBtServicePort           = 30315;   // RcfClient<SVP_BtService>
const int kBtListenerPort          = 30316;   // SVP_BtListener publisher (127.0.0.1)
const int kBtListenerPublisherPort = 30316;   // SVP_BtListener publisher (not declared here)

#endif // AA_CARLIFE_SHIM_BT_IDL_HPP
