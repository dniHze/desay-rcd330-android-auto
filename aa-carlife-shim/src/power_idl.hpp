// SPDX-License-Identifier: GPL-2.0-only
//
// The SVP power service (svp/bin/powermgr, registry svp.svc.power): the unit's power state
// (the user switching it off and on with the ignition on, shutdown) and the backlight. The shim
// follows the power state as stock carlifeservice does, and only reads.
//
// Recovered from the unit's own binaries (step 3 capture: libpower.so.1.0.0 md5 2ea27e14,
// powermgr md5 c0974065), the same way as can_idl.hpp; they match the firmware dump:
//
//   * service "SVP_PowerService" on TCP 30201 (0.0.0.0; netstat of step 1). Method IDs: the fnId
//     each libpower RcfClient<SVP_PowerService>::M(CallOptions&, ...) stub passes to
//     FutureImplBase. GetPowerState 3 allocates <int, PowerState&, unsigned char&>. The methods
//     before it are placeholders here (0 SetPowerState, 1 SetPowerStateEx, 2 GetPowerState()).
//   * listener "SVP_PowerListener": publish/subscribe, publisher on 127.0.0.1:30202 (netstat of
//     step 1). IDs and types from powermgr's RcfClient<SVP_PowerListener> stubs: 0
//     OnScreenOffChanged <uchar>, 1 OnDisplayoffChanged <int>, 2 OnPowerStateChanged
//     <PowerState, uchar, uchar>, 3 OnBkLightLevelChanged <BkLightMode, BkLightType, uchar>. A
//     subscriber's servant must have all four (a missing one aborts a debug RCF; see bt_idl.hpp).
//   * PowerState: the order of libpower's and powermgr's name table (PS_FIRST_CONNECT_START ..
//     PS_UNDEFINED), which agrees with stock carlifeservice's own logs: its
//     SVPCarLifeService_Server::OnPowerStateChanged (dump 0x49d58) names 3 "PS_USER_ON" and 4
//     "PS_USER_OFF".
//
// What stock carlifeservice does with it (dump 0x49d58), which the shim copies:
//   * PS_USER_OFF (the unit switched off with the ignition on, or going down): unless CarLife's
//     music is already paused, pause it (CCarLifeLibManager::moduleControl(3, 0)), and stop the
//     voice module if it runs; remember that it paused it. The second argument tells whether the
//     unit can be woken (logged as "can" / "can't be waked up"); the pause is the same.
//   * PS_USER_ON: resume (moduleControl(3, 1)) only what it paused, and not when
//     GetPowerState answers (PS_USER_ON, 1), a temporary switch-on ("PP_TEMP_USER_ON, do not
//     play carlife music").
#ifndef AA_CARLIFE_SHIM_POWER_IDL_HPP
#define AA_CARLIFE_SHIM_POWER_IDL_HPP

#include <RCF/Idl.hpp>

namespace SVPPower {

enum PowerState {
    PS_FIRST_CONNECT_START    = 1,
    PS_FIRST_CONNECT_COMPLETE = 2,
    PS_USER_ON                = 3,
    PS_USER_OFF               = 4,
    PS_LOGO_ON                = 5,
    PS_LOGO_OFF               = 6,
    PS_SCREEN_LOCK            = 7,
    PS_SCREEN_UNLOCK          = 8,
    PS_SHUT_DOWN              = 9,
    PS_RESET                  = 10,
    PS_UNDEFINED              = 11,
    PS_MAX_                   = 0x7fffffff
};

inline const char* name(int s) {
    static const char* const kNames[] = {"?", "FIRST_CONNECT_START", "FIRST_CONNECT_COMPLETE", "USER_ON",
                                         "USER_OFF", "LOGO_ON", "LOGO_OFF", "SCREEN_LOCK", "SCREEN_UNLOCK",
                                         "SHUT_DOWN", "RESET", "UNDEFINED"};
    return s >= 0 && s <= PS_UNDEFINED ? kNames[s] : "?";
}

// Backlight types (SVPPower::BkLight::, as the stubs name them) are int32 on the wire; the shim
// never looks at them.
namespace BkLight {
enum BkLightMode { BKLIGHT_MODE_MAX_ = 0x7fffffff };
enum BkLightType { BKLIGHT_TYPE_MAX_ = 0x7fffffff };
} // namespace BkLight

} // namespace SVPPower

RCF_BEGIN(SVP_PowerService, "SVP_PowerService")
    RCF_METHOD_PLACEHOLDER()                                                  // 0 SetPowerState
    RCF_METHOD_PLACEHOLDER()                                                  // 1 SetPowerStateEx
    RCF_METHOD_PLACEHOLDER()                                                  // 2 GetPowerState()
    RCF_METHOD_R2(int, GetPowerState, SVPPower::PowerState&, unsigned char&)  // 3
RCF_END(SVP_PowerService)

RCF_BEGIN(SVP_PowerListener, "SVP_PowerListener")
    RCF_METHOD_V1(void, OnScreenOffChanged, unsigned char)                    // 0
    RCF_METHOD_V1(void, OnDisplayoffChanged, int)                             // 1
    RCF_METHOD_V3(void, OnPowerStateChanged, SVPPower::PowerState, unsigned char, unsigned char) // 2
    RCF_METHOD_V3(void, OnBkLightLevelChanged, SVPPower::BkLight::BkLightMode,
                  SVPPower::BkLight::BkLightType, unsigned char)              // 3
RCF_END(SVP_PowerListener)

const int kPowerServicePort = 30201;
const int kPowerListenerPort = 30202;

#endif // AA_CARLIFE_SHIM_POWER_IDL_HPP
