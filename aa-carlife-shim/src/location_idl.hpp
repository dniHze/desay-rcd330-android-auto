// SPDX-License-Identifier: GPL-2.0-only
//
// The SVP location service (svp/bin/locationservice, registry svp.svc.device.location): GNSS
// fixes from the ST GPS HAL (svp/lib/hw/libhal-gps-st.so, NMEA from /dev/ttyS4). Recovered
// from the stock client svp/lib/liblocation.so.1.0.0 (SVPLocationService_Client) and checked
// against locationservice (SVPLocationService_Server). Both are built against RCF 2.0
// (NEEDED libRCF.so.2.0.0.2685).
//
// CAVEAT, read first: on this firmware dump nothing starts locationservice (svp/etc/init.sh
// does not list it; only svp/etc/test.sh does, and that script also starts services this image
// does not have: discservice, wifiservice, ttsservice, asrservice), and libRCF.so.2.0.0.2685 is
// not in the dump at all. So the service may not be running on the unit. Check on the device
// (pidof locationservice; ports 30105/30106 listening) before relying on it.
//
//   * service "SVP_LocationService" (SVP_LocationService::getInterfaceName, liblocation
//     0x35470) on TCP 30105: RcfClient<SVP_LocationService>(TcpEndpoint("0.0.0.0", 30105))
//     (SVPLocationService_Client ctor 0x30fc8); locationservice: RcfServer(TcpEndpoint("0.0.0.0",
//     30105)) (ctor 0x30a7c), bind<SVP_LocationService>(impl, "") (InitInstance 0x31454).
//     The client's InitInstance only does SVP_WaitService("svp.svc.device.location", 5000).
//     It has two methods, neither a getter: there is no way to ask for the current fix.
//   * notifications "SVP_LocationNotify" (getInterfaceName 0x356bc) are RCF publish/subscribe:
//     locationservice runs RcfServer(TcpEndpoint("127.0.0.1", 30106)) (ctor 0x30af0) and
//     RcfServer::createPublisher<SVP_LocationNotify>() (InitInstance 0x31494: default topic =
//     the interface name). The stock subscriber (SVPLocationService_Client::InitListener 0x31208):
//         RcfServer sub(TcpEndpoint(-1));          // ctor 0x31034
//         sub.start();
//         SubscriptionParms p;
//         p.setPublisherEndpoint(TcpEndpoint("127.0.0.1", 30106));    // 0x3129c
//         sub.createSubscription<SVP_LocationNotify>(servant, p);     // 0x312d8
//     The servant must implement all six SVP_LocationNotify methods. locationservice publishes
//     OnGpsLocationUpdate for every HAL measurement (SVPLocationService_Server::
//     OnGpsLocationUpdate 0x310a8: publish().OnGpsLocationUpdate(loc), no throttling); the HAL
//     produces one per NMEA epoch (run_gps_thread 0x2880; the rate is the receiver's, presumably
//     1 Hz).
//   * method IDs: RCF 2.0 stubs have no FutureImplBase; each RcfClient<I>::M(CallOptions&, ...)
//     passes its fnId to ClientStub::init(interfaceName, fnId, semantics) (e.g. RestartGPS
//     0x43f68 -> 1, Feedback 0x4411c -> 0 in liblocation; the notify stubs 0x466f4..0x46f88 in
//     locationservice). Types: the ClientParameters<> typeinfo of each binary, return type also
//     from the Cm_Ret<int> the service stubs build. Cross-checked against the call-descriptor
//     name each StubAccess::invoke<..., int_<N>> loads (locationservice for the service,
//     liblocation for the notify interface).
//
// RCF 2.0 vs our RCF 2.1 (inferred from RCF 2.1 source, not tested): the RCF 2.0 library is not
// in the dump, so its runtime version cannot be read; RCF 2.1's Version.hpp dates runtime
// version 12 to 2013-09 and 11 to 2012-09, so a 2.0 peer is <= 11. RCF 2.1 handles that: the
// client stub's auto-versioning retries at the server's version, SubscriptionService::
// createSubscription pings first and uses doRequestSubscription_Legacy for runtime <= 11
// (src/RCF/SubscriptionService.cpp:343), and our RcfServer accepts the publisher's older
// requests (build.sh keeps LEGACY on). SF field encoding is the same code path in 2.0 and 2.1
// (operator& -> invokePtrSerializer -> Serializer<T>), as the stock serializers below show.
// Reference parameters (all notify methods take T&) are marshalled per runtime version, so the
// T& in the IDL must stay a reference (stock_ids.txt checks it).
//
// SVPTypeGpsLocation, wire order from SF::Serializer<SVPTypeGpsLocation>::serializeContents
// (liblocation 0x36a30, locationservice 0x36a10, identical). It is NOT plain field order:
// after the float at +52 it writes the float at +72, then +60, +64, +68, then +72 again; the
// float at +56 is never sent (serialize() below reproduces this exactly). Meanings: test.location
// prints +24/+32 as "lat"/"lon" (0xc374). locationservice copies the HAL's
// HW_GPS_MEASUREMENT_DATA field for field (hw_gps_measure_cb 0x5c6dc), and the HAL builds that
// record at get_pos_data(out, msg) out+72 (run_gps_thread 0x29f8 passes out+72 to the measure
// callback, slot 1 of the callback table at locationservice 0x707bc). The parse_nmea message
// layout (0x3c18): $GPRMC at +0, $GPGSA +72, $GPGGA +200, $GPGSV +216, $PSTMDRGPS +872,
// $PSTMDRDEBUG +936, $PSTMDRCOV +976 (tags from is_* compares). Field derivations (get_pos_data
// 0x2b00 unless noted) are listed on the members. Angles are RADIANS: get_pos_data multiplies
// degrees by a float-precision pi (0x400921fb60000000 = 3.1415927410125732) and divides by 180.
// SVPTypeGpsTime: SF serializeContents (liblocation 0x36920), filled by get_pos_data at out+160
// from $GPRMC (UTC) and copied 1:1 by hw_gps_date_time_cb (0x5c8ac).
// The other types are listed for completeness (wire order from their serializeContents,
// identical in both binaries); their field meanings are not needed and mostly not recovered.
#ifndef AA_CARLIFE_SHIM_LOCATION_IDL_HPP
#define AA_CARLIFE_SHIM_LOCATION_IDL_HPP

#include <RCF/Idl.hpp>

// The pi the HAL uses for degrees -> radians; divide by this, not M_PI, to get its degrees back.
const double kLocationHalPi = 3.1415927410125732;

struct SVPTypeGpsLocation {
    // From $GPGSA field 2 (fix type) of the GPS GSA: '2' -> 2, '3' -> 3, anything else -> 0.
    // The HAL never produces 1.
    enum GpsTrackingStatus {
        GPS_TRACK_NO_FIX  = 0,
        GPS_TRACK_FIX_2D  = 2,
        GPS_TRACK_FIX_3D  = 3,
        GPS_TRACK_MAX_    = 0x7fffffff
    };

    GpsTrackingStatus trackingStatus = GPS_TRACK_NO_FIX;  // +0
    unsigned char satsWithSignal  = 0;  // +4  $GPGSV satellites with SNR > 0.1 (GPS + 2nd system)
    unsigned char satsWithSignal2 = 0;  // +5  same count
    unsigned char satsUsed        = 0;  // +6  $GPGGA field 7 (satellites used)
    unsigned char reserved7       = 0;  // +7  always 0
    unsigned char reserved8       = 0;  // +8  always 0
    float  drGps5      = 0;  // +12 $PSTMDRGPS field 5 (meaning unknown)
    float  drGps7      = 0;  // +16 $PSTMDRGPS field 7 (meaning unknown)
    float  drGps6      = 0;  // +20 $PSTMDRGPS field 6 (meaning unknown)
    double latitude    = 0;  // +24 rad = $PSTMDRGPS field 1 (deg) * pi/180
    double longitude   = 0;  // +32 rad = $PSTMDRGPS field 2 (deg) * pi/180
    float  hAccuracy   = 0;  // +40 hypot($PSTMDRCOV f1/10, f2/10); a position error estimate
                             //     (inferred; unit presumably m)
    float  altitude    = 0;  // +44 $GPGGA field 9, metres above MSL
    float  zero48      = 0;  // +48 always 0
    float  speed       = 0;  // +52 hypot($PSTMDRGPS f3, f4): horizontal speed (unit inferred m/s)
    float  drDebug4    = 0;  // +56 $PSTMDRDEBUG field 4 -- never on the wire, stays 0 here
    float  zero60      = 0;  // +60 always 0
    float  zero64      = 0;  // +64 always 0
    float  heading     = 0;  // +68 rad, clockwise from north: (f3 >= 0 ? 5pi/2 : pi/2) -
                             //     atan2(f3, f4) of $PSTMDRGPS, i.e. f3 = v north, f4 = v east.
                             //     Range [pi/2, 5pi/2]: reduce mod 2pi.
    float  drDebug3Rad = 0;  // +72 rad = $PSTMDRDEBUG field 3 (deg) * pi/180 (sent twice)
    unsigned int tickMs    = 0;  // +76 HAL get_tick_count(): CLOCK_MONOTONIC in ms
    unsigned int validMask = 0;  // +80 always 0x7ffff from this HAL (no fix information)

    void serialize(SF::Archive& ar) {
        ar & trackingStatus & satsWithSignal & satsWithSignal2 & satsUsed & reserved7 & reserved8
           & drGps5 & drGps7 & drGps6 & latitude & longitude & hAccuracy & altitude & zero48
           & speed
           & drDebug3Rad            // stock writes +72 here, where +56 would be
           & zero60 & zero64 & heading
           & drDebug3Rad            // and +72 again in its own place
           & tickMs & validMask;
    }
};

struct SVPTypeGpsTime {
    enum GpsTimeType { GPS_TIME_TYPE_MAX_ = 0x7fffffff };  // the HAL always sets 1

    unsigned short year   = 0;  // +0  $GPRMC yy + 2000
    unsigned char  month  = 0;  // +2
    unsigned char  day    = 0;  // +3
    unsigned char  hour   = 0;  // +4  UTC
    unsigned char  minute = 0;  // +5
    unsigned char  second = 0;  // +6
    GpsTimeType    type   = GpsTimeType(0);  // +8
    unsigned int   tickMs = 0;  // +12 get_tick_count()
    unsigned char  flags  = 0;  // +16 the HAL sets 1

    void serialize(SF::Archive& ar) {
        ar & year & month & day & hour & minute & second & type & tickMs & flags;
    }
};

// 0x36be0: the enum, then 12 GpsSatelliteStatus records, each serialised as its own object
// (setFlag + Serializer<GpsSatelliteStatus>::invoke, i.e. "ar & sats[i]", not an SF array).
struct SVPTypeGpsChannel {
    enum GpsSatalliteType { GPS_SAT_TYPE_MAX_ = 0x7fffffff };  // stock spelling

    struct GpsSatelliteStatus {
        unsigned char b0  = 0;
        float         f4  = 0, f8 = 0, f12 = 0;
        unsigned char b16 = 0;
        unsigned int  u20 = 0, u24 = 0;

        void serialize(SF::Archive& ar) { ar & b0 & f4 & f8 & f12 & b16 & u20 & u24; }
    };

    GpsSatalliteType   type = GpsSatalliteType(0);
    GpsSatelliteStatus sats[12];

    void serialize(SF::Archive& ar) {
        ar & type;
        for (int i = 0; i < 12; ++i) ar & sats[i];
    }
};

// 0x370b8.
struct SVPTypeDrLocation {
    unsigned int   u0  = 0;
    double         d8  = 0, d16 = 0;
    float          f24 = 0, f28 = 0, f32 = 0, f36 = 0, f40 = 0, f44 = 0, f48 = 0, f52 = 0, f56 = 0;
    unsigned char  b60 = 0;
    unsigned short h62 = 0, h64 = 0;
    signed char    c66 = 0, c67 = 0;

    void serialize(SF::Archive& ar) {
        ar & u0 & d8 & d16 & f24 & f28 & f32 & f36 & f40 & f44 & f48 & f52 & f56 & b60 & h62
           & h64 & c66 & c67;
    }
};

// 0x3669c.
struct SVPTypeDrSensorData {
    unsigned char b0 = 0;
    float f4 = 0, f8 = 0, f12 = 0, f16 = 0, f20 = 0, f24 = 0;

    void serialize(SF::Archive& ar) { ar & b0 & f4 & f8 & f12 & f16 & f20 & f24; }
};

// 0x365d8. The HAL's gps_mmfb (0x2564) prints it as "$PSTMDRMMFB,%02d%02d%02d.00,%01d,%01d,
// %01d,%01d,%0.7f,%0.7f,%0.1f x6": hh mm ss, four flags, lat/lon in degrees, six floats.
struct SVPTypeMmFeedback {
    unsigned char hour = 0, minute = 0, second = 0;
    unsigned char flag3 = 0, flag4 = 0, flag5 = 0, flag6 = 0;
    double latDeg = 0, lonDeg = 0;
    float  f24 = 0, f28 = 0, f32 = 0, f36 = 0, f40 = 0, f44 = 0;

    void serialize(SF::Archive& ar) {
        ar & hour & minute & second & flag3 & flag4 & flag5 & flag6 & latDeg & lonDeg & f24 & f28
           & f32 & f36 & f40 & f44;
    }
};

RCF_BEGIN(SVP_LocationService, "SVP_LocationService")
    RCF_METHOD_R1(int, Feedback, SVPTypeMmFeedback&)                          // 0
    RCF_METHOD_R1(int, RestartGPS, int)                                       // 1
RCF_END(SVP_LocationService)

RCF_BEGIN(SVP_LocationNotify, "SVP_LocationNotify")
    RCF_METHOD_V1(void, OnGpsTimeUpdate, SVPTypeGpsTime&)                     // 0
    RCF_METHOD_V1(void, OnGpsLocationUpdate, SVPTypeGpsLocation&)             // 1
    RCF_METHOD_V1(void, OnGpsChannelUpdate, SVPTypeGpsChannel&)               // 2
    RCF_METHOD_V1(void, OnDrLocationUpdate, SVPTypeDrLocation&)               // 3
    RCF_METHOD_V1(void, OnDrSensorDataUpdate, SVPTypeDrSensorData&)           // 4
    RCF_METHOD_V1(void, OnGpsTTFFUpdate, unsigned short&)                     // 5
RCF_END(SVP_LocationNotify)

const int kLocationServicePort         = 30105;   // RcfClient<SVP_LocationService>
const int kLocationNotifyPublisherPort = 30106;   // SubscriptionParms::setPublisherEndpoint

#endif // AA_CARLIFE_SHIM_LOCATION_IDL_HPP
