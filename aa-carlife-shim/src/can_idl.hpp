// SPDX-License-Identifier: GPL-2.0-only
//
// The SVP CAN service (svp/bin/canservice, registry svp.svc.can, started by svp/etc/init.sh):
// vehicle data the MCU forwards from CAN/BAP, among it the CarPlay info record (day/night,
// transmission state, right-hand drive). Recovered from the stock client svp/lib/libcan.so.1.0.0
// (SVPCanService_Client, RCF 2.2) and checked against canservice (SVPCanService_Server):
//
//   * service "SVP_CanService" (SVP_CanService::getInterfaceName, libcan 0x856ec) on TCP 30705:
//     the client builds RcfClient<SVP_CanService>(TcpEndpoint("0.0.0.0", 30705), "")
//     (SVPCanService_Client ctor 0x7a590; "0.0.0.0" connects to the local host on Linux);
//     canservice serves RcfServer(TcpEndpoint("0.0.0.0", 30705)) (ctor 0x7fc48) with
//     bind<SVP_CanService>(impl, "") (InitInstance 0x8146c). InitInstance in the client only
//     waits for the registry name: SVP_WaitService("svp.svc.can", 5000).
//   * method IDs as the UNIT's libcan (step 3 capture, md5 a9f60f7c) numbers them: it adds
//     GetCanPsrsoStsActive (5) and GetDescAdaptionSpeedLimit (9) to the firmware dump's, so the
//     dump's 5.. are one or two higher there (the shim calls only 2 and 3, the same in both).
//     The listener's layout on the unit is not checked yet (canservice, its publisher, is
//     captured from step 3 on).
//   * method IDs (dump): the fnId each libcan RcfClient<SVP_CanService>::M(CallOptions&, ...) stub passes
//     to FutureImplBase; types: the AllocateClientParameters<> each stub uses. Cross-checked
//     against the call-descriptor name each canservice StubAccess::invoke<..., int_<N>> loads.
//     GetCanIgnitionMode is overloaded in the stock IDL (0 and 1). The getters with no argument
//     (0, 2, 21) are void: for 0 and 2 canservice answers by publishing (SVPRmtCan::
//     GetCanIgnitionMode() 0xf0f40 / GetCanVehicleSpeed() 0xf0fd0 call the server's
//     OnCanIgnitionModeChanged / OnCanVehicleSpeedChanged); 21 was not followed.
//   * listener "SVP_CanListener" is RCF publish/subscribe, not a callback connection:
//     canservice runs a second RcfServer(TcpEndpoint("127.0.0.1", 30706)) (ctor 0x7fcc4) and
//     createPublisher<SVP_CanListener>(PublisherParms()) on it (InitInstance 0x814ec), so the
//     topic is the default, the interface name. The stock subscriber
//     (SVPCanService_Client::InitListener 0x7be70) does:
//         RcfServer sub(TcpEndpoint(-1));              // ctor 0x7a62c; -1 = no listening socket
//         sub.start();
//         SubscriptionParms p;
//         p.setPublisherEndpoint(TcpEndpoint("127.0.0.1", 30706));    // 0x7bf40
//         sub.createSubscription<SVP_CanListener>(servant, p);        // 0x7bf8c
//     The servant must implement every SVP_CanListener method below. Listener IDs and types are
//     from canservice's RcfClient<SVP_CanListener> (publisher) stubs, cross-checked against
//     libcan's StubAccess::invoke<RcfClient<SVP_CanListener>, int_<N>> names.
//     canservice publishes OnCanCarplayInfoChanged each time the MCU message arrives
//     (SVPRmtCan::MsgDecode 0xf34bc -> SVPCanService_Server vtable +20), with no change filter.
//
// SVPCanCarplayInfoType: SF::Serializer<SVPCanType::SVPCanCarplayInfoType>::serializeContents
// (libcan 0x83844) writes three unsigned chars, struct offsets 0, 1, 2, in that order.
// test.can names them (printf at 0xb0e8 and TestListener::OnCanCarplayInfoChanged 0xc170):
// offset 0 DayNight, 1 TransmissionState, 2 RightHandDrive. sizeof is 3 (SVPRmtCan::
// GetCanCarplayInfo 0xf1054 copies a halfword and a byte). canservice copies the three bytes
// raw from the MCU message (MsgDecode 0xf34bc: msg[4], msg[5], msg[6]); they start at 0.
// DayNight meaning, from the stock consumer appconnect: CCarplayDataMgr::
// OnCanCarplayInfoChanged (0x3e354) passes the raw DayNight byte to ISVPCarPlayService vtable
// +124 = SVPCarPlay_Client::SetNightMode(int) (libcarplay vtable), which carplayservice logs as
// "isNightMode = %d" and hands to carplayserver_AccessorySendSetNightMode (AirPlay nightMode);
// SVPDataMgr::OnCanCarplayInfoChanged (0x49d28) maps DayNight != 0 to night for MirrorLink.
// So 0 = day, non-zero = night (the value the MCU sends for night is presumably 1; not seen
// in the code). ipodservice maps TransmissionState for iAP2 PASCD (SVPIpodCANListener::
// SendThreadProc 0xc9440): 1 'P', 2 'R', 3 'N', 4..6 'D', anything else 'U'.
//
// How stock CarPlay gets day/night, end to end (the path the shim copies). appconnect
// (storage/app/sv.g5.mib2.appconnect/bin/appconnect, via libcan) is the only caller of
// SetNightMode in the firmware (searched every client of the CarPlay service for vtable +124):
//   1. CCarplayDataMgr::InitService (0x3eb78) -> InitCanService (0x3eb0c): SVP_CreateCanService();
//      InitInstance (vtable +100: SVP_WaitService("svp.svc.can")); AddListener(this)
//      (ISVPListener vtable +8). libcan's ISVPService::OnListenerAdded (0x82598) calls
//      InitListener (vtable +108) for the first listener: the subscription above.
//   2. InitCarplayInfo (0x3e94c): GetCanCarplayInfo (vtable +136, method 3) once, then
//      SetNightMode(DayNight) and SetRightHandDrive(RightHandDrive) on the CarPlay service.
//   3. OnCanCarplayInfoChanged (0x3e354, reached from libcan's subscriber servant
//      SVPCanService_Client::OnCanCarplayInfoChanged, which fans out to every listener): if
//      DayNight differs from the cached byte, SetNightMode(DayNight); same for RightHandDrive.
// carplayservice only relays (SVPCarPlay_Server::SetNightMode -> AirPlay nightMode). It does
// AddListener on the CAN client too (InitInstance 0x695f0) but overrides only
// OnCanIgnitionModeChanged and OnBapScreenViewRequestEvent. exlapservice's CanClient does not
// override OnCanCarplayInfoChanged nor call GetCanCarplayInfo; "System_DayNight" there is an
// ExLAP channel name, not a source.
//
// The other structs are all-unsigned-char records (their serializeContents in libcan, field
// order as listed); field names other than the ones test.can prints are not known. Every
// other SVPCanType is an enum (serializeContents calls SF::serializeEnum: int32 on the wire);
// their values are not needed by the shim and are not recovered.
#ifndef AA_CARLIFE_SHIM_CAN_IDL_HPP
#define AA_CARLIFE_SHIM_CAN_IDL_HPP

#include <vector>

#include <RCF/Idl.hpp>
#include <SF/vector.hpp>

namespace SVPCanType {

// SF writes enums as a raw int32, so only the numeric value matters; the max sentinel keeps
// any int32 read back a valid value of the type.
enum SVPCanIgnModeType            { CAN_IGN_MODE_MAX_          = 0x7fffffff };
enum SVPDescActuatorIdType        { DESC_ACTUATOR_ID_MAX_      = 0x7fffffff };
enum SVPDescActuatorCtrlType      { DESC_ACTUATOR_CTRL_MAX_    = 0x7fffffff };
enum SVPDescDatasetIdType         { DESC_DATASET_ID_MAX_       = 0x7fffffff };
enum SVPDescRoutineIdType         { DESC_ROUTINE_ID_MAX_       = 0x7fffffff };
enum SVPDescRoutineCtrlType       { DESC_ROUTINE_CTRL_MAX_     = 0x7fffffff };
enum SVPDescMeasuredIdType        { DESC_MEASURED_ID_MAX_      = 0x7fffffff };
enum SVPBapScreenViewIdType       { BAP_SCREEN_VIEW_ID_MAX_    = 0x7fffffff };
enum SVPBapScreenCtrlType         { BAP_SCREEN_CTRL_MAX_       = 0x7fffffff };
enum SVPBapPhoneDataInfoType      { BAP_PHONE_DATA_INFO_MAX_   = 0x7fffffff };
enum SVPBapScreenSetIdType        { BAP_SCREEN_SET_ID_MAX_     = 0x7fffffff };
enum SVPBapScreenSetParamIdType   { BAP_SCREEN_SET_PARAM_MAX_  = 0x7fffffff };
enum SVPBapWakeupFlagType         { BAP_WAKEUP_FLAG_MAX_       = 0x7fffffff };
enum SVPBapWakeupStateType        { BAP_WAKEUP_STATE_MAX_      = 0x7fffffff };
enum SVPBapScreenOffFuncType      { BAP_SCREEN_OFF_FUNC_MAX_   = 0x7fffffff };
enum SVPBapScreenOffTimerType     { BAP_SCREEN_OFF_TIMER_MAX_  = 0x7fffffff };
enum SVPBapRvcOpsStateType        { BAP_RVC_OPS_STATE_MAX_     = 0x7fffffff };
enum SVPBapPhoneCallActionType    { BAP_PHONE_CALL_ACTION_MAX_ = 0x7fffffff };
enum SVPBapFsgStateType           { BAP_FSG_STATE_MAX_         = 0x7fffffff };
enum SVPBapGeneralPopupType       { BAP_GENERAL_POPUP_MAX_     = 0x7fffffff };
enum SVPStartStopValueIdType      { START_STOP_VALUE_ID_MAX_   = 0x7fffffff };

// GetCanCarplayInfo / OnCanCarplayInfoChanged. Wire: 3 x unsigned char, in this order.
struct SVPCanCarplayInfoType {
    unsigned char DayNight          = 0;    // 0 = day, non-zero = night (see header)
    unsigned char TransmissionState = 0;
    unsigned char RightHandDrive    = 0;

    void serialize(SF::Archive& ar) { ar & DayNight & TransmissionState & RightHandDrive; }
};

// serializeContents (libcan 0x837e0) writes 2 x unsigned char; test.can TestBap prints offset 0 as
// timeFormat and 1 as dateFormat.
struct SVPBapClockFormatType {
    unsigned char timeFormat = 0;
    unsigned char dateFormat = 0;

    void serialize(SF::Archive& ar) { ar & timeFormat & dateFormat; }
};

// serializeContents (libcan 0x838c8): 3 x unsigned char.
struct SVPDescAdaptionBtSettingType {
    unsigned char b0 = 0, b1 = 0, b2 = 0;

    void serialize(SF::Archive& ar) { ar & b0 & b1 & b2; }
};

// serializeContents (libcan 0x8394c): 6 x unsigned char.
struct SVPDescAdaptionVehicleConfigType {
    unsigned char b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0;

    void serialize(SF::Archive& ar) { ar & b0 & b1 & b2 & b3 & b4 & b5; }
};

// serializeContents (libcan 0x836cc): 7 x unsigned char; test.can's listener prints offset 0
// as BtCountryLimitation and 1 as BtMultiMedia.
struct SVPDescCodingBtSettingType {
    unsigned char BtCountryLimitation = 0, BtMultiMedia = 0;
    unsigned char b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;

    void serialize(SF::Archive& ar) {
        ar & BtCountryLimitation & BtMultiMedia & b2 & b3 & b4 & b5 & b6;
    }
};

// serializeContents (libcan 0x83600): 5 x unsigned char.
struct SVPDescCodingVehicleImageType {
    unsigned char b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0;

    void serialize(SF::Archive& ar) { ar & b0 & b1 & b2 & b3 & b4; }
};

} // namespace SVPCanType

RCF_BEGIN(SVP_CanService, "SVP_CanService")
    RCF_METHOD_V0(void, GetCanIgnitionMode)                                                  // 0
    RCF_METHOD_V1(void, GetCanIgnitionMode, SVPCanType::SVPCanIgnModeType&)                  // 1
    RCF_METHOD_V0(void, GetCanVehicleSpeed)                                                  // 2
    RCF_METHOD_V1(void, GetCanCarplayInfo, SVPCanType::SVPCanCarplayInfoType&)               // 3
    RCF_METHOD_R0(int, GetCanAudioLoweringPresent)                                           // 4
    RCF_METHOD_PLACEHOLDER()                                                                 // 5 GetCanPsrsoStsActive (unit)
    RCF_METHOD_V1(void, GetDescAdaptionBtSetting, SVPCanType::SVPDescAdaptionBtSettingType&) // 6
    RCF_METHOD_V1(void, GetDescAdaptionVehicleCfg,
                  SVPCanType::SVPDescAdaptionVehicleConfigType&)                             // 7
    RCF_METHOD_V1(void, GetDescAdaptionStickerBits, unsigned short&)                         // 8
    RCF_METHOD_PLACEHOLDER()                                                                 // 9 GetDescAdaptionSpeedLimit (unit)
    RCF_METHOD_V1(void, GetDescCodingVehicleImage, SVPCanType::SVPDescCodingVehicleImageType&) // 10
    RCF_METHOD_V1(void, GetDescCodingBtSetting, SVPCanType::SVPDescCodingBtSettingType&)     // 11
    RCF_METHOD_V1(void, GetDescCodingHmiSkin, unsigned char&)                                // 12
    RCF_METHOD_V1(void, GetDescCodingStartScreen, unsigned char&)                            // 13
    RCF_METHOD_V2(void, SetDescActuatorTestDone, SVPCanType::SVPDescActuatorIdType,
                  SVPCanType::SVPDescActuatorCtrlType)                                       // 14
    RCF_METHOD_V1(void, SetDescDatasetDownloadDone, SVPCanType::SVPDescDatasetIdType)        // 15
    RCF_METHOD_V3(void, SetDescRoutineTestDone, SVPCanType::SVPDescRoutineIdType,
                  SVPCanType::SVPDescRoutineCtrlType, unsigned char)                         // 16
    RCF_METHOD_V2(void, SetDescMeasuredValue, SVPCanType::SVPDescMeasuredIdType,
                  std::vector<unsigned char>&)                                               // 17
    RCF_METHOD_V1(void, GetDescCarFuncAdaption, std::vector<unsigned char>&)                 // 18
    RCF_METHOD_V2(void, SetBapScreenViewRequest, SVPCanType::SVPBapScreenViewIdType,
                  SVPCanType::SVPBapScreenCtrlType)                                          // 19
    RCF_METHOD_V1(void, SetBapScreenViewOptionRequest, SVPCanType::SVPBapScreenViewIdType)   // 20
    RCF_METHOD_V1(void, SetBapScreenVauleRequest, SVPCanType::SVPBapScreenViewIdType)        // 21
    RCF_METHOD_V2(void, SetBapPhoneDataInfo, SVPCanType::SVPBapPhoneDataInfoType,
                  std::vector<unsigned char>&)                                               // 22
    RCF_METHOD_V0(void, GetBapStatusBarVal)                                                  // 23
    RCF_METHOD_V2(void, SetBapScreenItemVal, SVPCanType::SVPBapScreenSetIdType, unsigned char) // 24
    RCF_METHOD_V2(void, SetBapScreenItemParams, SVPCanType::SVPBapScreenSetParamIdType,
                  std::vector<unsigned char>&)                                               // 25
    RCF_METHOD_V1(void, GetBapClockFormat, SVPCanType::SVPBapClockFormatType&)               // 26
    RCF_METHOD_V2(void, SetBapWakeupFlag, SVPCanType::SVPBapWakeupFlagType,
                  SVPCanType::SVPBapWakeupStateType)                                         // 27
    RCF_METHOD_V2(void, SetBapScreenOffTimer, SVPCanType::SVPBapScreenOffFuncType,
                  SVPCanType::SVPBapScreenOffTimerType)                                      // 28
    RCF_METHOD_V1(void, GetBapRvcOpsState, SVPCanType::SVPBapRvcOpsStateType&)               // 29
    // 0358 (PQ) numbering. The 0394 (MQB) libcan inserts GetBapColorListValue as 30 and moves
    // this one to 31; methods 0..29 and the listener are the same. Never called by the shim.
    RCF_METHOD_R2(int, SendCanServiceMessage, unsigned short, std::vector<unsigned char>&)   // 30
RCF_END(SVP_CanService)

RCF_BEGIN(SVP_CanListener, "SVP_CanListener")
    RCF_METHOD_V1(void, OnCanIgnitionModeChanged, SVPCanType::SVPCanIgnModeType)             // 0
    RCF_METHOD_V1(void, OnCanVehicleSpeedChanged, unsigned short)                            // 1
    RCF_METHOD_V1(void, OnCanCarplayInfoChanged, SVPCanType::SVPCanCarplayInfoType)          // 2
    RCF_METHOD_V1(void, OnCanAudioLoweringChange, int)                                       // 3
    RCF_METHOD_V1(void, OnDescAdaptionBtSettingChanged,
                  SVPCanType::SVPDescAdaptionBtSettingType)                                  // 4
    RCF_METHOD_V1(void, OnDescAdaptionVehicleCfgChanged,
                  SVPCanType::SVPDescAdaptionVehicleConfigType)                              // 5
    RCF_METHOD_V1(void, OnDescAdaptionStickerBitsChanged, unsigned short)                    // 6
    RCF_METHOD_V1(void, OnDescCodingVehicleImageChanged,
                  SVPCanType::SVPDescCodingVehicleImageType)                                 // 7
    RCF_METHOD_V1(void, OnDescCodingBtSettingChanged, SVPCanType::SVPDescCodingBtSettingType) // 8
    RCF_METHOD_V2(void, OnDescActuatorTestEvent, SVPCanType::SVPDescActuatorIdType,
                  SVPCanType::SVPDescActuatorCtrlType)                                       // 9
    RCF_METHOD_V2(void, OnDescDatasetDownloadEvent, SVPCanType::SVPDescDatasetIdType,
                  std::vector<unsigned char>&)                                               // 10
    RCF_METHOD_V3(void, OnDescRoutineTestEvent, SVPCanType::SVPDescRoutineIdType,
                  SVPCanType::SVPDescRoutineCtrlType, unsigned char)                         // 11
    RCF_METHOD_V1(void, OnDescMeasuredValueEvent, SVPCanType::SVPDescMeasuredIdType)         // 12
    RCF_METHOD_V2(void, OnBapScreenViewOptionEvent, SVPCanType::SVPBapScreenViewIdType,
                  std::vector<unsigned char>&)                                               // 13
    RCF_METHOD_V2(void, OnBapScreenViewDataEvent, SVPCanType::SVPBapScreenViewIdType,
                  std::vector<unsigned char>&)                                               // 14
    RCF_METHOD_V2(void, OnBapScreenViewRequestEvent, SVPCanType::SVPBapScreenViewIdType,
                  SVPCanType::SVPBapScreenCtrlType)                                          // 15
    RCF_METHOD_V2(void, OnBapPhoneCallActionChange, SVPCanType::SVPBapPhoneCallActionType,
                  unsigned char)                                                             // 16
    RCF_METHOD_V1(void, OnBapStatusBarChanged, std::vector<unsigned char>&)                  // 17
    RCF_METHOD_V2(void, OnBapFsgStateChanged, SVPCanType::SVPBapFsgStateType, int)           // 18
    RCF_METHOD_V1(void, OnBapGeneralPopupChanged, SVPCanType::SVPBapGeneralPopupType)        // 19
    RCF_METHOD_V2(void, OnBapStartStopValueChange, SVPCanType::SVPStartStopValueIdType,
                  std::vector<unsigned char>&)                                               // 20
    RCF_METHOD_V2(void, OnBapScreenOffTimerEvent, SVPCanType::SVPBapScreenOffFuncType,
                  SVPCanType::SVPBapScreenOffTimerType)                                      // 21
    RCF_METHOD_V2(void, OnCanServiceMessage, unsigned short, std::vector<unsigned char>&)    // 22
RCF_END(SVP_CanListener)

const int kCanServicePort           = 30705;   // RcfClient<SVP_CanService>
const int kCanListenerPublisherPort = 30706;   // SubscriptionParms::setPublisherEndpoint

#endif // AA_CARLIFE_SHIM_CAN_IDL_HPP
