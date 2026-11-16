// Generated from aa-carlife-shim/src/power_idl.hpp by gen_tables.py (method IDs and signatures, in
// declaration order: RCF numbers methods from 0, a placeholder keeps a position).
#![allow(dead_code)]

use super::Method;

pub const POWER_SERVICE: &str = "SVP_PowerService";
pub const POWER_SERVICE_GET_POWER_STATE: i32 = 3;
pub const POWER_SERVICE_METHODS: &[Method] = &[
    Method { id: POWER_SERVICE_GET_POWER_STATE, name: "GetPowerState", sig: "sig<int, SVPPower::PowerState&, unsigned char&>" },
];

pub const POWER_LISTENER: &str = "SVP_PowerListener";
pub const POWER_LISTENER_ON_SCREEN_OFF_CHANGED: i32 = 0;
pub const POWER_LISTENER_ON_DISPLAYOFF_CHANGED: i32 = 1;
pub const POWER_LISTENER_ON_POWER_STATE_CHANGED: i32 = 2;
pub const POWER_LISTENER_ON_BK_LIGHT_LEVEL_CHANGED: i32 = 3;
pub const POWER_LISTENER_METHODS: &[Method] = &[
    Method { id: POWER_LISTENER_ON_SCREEN_OFF_CHANGED, name: "OnScreenOffChanged", sig: "sig<RCF::Void, unsigned char>" },
    Method { id: POWER_LISTENER_ON_DISPLAYOFF_CHANGED, name: "OnDisplayoffChanged", sig: "sig<RCF::Void, int>" },
    Method { id: POWER_LISTENER_ON_POWER_STATE_CHANGED, name: "OnPowerStateChanged", sig: "sig<RCF::Void, SVPPower::PowerState, unsigned char, unsigned char>" },
    Method { id: POWER_LISTENER_ON_BK_LIGHT_LEVEL_CHANGED, name: "OnBkLightLevelChanged", sig: "sig<RCF::Void, SVPPower::BkLight::BkLightMode, SVPPower::BkLight::BkLightType, unsigned char>" },
];
