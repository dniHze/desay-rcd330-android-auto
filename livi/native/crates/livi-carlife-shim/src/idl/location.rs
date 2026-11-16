// Generated from aa-carlife-shim/src/location_idl.hpp by gen_tables.py (method IDs and signatures, in
// declaration order: RCF numbers methods from 0, a placeholder keeps a position).
#![allow(dead_code)]

use super::Method;

pub const LOCATION_SERVICE: &str = "SVP_LocationService";
pub const LOCATION_SERVICE_FEEDBACK: i32 = 0;
pub const LOCATION_SERVICE_RESTART_GPS: i32 = 1;
pub const LOCATION_SERVICE_METHODS: &[Method] = &[
    Method { id: LOCATION_SERVICE_FEEDBACK, name: "Feedback", sig: "sig<int, SVPTypeMmFeedback&>" },
    Method { id: LOCATION_SERVICE_RESTART_GPS, name: "RestartGPS", sig: "sig<int, int>" },
];

pub const LOCATION_NOTIFY: &str = "SVP_LocationNotify";
pub const LOCATION_NOTIFY_ON_GPS_TIME_UPDATE: i32 = 0;
pub const LOCATION_NOTIFY_ON_GPS_LOCATION_UPDATE: i32 = 1;
pub const LOCATION_NOTIFY_ON_GPS_CHANNEL_UPDATE: i32 = 2;
pub const LOCATION_NOTIFY_ON_DR_LOCATION_UPDATE: i32 = 3;
pub const LOCATION_NOTIFY_ON_DR_SENSOR_DATA_UPDATE: i32 = 4;
pub const LOCATION_NOTIFY_ON_GPS_TTFF_UPDATE: i32 = 5;
pub const LOCATION_NOTIFY_METHODS: &[Method] = &[
    Method { id: LOCATION_NOTIFY_ON_GPS_TIME_UPDATE, name: "OnGpsTimeUpdate", sig: "sig<RCF::Void, SVPTypeGpsTime&>" },
    Method { id: LOCATION_NOTIFY_ON_GPS_LOCATION_UPDATE, name: "OnGpsLocationUpdate", sig: "sig<RCF::Void, SVPTypeGpsLocation&>" },
    Method { id: LOCATION_NOTIFY_ON_GPS_CHANNEL_UPDATE, name: "OnGpsChannelUpdate", sig: "sig<RCF::Void, SVPTypeGpsChannel&>" },
    Method { id: LOCATION_NOTIFY_ON_DR_LOCATION_UPDATE, name: "OnDrLocationUpdate", sig: "sig<RCF::Void, SVPTypeDrLocation&>" },
    Method { id: LOCATION_NOTIFY_ON_DR_SENSOR_DATA_UPDATE, name: "OnDrSensorDataUpdate", sig: "sig<RCF::Void, SVPTypeDrSensorData&>" },
    Method { id: LOCATION_NOTIFY_ON_GPS_TTFF_UPDATE, name: "OnGpsTTFFUpdate", sig: "sig<RCF::Void, unsigned short&>" },
];
