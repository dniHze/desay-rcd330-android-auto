// SPDX-License-Identifier: GPL-3.0-or-later
//
// aa-carlife-shim: stand-in for /svp/bin/carlifeservice; the Rust port of
// aa-carlife-shim/src/shim.cpp, run as `aa-headless shim <args>`.
//
// It serves the stock CarLife RCF contract (idl/carlife.rs) so that the unchanged
// appconnect / connectionservice front end drives Android Auto instead of Baidu CarLife:
//
//   * an RCF server on <bind>:30407 with ISVP_CarLifeService bound (stock binds 0.0.0.0);
//   * an RCF server on 127.0.0.1:30408 publishing ISVP_CarLifeListener (clients subscribe);
//   * /tmp/run/service/registry/svp.svc.carlife, the empty file SVP_RegisterService creates
//     and the clients' StartService() waits for.
//
// The shim never touches USB. It tells the AA receiver what to do over a line-based Unix
// socket (AA_CTL_SOCK) and turns the receiver's state lines into connect-status callbacks:
//
//   shim -> receiver   "start" | "stop"
//                      "touch <down|up|move> <x> <y>"   (appconnect screen coordinates)
//                      "rotary <code> <value>"          (raw RotateKeyEvent, mapping TBD)
//                      "key <keyfeed token>"            (play / pause, see keyfeed.rs)
//                      "show <n>" | "foreground"
//   receiver -> shim   "state <connecting|connected|projecting|failed|disconnected>"
//                      "audio <media|speech|system|telephony> <0|1>"   phone audio on / off
//                      "mic <0|1>"                                     phone wants the mic / done
//                      "hostui"                  the phone's "back to the car" icon: OnGoToDeskTop
//   both ways          "hello <protocol>"        first line on a connection (protocol 2 now)
//   liveness           shim "ping <n>" every 2 s, receiver "pong <n>", only once the receiver
//                      said hello 2 or later; 3 unanswered: the receiver is killed (procguard
//                      restarts it) and the connection dropped. An old receiver is never pinged.
//   Lines either side does not know are ignored, so old and new builds can be mixed.
//
// Connect statuses follow stock (see idl/mod.rs): CarLifeConnection() publishes connecting
// (2, 0) at once; the receiver's first video frame gives (3, 0); a failed start gives (1, 1),
// no video in time gives the timeout (5, 1), and the end of a session (4, 0). With
// AA_SHIM_FAKE=1 (or --fake) no receiver is needed: CarLifeConnection() plays connecting ->
// video, to check how appconnect reacts on the unit.
//
// procguard launches services as "procguard NAME --debug-output none" and restarts them after
// an exit 0 or a signal, every 50 ms; any other exit leaves them down for the boot. So:
//   * --debug-output console logs to stderr; any other mode (none, the unit's default) logs to
//     syslog, through a socket that never blocks (log.rs): under procguard stderr is the serial
//     console;
//   * a start-up failure writes its reason to AA_SHIM_FATAL_FILE and exits 0: the launcher then
//     runs the stock service in the slot (fatal_start). Exit 1 only when another server already
//     holds our ports, which then serves the slot;
//   * a crash logs a backtrace and dies by its signal (crash.rs), so procguard restarts it;
//   * SIGTERM ends a running session first (CarLifeShim::end_session), then exits 0.
// unknown arguments only warn. "--log-relay TAG" is a separate small mode: it copies stdin to
// syslog line by line (relay.rs), for the processes the launcher starts.
//
// Timed waits use std's Condvar, which waits on CLOCK_MONOTONIC on Linux: the unit's wall clock
// steps by hours early in every boot, which the C++ shim's MonoCv and its patched RCF had to
// work around; rcf-lite's timeouts are socket timeouts, also monotonic.

pub mod appc;
mod audio;
mod bridge;
mod btinfo;
mod btpairing;
mod carinfo;
mod carlife;
mod config;
mod control;
mod crash;
pub mod idl;
mod ilm;
mod lastmode;

pub mod log;
mod mic;
mod power;
mod relay;
mod scan;
mod video;

use std::io::Write;
use std::net::TcpListener;
use std::os::unix::fs::OpenOptionsExt;
use std::sync::{Arc, Weak};
use std::time::Duration;

use rcf_lite::{Publisher, Server, ServerHandle};

use carlife::{CarLifeService, CarLifeShim};
use config::{dir_name, env_int, env_or, errstr, mkdirs, tcp_reachable, Config, MAX_PORT};
use control::ControlChannel;
use crash::VERSION;

const SERVICE_NAME: &str = "svp.svc.carlife";

// SVP_RegisterService (libservice.so) only creates this empty file. Clients watch the
// directory for IN_CREATE | IN_DELETE (SVPServiceMonitor, mask 0x300); connectionservice makes
// its client lazily on a call, while appconnect subscribes to our events once, when it starts
// (step 3 run 46). A file left behind by a killed carlifeservice would make our O_CREAT a
// silent no-op, so delete it first: clients then see "down", then "up".
fn register_service(dir: &str, path_out: &mut String) -> Result<(), std::io::Error> {
    if dir == "-" {
        return Ok(());
    }
    if let Err(e) = mkdirs(dir, 0o777) {
        loge!("cannot create {}: {}", dir, errstr(&e));
        return Err(e);
    }
    *path_out = format!("{dir}/{SERVICE_NAME}");
    if std::fs::remove_file(&*path_out).is_ok() {
        logi!("removed a stale {}", path_out);
        std::thread::sleep(Duration::from_millis(200));
    }
    // 0700, as SVP_RegisterService leaves the other services' files.
    match std::fs::OpenOptions::new().write(true).create_new(true).mode(0o700).open(&*path_out) {
        Ok(_) => {
            logi!("registered {}", path_out);
            Ok(())
        }
        Err(e) => {
            loge!("cannot create {}: {}", path_out, errstr(&e));
            path_out.clear();
            Err(e)
        }
    }
}

// Port 30407/30408 can still be held by a previous instance that procguard is replacing:
// up to 10 tries, 500 ms apart.
fn start_server(host: &str, port: i32, what: &str, server: Arc<Server>) -> Option<ServerHandle> {
    let mut attempt = 1;
    loop {
        let r = TcpListener::bind((host, port as u16)).and_then(|l| server.clone().start(l));
        match r {
            Ok(h) => return Some(h),
            Err(e) => {
                if attempt >= 10 {
                    loge!("{} {}:{}: start failed: {}", what, host, port, errstr(&e));
                    return None;
                }
                logw!("{} {}:{}: start failed ({}), retrying", what, host, port, errstr(&e));
            }
        }
        std::thread::sleep(Duration::from_millis(500));
        attempt += 1;
    }
}

// The shim cannot serve the slot. Under procguard a non-zero exit leaves the slot empty for the
// whole boot, and appconnect then waits on a service that never answers (step 3 runs 34-36: the
// unit lagged); so the reason goes to cfg.fatal_file, the registry file goes (it is ours, or a
// crashed instance's), and the exit is 0: procguard runs the launcher again, which finds the
// file and starts the stock service in our place. When another live server holds our port,
// that one serves the slot: exit 1, stay down, and leave its registry file alone.
fn fatal_start(cfg: &Config, reason: &str, busy: Option<(&str, i32)>) -> i32 {
    if let Some((host, port)) = busy
        && port > 0 && tcp_reachable(if host == "0.0.0.0" { "127.0.0.1" } else { host }, port, 500) {
            loge!(
                "start-up failed: {}; another server holds {}:{} and serves the slot, staying down",
                reason,
                host,
                port
            );
            return 1;
        }
    if cfg.registry_dir != "-" {
        let _ = std::fs::remove_file(format!("{}/{}", cfg.registry_dir, SERVICE_NAME));
    }
    let dir = dir_name(&cfg.fatal_file);
    if !dir.is_empty() {
        let _ = mkdirs(&dir, 0o755);
    }
    let tmp = format!("{}.tmp", cfg.fatal_file);
    let text = format!("{reason}\n");
    let r = std::fs::OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .mode(0o644)
        .open(&tmp)
        .and_then(|mut f| {
            let n = f.write(text.as_bytes())?;
            if n != text.len() {
                return Err(std::io::Error::last_os_error());
            }
            Ok(())
        })
        .and_then(|_| std::fs::rename(&tmp, &cfg.fatal_file));
    match r {
        Ok(()) => loge!(
            "start-up failed: {}; written to {} for the launcher (the stock service takes the slot)",
            reason,
            cfg.fatal_file
        ),
        Err(e) => loge!("start-up failed: {}; and writing {} failed: {}", reason, cfg.fatal_file, errstr(&e)),
    }
    0
}

fn usage() {
    let _ = std::io::stderr().write_all(usage_text().as_bytes());
}

/// The usage text, as the C++ shim's.
pub fn usage_text() -> &'static str {
    "usage: aa-carlife-shim [--fake] [--verbose] [--debug-output <mode>]\n\
         \x20      aa-carlife-shim --log-relay TAG    stdin to syslog, ident TAG, until the end of input\n\
         --debug-output console logs to stderr; any other mode (none, syslog, kmsg, file) to syslog\n\
         environment:\n\
         \x20 AA_CL_BIND=127.0.0.1           service bind address (stock uses 0.0.0.0)\n\
         \x20 AA_CL_SVC_PORT=30407           ISVP_CarLifeService port\n\
         \x20 AA_CL_PUB_PORT=30408           ISVP_CarLifeListener publisher port\n\
         \x20 AA_CL_REGISTRY_DIR=/tmp/run/service/registry   '-' to skip registration\n\
         \x20 AA_CTL_SOCK=/tmp/run/aa-carlife.sock           '-' to disable the receiver link\n\
         \x20 AA_SHIM_FATAL_FILE=/tmp/run/aa/shim.fatal      a start-up failure's reason, for the launcher\n\
         \x20 AA_RECEIVER_COMM=aa-headless,aa-receiver       receiver process names a hung receiver is\n\
         \x20                                killed by (SIGKILL after 3 unanswered pings)\n\
         \x20 AA_SHIM_FAKE=1                 play connecting (2) then video (3), no receiver\n\
         \x20 AA_SHIM_FAKE_VIDEO_MS=1500     fake mode: delay from connecting to video\n\
         \x20 AA_SHIM_CONNECT_TIMEOUT_MS=30000  wait for 'state projecting' after 'start'\n\
         \x20 AA_SHIM_RECEIVER_WAIT_MS=5000     wait for the receiver to connect\n\
         \x20 AA_SHIM_VERBOSE=1              log touches, keys, getters and the like (debug level)\n\
         \x20 AA_SHIM_SYSLOG_SOCK=/dev/log   the syslog socket (tests)\n\
         \x20 AA_ILM_HOST=127.0.0.1 AA_ILM_PORT=30005   SVP iLM service for Show(n); port 0 disables\n\
         \x20 AA_BRIDGE_COMM=androidautobrid,aa-bridge  video bridge process names (its ILM surface\n\
         \x20                                is 20000000 + its pid)\n\
         \x20 AA_VIDEO_SOURCE=32,24,800,480  part of the bridge's surface to show ('-': leave it)\n\
         \x20 AA_VIDEO_DEST=0,0,800,480      where on the screen ('-': leave it)\n\
         \x20 AA_BT_HOST=127.0.0.1 AA_BT_PORT=30315  SVP Bluetooth service: the car mic's route\n\
         \x20                                while the phone has the mic, and the unit's Bluetooth address\n\
         \x20                                for the receiver (receiver mode only); port 0 disables\n\
         \x20 AA_BT_PAIRING=1                Android Auto's Bluetooth pairing loop with the car\n\
         \x20                                (receiver btpair / btpaired / btauth); 0: every phone is\n\
         \x20                                answered \"already paired\"\n\
         \x20 AA_BT_LISTEN_PORT=30316        btservice's event publisher (pairing loop, music link)\n\
         \x20 AA_BT_A2DP_DROP=1              Bluetooth music (A2DP) off during the session, back after\n\
         \x20 AA_CALL_FIX=1 AA_CALL_FIX_MS=400  keep an outgoing call on Android Auto's screen\n\
         \x20 AA_BT_LOCK=1                   while Android Auto's phone holds the car's phone link, no\n\
         \x20                                other device may pair or connect (capability 10)\n\
         \x20 AA_BT_LOCK_FILE=/tmp/run/aa/btlock  marks that lock as ours: a restart takes it off\n\
         \x20 AA_ONTOP_FILE=/tmp/run/aa/ontop  \"<pid> <0|1>\": Android Auto on screen; the Bluetooth\n\
         \x20                                app shows no pop-ups while it is 1; empty disables\n\
         \x20 AA_APPC_PATCH=1                appconnect's code changes in memory: outgoing calls stay on\n\
         \x20                                Android Auto; an iPhone goes straight to CarPlay\n\
         \x20 AA_CAN_HOST=127.0.0.1 AA_CAN_PORT=30705  SVP CAN service: the car's day/night, gear and\n\
         \x20                                driver's side for Android Auto (receiver mode only); port 0\n\
         \x20                                disables all CAN data\n\
         \x20 AA_CAN_LISTEN_PORT=30706       the CAN service's publisher: the vehicle speed; 0 disables\n\
         \x20 AA_AUDIO_HOST=127.0.0.1 AA_AUDIO_PORT=30203  audiomgr: RegisterAudio for the bridge's\n\
         \x20                                prompt streams (receiver mode only); port 0 disables\n\
         \x20 AA_SHIM_NOTIFY=/tmp/run/aa/notify.sock  datagram socket the bridge reports opened streams to\n\
         \x20 AA_SOURCE_HOST=127.0.0.1 AA_SOURCE_PORT=30205  SVP source service for the CarLife\n\
         \x20                                audio sources (receiver mode only); port 0 disables\n\
         \x20 AA_SOURCE_LISTEN_PORT=30206    its status topic: when an interruption ends; 0 disables\n\
         \x20 AA_POWER_HOST=127.0.0.1 AA_POWER_PORT=30201  SVP power service: is a switch-on temporary\n\
         \x20 AA_POWER_LISTEN_PORT=30202     its publisher: the unit switched off / on (pause and resume\n\
         \x20                                AA's media as stock CarLife does); 0 disables\n\
         \x20 AA_LAST_MODE_FILE=             last mode: whether AA had the speakers at the last\n\
         \x20                                switch-off; if not, the phone's autoplay is refused once\n\
         \x20                                at the next connection; empty disables\n\
         \x20 AA_BOOTS_FILE=                 the launcher's boot-loop counter, set to 0 at a switch-off\n"
}

/// The shim: `args` as its own argv (args[0] the program).
pub fn main(args: Vec<String>) -> i32 {
    let argc = args.len();
    // The log relay is a mode of its own: nothing below runs for it.
    for i in 1..argc {
        if args[i] == "--log-relay" {
            if i + 1 >= argc || args[i + 1].is_empty() {
                usage();
                return 2;
            }
            log::set_path(&env_or("AA_SHIM_SYSLOG_SOCK", "/dev/log"));
            return relay::run(&args[i + 1]);
        }
    }
    // Where the log goes, before anything is logged: stderr only for --debug-output console
    // (and when started by hand without --debug-output); the unit's procguard passes "none".
    {
        let mut mode: Option<&str> = None;
        for i in 1..argc.saturating_sub(1) {
            if args[i] == "--debug-output" {
                mode = Some(&args[i + 1]);
            }
        }
        log::set_stderr(mode.is_none_or(|m| m == "console"));
        log::set_path(&env_or("AA_SHIM_SYSLOG_SOCK", "/dev/log"));
    }
    log::set_verbose(env_int("AA_SHIM_VERBOSE", 0, 0, 1) != 0);
    crash::install();

    let mut cfg = Config {
        bind_host: env_or("AA_CL_BIND", "127.0.0.1"),
        svc_port: env_int("AA_CL_SVC_PORT", idl::CARLIFE_SERVICE_PORT, 1, MAX_PORT),
        pub_port: env_int("AA_CL_PUB_PORT", idl::CARLIFE_PUBLISHER_PORT, 1, MAX_PORT),
        registry_dir: env_or("AA_CL_REGISTRY_DIR", "/tmp/run/service/registry"),
        ctl_sock: env_or("AA_CTL_SOCK", "/tmp/run/aa-carlife.sock"),
        fake: env_int("AA_SHIM_FAKE", 0, 0, 1) != 0,
        fake_video_ms: env_int("AA_SHIM_FAKE_VIDEO_MS", 1500, 0, 60000),
        // 30 s: what every car run used (start.sh); a first session that dies after the phone's
        // accessory switch and the second one that works take about 4 s together (step 3 run 47).
        connect_timeout_ms: env_int("AA_SHIM_CONNECT_TIMEOUT_MS", 30000, 1000, 120000),
        receiver_wait_ms: env_int("AA_SHIM_RECEIVER_WAIT_MS", 5000, 100, 60000),
        ilm_host: env_or("AA_ILM_HOST", "127.0.0.1"),
        ilm_port: env_int("AA_ILM_PORT", idl::ILM_SERVICE_PORT, 0, MAX_PORT),
        // /proc/<pid>/comm keeps 15 characters: "androidautobridge" reads "androidautobrid".
        bridge_comm: env_or("AA_BRIDGE_COMM", "androidautobrid,aa-bridge"),
        // Measured on the unit with waylandsink2 (896x576 surface, picture at 32,24).
        video_source: env_or("AA_VIDEO_SOURCE", "32,24,800,480"),
        video_dest: env_or("AA_VIDEO_DEST", "0,0,800,480"),
        source_host: env_or("AA_SOURCE_HOST", "127.0.0.1"),
        source_port: env_int("AA_SOURCE_PORT", idl::SOURCE_SERVICE_PORT, 0, MAX_PORT),
        source_listen_port: env_int("AA_SOURCE_LISTEN_PORT", idl::SOURCE_LISTENER_PORT, 0, MAX_PORT),
        can_host: env_or("AA_CAN_HOST", "127.0.0.1"),
        can_port: env_int("AA_CAN_PORT", idl::CAN_SERVICE_PORT, 0, MAX_PORT),
        can_listen_port: env_int("AA_CAN_LISTEN_PORT", idl::CAN_LISTENER_PUBLISHER_PORT, 0, MAX_PORT),
        bt_host: env_or("AA_BT_HOST", "127.0.0.1"),
        bt_port: env_int("AA_BT_PORT", idl::BT_SERVICE_PORT, 0, MAX_PORT),
        bt_listen_port: env_int("AA_BT_LISTEN_PORT", idl::BT_LISTENER_PORT, 0, MAX_PORT),
        bt_pairing: env_int("AA_BT_PAIRING", 1, 0, 1) != 0,
        bt_a2dp_drop: env_int("AA_BT_A2DP_DROP", 1, 0, 1) != 0,
        bt_lock: env_int("AA_BT_LOCK", 1, 0, 1) != 0,
        bt_lock_file: env_or("AA_BT_LOCK_FILE", "/tmp/run/aa/btlock"),
        // The path is fixed in the Bluetooth app's side (i18n/lib/bluetooth.cpp); this is for tests.
        on_top_file: env_or("AA_ONTOP_FILE", "/tmp/run/aa/ontop"),
        call_fix: env_int("AA_CALL_FIX", 1, 0, 1) != 0,
        appc_patch: env_int("AA_APPC_PATCH", 1, 0, 1) != 0,
        call_fix_ms: env_int("AA_CALL_FIX_MS", 400, 0, 10000),
        audio_host: env_or("AA_AUDIO_HOST", "127.0.0.1"),
        audio_port: env_int("AA_AUDIO_PORT", idl::AUDIO_SERVICE_PORT, 0, MAX_PORT),
        notify_sock: env_or("AA_SHIM_NOTIFY", "/tmp/run/aa/notify.sock"),
        power_host: env_or("AA_POWER_HOST", "127.0.0.1"),
        power_port: env_int("AA_POWER_PORT", idl::POWER_SERVICE_PORT, 0, MAX_PORT),
        power_listen_port: env_int("AA_POWER_LISTEN_PORT", idl::POWER_LISTENER_PORT, 0, MAX_PORT),
        last_mode_file: env_or("AA_LAST_MODE_FILE", ""),
        fatal_file: env_or("AA_SHIM_FATAL_FILE", "/tmp/run/aa/shim.fatal"),
        // /proc/<pid>/comm of the receiver: its build's name, or its name on the unit (start.sh
        // and the system image run it as aa-receiver).
        receiver_comm: env_or("AA_RECEIVER_COMM", "aa-headless,aa-receiver"),
        boots_file: env_or("AA_BOOTS_FILE", ""),
    };

    let mut i = 1;
    while i < argc {
        match args[i].as_str() {
            "--debug-output" => {
                if i + 1 < argc {
                    i += 1;
                }
            }
            "--fake" => cfg.fake = true,
            "--verbose" => log::set_verbose(true),
            "--help" | "-h" => {
                usage();
                return 0;
            }
            other => logw!("ignoring unknown argument {}", other),
        }
        i += 1;
    }
    // Fake mode has no sound: leave the radio its source.
    if cfg.fake {
        cfg.appc_patch = false;
        cfg.source_port = 0;
        cfg.can_port = 0;
        cfg.can_listen_port = 0;
        cfg.bt_port = 0;
        cfg.audio_port = 0;
    }
    if cfg.can_port <= 0 {
        cfg.can_listen_port = 0;
    }
    if cfg.fake || cfg.source_port <= 0 {
        cfg.source_listen_port = 0;
    }
    if cfg.fake || cfg.bt_port <= 0 {
        cfg.bt_listen_port = 0;
    }
    if cfg.fake {
        cfg.power_port = 0;
        cfg.power_listen_port = 0;
        cfg.last_mode_file.clear();
        cfg.notify_sock = "-".to_string(); // no bridge to hear from
    }

    // Signals go to sigwait() below; every thread created after this inherits the mask.
    let sigs = unsafe {
        let mut s: libc::sigset_t = std::mem::zeroed();
        libc::sigemptyset(&mut s);
        libc::sigaddset(&mut s, libc::SIGINT);
        libc::sigaddset(&mut s, libc::SIGTERM);
        libc::pthread_sigmask(libc::SIG_BLOCK, &s, std::ptr::null_mut());
        libc::signal(libc::SIGPIPE, libc::SIG_IGN);
        s
    };

    let mode = if cfg.fake {
        format!("FAKE mode (no receiver), video after {} ms", cfg.fake_video_ms)
    } else {
        format!("receiver via {}", cfg.ctl_sock)
    };
    logi!(
        "starting: build {}, service {}:{}, publisher 127.0.0.1:{}, {}, log to {}{}",
        VERSION,
        cfg.bind_host,
        cfg.svc_port,
        cfg.pub_port,
        mode,
        if log::to_stderr() { "stderr".to_string() } else { log::path() },
        if log::verbose() { " (verbose)" } else { "" }
    );
    logi!(
        "config: connect timeout {} ms, receiver wait {} ms, ILM {}, source {}/{}, CAN {}/{}, BT {}/{} \
         (pairing {}, A2DP drop {}), audio {}, power {}/{}, call fix {} ({} ms), appconnect patch {}, \
         video {} -> {}, bridge {}, notify {}, last mode {}, fatal file {}",
        cfg.connect_timeout_ms,
        cfg.receiver_wait_ms,
        cfg.ilm_port,
        cfg.source_port,
        cfg.source_listen_port,
        cfg.can_port,
        cfg.can_listen_port,
        cfg.bt_port,
        cfg.bt_listen_port,
        cfg.bt_pairing as i32,
        cfg.bt_a2dp_drop as i32,
        cfg.audio_port,
        cfg.power_port,
        cfg.power_listen_port,
        cfg.call_fix as i32,
        cfg.call_fix_ms,
        cfg.appc_patch as i32,
        cfg.video_source,
        cfg.video_dest,
        cfg.bridge_comm,
        cfg.notify_sock,
        if cfg.last_mode_file.is_empty() { "off" } else { cfg.last_mode_file.as_str() },
        cfg.fatal_file
    );

    let cfg = Arc::new(cfg);
    let ctl = ControlChannel::new();
    let shim = CarLifeShim::new(cfg.clone(), ctl.clone());
    let rc = serve(&cfg, &ctl, &shim, &sigs);
    // As the C++ shim's locals went: the receiver link first, so no receiver line outlives the
    // shim, then the shim and its parts.
    ctl.close();
    shim.shutdown();
    rc
}

fn serve(cfg: &Config, ctl: &Arc<ControlChannel>, shim: &Arc<CarLifeShim>, sigs: &libc::sigset_t) -> i32 {
    if !cfg.fake && cfg.ctl_sock != "-" {
        let dir = dir_name(&cfg.ctl_sock);
        if !dir.is_empty()
            && let Err(e) = mkdirs(&dir, 0o755) {
                return fatal_start(cfg, &format!("cannot create {}: {}", dir, errstr(&e)), None);
            }
        let w = Arc::downgrade(shim);
        let on_line = Box::new(move |line: &str| {
            if let Some(s) = w.upgrade() {
                s.receiver_line(line);
            }
        });
        if !ctl.open(&cfg.ctl_sock, on_line, &cfg.receiver_comm) {
            return fatal_start(cfg, &format!("cannot listen on the control socket {}", cfg.ctl_sock), None);
        }
    }

    // A client that subscribes while a session runs hears it announced (subscriber_joined).
    let w: Weak<CarLifeShim> = Arc::downgrade(shim);
    let publisher = Publisher::with_on_subscribe(idl::carlife::CAR_LIFE_LISTENER, move || {
        if let Some(s) = w.upgrade() {
            s.subscriber_joined();
        }
    });
    let Some(mut pub_server) =
        start_server("127.0.0.1", cfg.pub_port, "publisher", Arc::new(Server::new().publish(publisher.clone())))
    else {
        return fatal_start(cfg, &format!("publisher port {}", cfg.pub_port), Some(("127.0.0.1", cfg.pub_port)));
    };
    shim.set_publisher(Some(publisher));

    let svc = Arc::new(Server::new().bind(Arc::new(CarLifeService(shim.clone()))));
    let Some(mut svc_server) = start_server(&cfg.bind_host, cfg.svc_port, "service", svc) else {
        let rc = fatal_start(cfg, &format!("service port {}", cfg.svc_port), Some((&cfg.bind_host, cfg.svc_port)));
        shim.set_publisher(None);
        return rc;
    };

    let mut registry_path = String::new();
    if let Err(e) = register_service(&cfg.registry_dir, &mut registry_path) {
        let rc = fatal_start(cfg, &format!("registry file in {}: {}", cfg.registry_dir, errstr(&e)), None);
        shim.set_publisher(None);
        return rc;
    }
    logi!("ready");

    let mut sig: libc::c_int = 0;
    unsafe { libc::sigwait(sigs, &mut sig) };
    logi!("signal {}, shutting down", sig);

    if !registry_path.is_empty() {
        let _ = std::fs::remove_file(&registry_path);
    }
    svc_server.stop();
    // A running session ends while the publisher can still tell appconnect (bounded).
    shim.end_session(3000);
    // The publisher refers to the publishing server: dropped before it.
    shim.set_publisher(None);
    drop(svc_server);
    pub_server.stop();
    logi!("stopped");
    0
}
