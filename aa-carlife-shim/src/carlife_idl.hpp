// SPDX-License-Identifier: GPL-2.0-only
//
// RCF interface definitions for the stock CarLife service slot (svp.svc.carlife).
//
// Recovered from the stock binaries:
//   * method IDs: the literal fnId each RcfClient<I>::Method(CallOptions&) stub passes to
//     FutureImplBase (libcarlife.so for the service, carlifeservice for the listener);
//   * signatures: each stub's AllocateClientParameters<R, A1, ...>.
//
// RCF numbers methods in declaration order, starting at 0, and dispatches on that number.
// The ORDER below is therefore the wire contract. Do not reorder, insert or remove lines;
// the trailing "// id" comments must stay 0..17 and 0..9 in sequence.
//
// Interface name strings select the servant (service) and the default topic (listener).
// Both are the class names, as in the stock binaries.

#ifndef AA_CARLIFE_IDL_HPP
#define AA_CARLIFE_IDL_HPP

#include <string>

#include <RCF/Idl.hpp>
#include <SF/string.hpp>

namespace SVPCarLife {

// SF writes enums as a raw int32 (SF::serializeEnum, NO_BEGIN_END), so only the numeric
// values matter on the wire. Each enum carries a max sentinel so that any int32 read back
// is a valid value of the type.

// What stock carlifeservice publishes, as (status, error). Each one is taken from the call
// site that publishes it, and the ones marked "seen" were recorded on the unit (step 1, run 2:
// a phone without the CarLife app). appconnect acts on 1..5 and ignores anything else.
//   (2, 0)  ConnectionInit, before it touches USB                       seen
//   (1, 1)  CCarLifeSetup::start, when ConnectionInit fails (the AOA    seen
//           switch or interface claim failed)
//   (3, 0)  CVideoPlayer::play, on the first decoded video frame
//   (4, 0)  ConnectionShutDown, when the session ends                   seen
//   (4, 1)  CVideoCarLifeManager::ReceiveDataDetectProcess, video data stopped
//   (5, 1)  ConnectionTimeOutProcess, the connection timer expired
// ClearCarLifeStatus stores 4 without publishing it.
enum S_CL_CONNECT_STATUS {
    CL_ST_NONE           = 0,
    CL_ST_CONNECT_FAILED = 1,
    CL_ST_CONNECTING     = 2,
    CL_ST_VIDEO          = 3,
    CL_ST_DISCONNECTED   = 4,
    CL_ST_TIMEOUT        = 5,
    CL_ST_MAX_           = 0x7fffffff
};

// Stock publishes 0 with 2, 3 and a normal 4, and 1 with every failure (1, 4 and 5).
enum S_CL_CONNECT_ERROR {
    CL_ERR_NONE   = 0,
    CL_ERR_FAILED = 1,
    CL_ERR_MAX_   = 0x7fffffff
};

// appconnect SVPCarLifeDlg::TouchBegin/TouchMove/TouchEnd pass 0/2/1 (single touch only).
enum S_CL_TOUCH_EVENT {
    CL_TOUCH_DOWN = 0,
    CL_TOUCH_UP   = 1,
    CL_TOUCH_MOVE = 2,
    CL_TOUCH_MAX_ = 0x7fffffff
};

// From CCmdCarLifeManager::cmdTelStateChange* in carlifeservice.
enum S_CL_TELEPHONE_STATUS {
    CL_TEL_INCOMING  = 0,
    CL_TEL_OUTGOING  = 1,
    CL_TEL_IDLE      = 2,
    CL_TEL_INCALLING = 3,
    CL_TEL_MAX_      = 0x7fffffff
};

// Values not recovered. Never emitted by the shim; declared for the listener signatures.
enum S_CL_APP_STATUS     { CL_APP_MAX_    = 0x7fffffff };
enum S_CL_PHONE_STATUS   { CL_PHONE_MAX_  = 0x7fffffff };
enum S_CL_SCREEN_STATUS  { CL_SCREEN_MAX_ = 0x7fffffff };
enum S_CL_EXCEPTION_TYPE { CL_EXC_MAX_    = 0x7fffffff };

} // namespace SVPCarLife

RCF_BEGIN(ISVP_CarLifeService, "ISVP_CarLifeService")
    RCF_METHOD_R1(int,                             Play,                            int)                       // 0
    RCF_METHOD_R1(int,                             Pause,                           int)                       // 1
    RCF_METHOD_R1(int,                             Show,                            int)                       // 2
    RCF_METHOD_R1(int,                             StartApp,                        int)                       // 3
    RCF_METHOD_R0(int,                             CarLifeConnection)                                          // 4
    RCF_METHOD_R0(int,                             CarLifeDisConnection)                                       // 5
    RCF_METHOD_V3(void,                            SingleTouchInfo,                 unsigned int, unsigned int,
                                                                                    SVPCarLife::S_CL_TOUCH_EVENT) // 6
    RCF_METHOD_V2(void,                            RotateKeyEvent,                  unsigned int, int)         // 7
    RCF_METHOD_R0(SVPCarLife::S_CL_CONNECT_STATUS, GetCarLifeConnectStatus)                                    // 8
    RCF_METHOD_V0(void,                            GoToForeGround)                                             // 9
    RCF_METHOD_R0(int,                             NeedBlockBTPopup)                                           // 10
    RCF_METHOD_R0(int,                             NeedSwitchCarLifeAppToFG)                                   // 11
    RCF_METHOD_R1(unsigned int,                    GetModuleStatus,                 unsigned int)              // 12
    RCF_METHOD_V1(void,                            SetSourceAvailable,              int)                       // 13
    RCF_METHOD_V2(void,                            SendModuleControlCmd,            unsigned int, unsigned int) // 14
    RCF_METHOD_R0(std::string,                     GetConnectCarlifePhoneBtAddress)                            // 15
    RCF_METHOD_R0(int,                             ConnectCarlifePhoneIsiOS)                                   // 16
    RCF_METHOD_V0(void,                            ClearCarLifeStatus)                                         // 17
    // The unit's libcarlife (md5 9a283ed0) has one more than the dump's: the Bluetooth app calls
    // it during an active or dialing call (result != 0: "CARLIFE NEED CHANGE HANDFREEE MODE",
    // then it switches hands-free mode; unit bluetooth 0x6b6a4, 0x6c164). Unanswered, the app's
    // client stub threw and the app restarted ~1.3 s into each call (step 3 run 38).
    RCF_METHOD_R0(int,                             NeedChangeHandFreeMode)                                     // 18
RCF_END(ISVP_CarLifeService)

RCF_BEGIN(ISVP_CarLifeListener, "ISVP_CarLifeListener")
    RCF_METHOD_V1(void, OnAppStatusChange,                 SVPCarLife::S_CL_APP_STATUS)                         // 0
    RCF_METHOD_V2(void, OnCarLifeConnectStatusChange,      SVPCarLife::S_CL_CONNECT_STATUS,
                                                           SVPCarLife::S_CL_CONNECT_ERROR)                      // 1
    RCF_METHOD_V1(void, OnConnectingStatusPercentage,      int)                                                 // 2
    RCF_METHOD_V1(void, OnPhoneStatusChange,               SVPCarLife::S_CL_PHONE_STATUS)                       // 3
    RCF_METHOD_V1(void, OnTelephoneStatusChange,           SVPCarLife::S_CL_TELEPHONE_STATUS)                   // 4
    RCF_METHOD_V1(void, OnScreenStatusChange,              SVPCarLife::S_CL_SCREEN_STATUS)                      // 5
    RCF_METHOD_V1(void, OnConnectException,                SVPCarLife::S_CL_EXCEPTION_TYPE)                     // 6
    RCF_METHOD_V0(void, OnGoToDeskTop)                                                                          // 7
    RCF_METHOD_V2(void, OnModuleStatusChange,              unsigned int, unsigned int)                          // 8
    RCF_METHOD_V1(void, OnBtCallingAndPauseCarlifeConnect, int)                                                 // 9
RCF_END(ISVP_CarLifeListener)

// Stock endpoints (SVPCarLifeService_Server::SVPCarLifeService_Server and
// SVPCarLifeService_Client::InitListener).
static const int kCarLifeServicePort   = 30407;   // RcfServer bound to 0.0.0.0 on stock
static const int kCarLifePublisherPort = 30408;   // RcfServer bound to 127.0.0.1 on stock

#endif // AA_CARLIFE_IDL_HPP
