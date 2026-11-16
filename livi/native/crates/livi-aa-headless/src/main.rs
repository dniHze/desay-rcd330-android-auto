// SPDX-License-Identifier: GPL-3.0-or-later
//
// livi-aa-headless — a minimal, UI-free Android Auto host.
//
// It wires together two existing LIVI crates and nothing from the LIVI UI:
//
//   * livi-aa        — the USB watcher + AOAP accessory switch + TLS + per-phone
//                      AA *session* front-end (`usb::run`, `session::run`, the
//                      bundled public GAL head-unit cert in `cert.rs`).
//   * livi-aa-stack  — the AA *protocol driver* / node (`manager::start`, which
//                      spawns the private `driver::run` per phone and drives the
//                      real AA service handshake via the `AaMedia` seam).
//
// In full LIVI these two halves live in separate processes bridged by a small
// line-based "helper" socket (livi-helperd announces sessions; livi-core's
// manager subscribes and connects). There is no single public "run it all"
// entry point, so this binary provides that tiny helper bridge in-process (the
// only stub in here — see `Helper`) and plugs both halves together.
//
// The video/audio feed and the microphone are decoupled from any renderer: the
// AA session writes H.264/audio feed records to a unix socket (default
// /tmp/aa.video.sock) that a downstream GStreamer/IVA-HD bridge (task R3)
// listens on, and the phone's microphone is exposed on a second unix socket
// (default /tmp/aa.mic.sock) that a device-mic capture connects to.

use std::collections::HashMap;
use std::net::{IpAddr, Ipv4Addr};
use std::os::unix::fs::PermissionsExt;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

mod ctl;
mod gate;
mod keyfeed;
mod keys;

use gate::Gate;
use livi_aa::session::{self, Peer};
use livi_aa::usb::{self, Control, UsbNote};
use livi_aa_stack::bridge::NowPlaying;
use livi_aa_stack::config::{AaConfig, android_auto_dpi, fitting_tier};
use livi_aa_stack::discovery::VideoCodec;
use livi_aa_stack::helper_sock::AaHelperSock;
use livi_aa_stack::manager::{self, AaCmd, AaEvent, AaHandle, SessionId};
use livi_aa_stack::media::{AaMedia, AudioKind, AudioOutput};
use livi_core_proto::aa_events::{AaHelperEvent, AaTransport};
use livi_session_io::sock::bind_session_socket;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::{TcpStream, UnixListener, UnixStream};
use tokio::sync::{Notify, broadcast, mpsc, watch};
use tokio::task::JoinHandle;

/// On SIGTERM (or Ctrl-C), how long the sessions get to end with a ByeBye before the exit.
const SHUTDOWN_WAIT: Duration = Duration::from_millis(1500);
/// After the sessions ended, time for the USB side to reset their accessories (usb.rs serve).
const SHUTDOWN_USB_RESET: Duration = Duration::from_millis(300);
/// AA_TCP_CONNECT: the wait before connecting again (after a failure or a session's end), and
/// how often a disarmed gate is looked at.
const TCP_RETRY: Duration = Duration::from_secs(2);
const TCP_GATE_POLL: Duration = Duration::from_millis(300);

const DEFAULT_VIDEO_SOCK: &str = "/tmp/aa.video.sock";
const DEFAULT_MIC_SOCK: &str = "/tmp/aa.mic.sock";
const DEFAULT_SESSIONS_DIR: &str = "/tmp/aa-headless";
const HELPER_SOCK_NAME: &str = "aa-helper.sock";

// A sane default for the Desay 6RD 035 187B panel (~800x480). Overridable.
const DEFAULT_W: u32 = 800;
const DEFAULT_H: u32 = 480;

// Head-unit branding sent at service discovery. The Android Auto phone draws the "return to
// car" / exit-button logo from the make string (e.g. "Volkswagen" -> VW logo). Override on the
// unit with AA_HU_MAKE / AA_HU_MODEL (no rebuild) if the phone wants a different brand token.
const DEFAULT_HU_MAKE: &str = "Volkswagen";
const DEFAULT_HU_MODEL: &str = "MIB2";

// ---------------------------------------------------------------------------
// Configuration (CLI args + env, with the task's defaults)
// ---------------------------------------------------------------------------

#[derive(Clone)]
struct Config {
    video_sock: String,
    mic_sock: String,
    usb_filter: String, // phone-serial substring; empty == accept every phone
    sessions_dir: PathBuf,
    helper_sock: PathBuf,
    display_w: u32,
    display_h: u32,
    mic_device: String,
    hu_make: String,
    hu_model: String,
    hevc: bool,
    ctl_sock: String, // aa-carlife-shim control socket; empty == not gated
    knob_left: u32,
    knob_right: u32,
    dry_run: bool,
}

impl Config {
    fn from_env() -> Self {
        let sessions_dir: PathBuf = env_or("AA_SESSIONS_DIR", DEFAULT_SESSIONS_DIR).into();
        let helper_sock = std::env::var("AA_HELPER_SOCK")
            .map(PathBuf::from)
            .unwrap_or_else(|_| sessions_dir.join(HELPER_SOCK_NAME));
        let (display_w, display_h) = std::env::var("AA_DISPLAY")
            .ok()
            .and_then(|v| parse_dim(&v))
            .unwrap_or((DEFAULT_W, DEFAULT_H));
        Self {
            video_sock: env_or("AA_VIDEO_SOCK", DEFAULT_VIDEO_SOCK),
            mic_sock: env_or("AA_MIC_SOCK", DEFAULT_MIC_SOCK),
            usb_filter: env_or("AA_USB_FILTER", ""),
            sessions_dir,
            helper_sock,
            display_w,
            display_h,
            mic_device: env_or("AA_MIC_DEVICE", ""),
            hu_make: env_or("AA_HU_MAKE", DEFAULT_HU_MAKE),
            hu_model: env_or("AA_HU_MODEL", DEFAULT_HU_MODEL),
            hevc: std::env::var("AA_HEVC").is_ok_and(|v| v == "1"),
            ctl_sock: env_or("AA_CTL_SOCK", ""),
            knob_left: env_or("AA_KNOB_LEFT", "7").parse().unwrap_or(7),
            knob_right: env_or("AA_KNOB_RIGHT", "6").parse().unwrap_or(6),
            dry_run: false,
        }
    }
}

fn env_or(key: &str, default: &str) -> String {
    std::env::var(key).unwrap_or_else(|_| default.to_string())
}

fn parse_dim(s: &str) -> Option<(u32, u32)> {
    let (w, h) = s.split_once(['x', 'X', '*'])?;
    Some((w.trim().parse().ok()?, h.trim().parse().ok()?))
}

enum Parsed {
    Help,
    Invalid,
    Run(Config),
}

fn next_val<I: Iterator<Item = String>>(inline: Option<String>, args: &mut I) -> Option<String> {
    inline.or_else(|| args.next())
}

fn parse_args() -> Parsed {
    parse_args_from(std::env::args().skip(1))
}

fn parse_args_from(args: impl Iterator<Item = String>) -> Parsed {
    let mut cfg = Config::from_env();
    let mut args = args.peekable();
    while let Some(a) = args.next() {
        let (key, inline) = match a.split_once('=') {
            Some((k, v)) => (k.to_string(), Some(v.to_string())),
            None => (a.clone(), None),
        };
        macro_rules! val {
            () => {
                match next_val(inline, &mut args) {
                    Some(v) => v,
                    None => {
                        eprintln!("error: {key} needs a value");
                        return Parsed::Invalid;
                    }
                }
            };
        }
        match key.as_str() {
            "-h" | "--help" => return Parsed::Help,
            "-n" | "--dry-run" => cfg.dry_run = true,
            "--video-sock" => cfg.video_sock = val!(),
            "--mic-sock" => cfg.mic_sock = val!(),
            "--usb-filter" => cfg.usb_filter = val!(),
            "--sessions-dir" => cfg.sessions_dir = val!().into(),
            "--helper-sock" => cfg.helper_sock = val!().into(),
            "--mic-device" => cfg.mic_device = val!(),
            "--hevc" => cfg.hevc = true,
            "--ctl-sock" => cfg.ctl_sock = val!(),
            "--display" => match parse_dim(&val!()) {
                Some((w, h)) => {
                    cfg.display_w = w;
                    cfg.display_h = h;
                }
                None => {
                    eprintln!("error: --display wants WIDTHxHEIGHT, e.g. 800x480");
                    return Parsed::Invalid;
                }
            },
            // svp_start_service (svp/etc/func_startstop.sh) appends `--debug-output
            // $SVP_DEBUG_OUTPUT` to every service it launches; accept and ignore it. Also
            // without a value (an empty variable leaves none): a bad exit here would keep
            // Android Auto down for the boot (procguard gives up on a non-zero exit).
            "--debug-output" => {
                if inline.is_none() && args.peek().is_some_and(|v| !v.starts_with('-')) {
                    args.next();
                }
            }
            // Never die on a flag the platform adds: warn and carry on.
            other => eprintln!("warning: ignoring unknown argument {other}"),
        }
    }
    Parsed::Run(cfg)
}

fn print_help() {
    println!(
        "livi-aa-headless — headless Android Auto host (UI-free)

USAGE:
    livi-aa-headless [OPTIONS]

OPTIONS:
    --video-sock <PATH>     unix socket the H.264/audio feed is written to
                            [env AA_VIDEO_SOCK] [default: {DEFAULT_VIDEO_SOCK}]
    --mic-sock <PATH>       unix socket the phone microphone is exposed on
                            [env AA_MIC_SOCK] [default: {DEFAULT_MIC_SOCK}]
    --usb-filter <SERIAL>   only switch/serve phones whose USB serial contains
                            SERIAL (checked before the AOAP switch)
                            [env AA_USB_FILTER] [default: all phones]
    --sessions-dir <DIR>    dir for per-session + helper sockets
                            [env AA_SESSIONS_DIR] [default: {DEFAULT_SESSIONS_DIR}]
    --helper-sock <PATH>    in-process helper socket [env AA_HELPER_SOCK]
                            [default: <sessions-dir>/{HELPER_SOCK_NAME}]
    --display <WxH>         advertised head-unit panel size
                            [env AA_DISPLAY] [default: {DEFAULT_W}x{DEFAULT_H}]
    --mic-device <NAME>     capture device passed to the mic tap
                            [env AA_MIC_DEVICE] [default: none]
    --hevc                  advertise H.265/HEVC support [env AA_HEVC=1]
    --ctl-sock <PATH>       gate on aa-carlife-shim: switch a phone only after the
                            shim's \"start\", report session states back to it
                            [env AA_CTL_SOCK] [default: not gated]
    -n, --dry-run           print the resolved config (and load the GAL cert),
                            then exit — needs no phone, no USB, no hardware
    --debug-output <MODE>   accepted and ignored (added by svp_start_service)
    -h, --help              print this help and exit

ENV-ONLY:
    AA_HU_MAKE              head-unit make sent at service discovery; the phone
                            draws the exit-button logo from it (\"Volkswagen\" ->
                            VW logo, unknown -> generic \"Exit\") [default: {DEFAULT_HU_MAKE}]
    AA_HU_MODEL             head-unit model sent at service discovery [default: {DEFAULT_HU_MODEL}]
    AA_HU_NAME              the label under the exit tile in Android Auto's launcher
                            [default: the make]
    AA_KNOB_LEFT/RIGHT      RotateKeyEvent codes for the knob (with --ctl-sock) [default: 7/6]
    AA_TCP_CONNECT          <host:port> of a phone's head unit server (Android Auto
                            developer setting; tests away from the car) instead of USB

The GAL head-unit certificate/key is bundled in livi-aa (cert.rs); no
provisioning is required. Video/audio go to the feed socket via the AA
session's runtime JSON sink config; the mic is bridged to the mic socket."
    );
}

// ---------------------------------------------------------------------------
// Config banner / dry-run output
// ---------------------------------------------------------------------------

fn print_config(cfg: &Config, dry: bool) {
    let (tw, th) = fitting_tier(cfg.display_w, cfg.display_h, !cfg.hevc);
    println!("livi-aa-headless — Android Auto host for Desay 6RD 035 187B (armv7)");
    println!("  mode              : {}", if dry { "dry-run (no hardware touched)" } else { "live" });
    println!("  video feed socket : {}", cfg.video_sock);
    println!("  mic socket        : {}", cfg.mic_sock);
    println!(
        "  usb filter        : {}",
        if cfg.usb_filter.is_empty() { "(all phones)".into() } else { format!("serial contains {:?}", cfg.usb_filter) }
    );
    println!(
        "  head unit brand   : make={:?} model={:?}  (AA exit-button logo; env AA_HU_MAKE/AA_HU_MODEL)",
        cfg.hu_make, cfg.hu_model
    );
    println!(
        "  carlife shim      : {}",
        if cfg.ctl_sock.is_empty() {
            "(none: not gated, every phone is switched on plug-in)".into()
        } else {
            format!("{} (gated: switch only after 'start'; knob left/right codes {}/{})", cfg.ctl_sock, cfg.knob_left, cfg.knob_right)
        }
    );
    println!("  exit tile label   : {:?}  (env AA_HU_NAME; default: the make)", hu_name(cfg));
    println!(
        "  car BT address    : {}  (advertised to {})",
        std::env::var("AA_BT_MAC").unwrap_or_else(|_| "(none yet: the shim's btmac)".into()),
        if bt_adapters_only() { "wireless adapters only" } else { "every phone" }
    );
    println!(
        "  driving           : restrictions {} while moving (AA_MOVING_RESTRICTIONS); hidden -> NATIVE after {} (AA_HIDE_NATIVE_MS)",
        ctl::moving_restrictions_from_env(),
        ctl::hide_native_from_env().map_or("never".into(), |d| format!("{} ms", d.as_millis()))
    );
    println!(
        "  log               : {}",
        if livi_aa_stack::log::debug() {
            "DEBUG=1: every message and event"
        } else {
            "lifecycle only (DEBUG=1 adds every message, media and navigation event)"
        }
    );
    println!("  sessions dir      : {}", cfg.sessions_dir.display());
    println!("  helper socket     : {}", cfg.helper_sock.display());
    println!(
        "  display -> AA tier: {}x{} -> {}x{} @{}dpi, {}",
        cfg.display_w,
        cfg.display_h,
        tw,
        th,
        android_auto_dpi(tw, th),
        if cfg.hevc { "H.264+H.265" } else { "H.264" }
    );
    println!(
        "  mic device        : {}",
        if cfg.mic_device.is_empty() { "(default)".into() } else { cfg.mic_device.clone() }
    );
    match livi_aa::tls::hu_cert() {
        Ok((certs, _key)) => println!(
            "  GAL HU cert       : loaded OK — {} certificate(s) + RSA key (bundled in livi-aa/cert.rs, issuer \"Google Automotive Link\")",
            certs.len()
        ),
        Err(e) => println!("  GAL HU cert       : FAILED to load: {e}"),
    }
    println!(
        "  sink JSON (sent to the session on 'ready'): {{\"type\":\"sink\",\"feed\":\"{}\",\"video\":[{{\"ch\":3,\"id\":1,\"codec\":\"{}\"}}]}}",
        cfg.video_sock,
        if cfg.hevc { "h265" } else { "h264" }
    );
    println!(
        "  audio             : PCM S16LE on the same feed socket; the driver adds audio sink\n                      entries on channel setup (feed-id 101=media 48k/2ch, 102=speech 16k/1ch).\n  mic               : phone-requested; a device-mic capture connects to {} and writes\n                      KIND_MIC PCM (16k/1ch) which the session relays to the phone.",
        cfg.mic_sock
    );
}

// ---------------------------------------------------------------------------
// In-process helper bridge  (the only stub: a minimal livi-helperd / Broadcaster)
//
// Speaks the exact line protocol livi-aa-stack's AaHelperSock expects:
//   * a client that sends "subscribe\n" is kept open and fed one JSON event
//     line per message;
//   * any other request line gets a single {"ok":true}\n reply.
// `on_session` from livi_aa::usb::run pushes {"event":"aa-session",...} lines.
// ---------------------------------------------------------------------------

#[derive(Clone)]
struct Helper {
    events: broadcast::Sender<String>,
    subscribed_tx: watch::Sender<bool>,
    subscribed_rx: watch::Receiver<bool>,
}

impl Helper {
    fn new() -> Self {
        let (events, _) = broadcast::channel(256);
        let (subscribed_tx, subscribed_rx) = watch::channel(false);
        Self { events, subscribed_tx, subscribed_rx }
    }

    fn push_json(&self, line: String) {
        let _ = self.events.send(line);
    }

    /// Resolves once at least one subscriber (the stack's manager) has attached.
    async fn subscribed(&self) {
        let mut rx = self.subscribed_rx.clone();
        let _ = rx.wait_for(|ready| *ready).await;
    }

    fn announce_session(&self, socket: &str, peer: &str, serial: &str) {
        let event = AaHelperEvent::AaSession {
            socket: socket.to_string(),
            peer: peer.to_string(),
            transport: AaTransport::Usb,
            serial: (!serial.is_empty()).then(|| serial.to_string()),
        };
        self.push_json(event.to_line());
    }

    async fn accept_loop(self, listener: UnixListener) {
        loop {
            match listener.accept().await {
                Ok((sock, _)) => {
                    let helper = self.clone();
                    tokio::spawn(async move { helper.serve_client(sock).await });
                }
                Err(e) => {
                    eprintln!("[helper] accept: {e}");
                    return;
                }
            }
        }
    }

    async fn serve_client(self, sock: UnixStream) {
        let (rd, mut wr) = sock.into_split();
        let mut lines = BufReader::new(rd).lines();
        let first = match lines.next_line().await {
            Ok(Some(l)) => l,
            _ => return,
        };
        if first.trim() != "subscribe" {
            let _ = wr.write_all(b"{\"ok\":true}\n").await;
            return;
        }
        let mut rx = self.events.subscribe();
        let _ = self.subscribed_tx.send(true);
        loop {
            tokio::select! {
                got = rx.recv() => match got {
                    Ok(line) => {
                        if wr.write_all(line.as_bytes()).await.is_err()
                            || wr.write_all(b"\n").await.is_err()
                        {
                            return;
                        }
                    }
                    Err(broadcast::error::RecvError::Lagged(_)) => {}
                    Err(broadcast::error::RecvError::Closed) => return,
                },
                // Notice the subscriber hanging up.
                next = lines.next_line() => match next {
                    Ok(Some(_)) => {}   // ignore any further input
                    _ => return,
                },
            }
        }
    }
}

// ---------------------------------------------------------------------------
// AaMedia seam — points the feed at the configured sockets, no renderer.
// ---------------------------------------------------------------------------

struct HeadlessMedia {
    video_sock: String,
    mic_sock: String,
    opened: broadcast::Sender<AudioOutput>,
    taps: Mutex<HashMap<u32, JoinHandle<()>>>,
    // stream -> (channel tag, sample_rate, channels), recorded at prime_audio so
    // audio_feed() can hand out a DETERMINISTIC, channel-keyed feed id (below).
    audio_fmt: Mutex<HashMap<u32, (String, u32, u32)>>,
    next_tap: AtomicU32,
    next_audio_stream: AtomicU32,
}

impl HeadlessMedia {
    fn new(video_sock: String, mic_sock: String) -> Self {
        let (opened, _) = broadcast::channel(16);
        Self {
            video_sock,
            mic_sock,
            opened,
            taps: Mutex::new(HashMap::new()),
            audio_fmt: Mutex::new(HashMap::new()),
            next_tap: AtomicU32::new(1),
            next_audio_stream: AtomicU32::new(1),
        }
    }
}

/// Deterministic KIND_AUDIO feed id, keyed by the AA audio channel name. The
/// downstream bridge keeps the matching fixed id->format table (aa-bridge
/// `AudioOut` defaults): 101=media 48k/2ch, 102=speech 16k/1ch,
/// 103=system 16k/1ch, 104=telephony 16k/1ch (all S16LE). An unknown tag falls
/// back to 100+stream (bridge then resamples at its default and logs it).
fn audio_feed_id(tag: &str, stream: u32) -> u32 {
    match tag {
        "media" => 101,
        "speech" => 102,
        "system" => 103,
        "telephony" => 104,
        _ => 100 + stream,
    }
}

impl AaMedia for HeadlessMedia {
    async fn feed_path(&self) -> String {
        // Every video + audio feed record is written to this unix socket; the
        // AA session's FeedWriter connects here and a downstream bridge listens.
        self.video_sock.clone()
    }

    fn video_feed(&self, _session: SessionId, cluster: bool) -> u32 {
        if cluster { 2 } else { 1 }
    }

    fn prime_video(&self, session: SessionId, cluster: bool, codec: VideoCodec) {
        println!(
            "[media] prime video session={session} cluster={cluster} codec={} -> feed {}",
            codec.name(),
            self.video_sock
        );
    }

    fn video_started(&self, cluster: bool, width: u32, height: u32) {
        println!("[media] video started cluster={cluster} {width}x{height}");
    }

    fn audio_feed(&self, _session: SessionId, stream: u32) -> u32 {
        let tag = self
            .audio_fmt
            .lock()
            .unwrap()
            .get(&stream)
            .map(|(t, _, _)| t.clone());
        audio_feed_id(tag.as_deref().unwrap_or(""), stream)
    }

    fn set_active(&self, session: SessionId, active: bool) {
        println!("[media] video focus session={session} active={active}");
    }

    fn release(&self, session: SessionId) {
        println!("[media] release session={session}");
    }

    fn audio_outputs(&self) -> Vec<AudioOutput> {
        Vec::new()
    }

    fn audio_output_opened(&self) -> broadcast::Receiver<AudioOutput> {
        self.opened.subscribe()
    }

    fn prime_audio(&self, kind: AudioKind, sample_rate: u32, channels: u32, tag: &str) {
        // Announce the opened output so the driver routes this audio channel to
        // the same feed socket (distinguished by KIND_AUDIO + id on the wire).
        let stream = self.next_audio_stream.fetch_add(1, Ordering::Relaxed);
        self.audio_fmt
            .lock()
            .unwrap()
            .insert(stream, (tag.to_string(), sample_rate, channels));
        let feed = audio_feed_id(tag, stream);
        println!(
            "[media] prime audio kind={kind:?} {sample_rate}Hz {channels}ch tag={tag} -> stream {stream} feed-id {feed} (bridge plays this id as {sample_rate}Hz/{channels}ch S16LE)"
        );
        let _ = self.opened.send(AudioOutput {
            kind,
            stream,
            tag: Some(tag.to_string()),
        });
    }

    fn set_host_volume(&self, kind: AudioKind, level: f64, ramp_ms: u32) {
        println!("[media] host volume kind={kind:?} level={level} ramp={ramp_ms}ms");
    }

    fn open_mic_tap(
        &self,
        path: &str,
        sample_rate: u32,
        channels: u32,
        device: &str,
    ) -> Option<u32> {
        // `path` is the session-owned mic socket (the session listens on it and
        // reads KIND_MIC feedproto records). We bridge it to the fixed, external
        // mic socket (self.mic_sock): a device-mic capture connects there and we
        // relay its bytes to the session. This is how the headless host exposes
        // the configurable mic path, since the sink JSON only configures video.
        let id = self.next_tap.fetch_add(1, Ordering::Relaxed);
        println!(
            "[media] open mic tap#{id} {sample_rate}Hz {channels}ch device={} : {} -> {}",
            if device.is_empty() { "(default)" } else { device },
            self.mic_sock,
            path
        );
        let listen = self.mic_sock.clone();
        let session_mic = path.to_string();
        let handle = tokio::spawn(mic_relay(listen, session_mic));
        self.taps.lock().unwrap().insert(id, handle);
        Some(id)
    }

    fn close_mic_tap(&self, id: u32) {
        if let Some(handle) = self.taps.lock().unwrap().remove(&id) {
            handle.abort();
            println!("[media] close mic tap#{id}");
        }
        let _ = std::fs::remove_file(&self.mic_sock);
    }
}

/// Bridge the external (device-mic producer) socket to the session mic socket.
async fn mic_relay(listen_path: String, session_mic: String) {
    let _ = std::fs::remove_file(&listen_path);
    let listener = match UnixListener::bind(&listen_path) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("[mic] bind {listen_path}: {e}");
            return;
        }
    };
    let _ = std::fs::set_permissions(&listen_path, std::fs::Permissions::from_mode(0o666));
    loop {
        let (mut producer, _) = match listener.accept().await {
            Ok(x) => x,
            Err(e) => {
                eprintln!("[mic] accept {listen_path}: {e}");
                return;
            }
        };
        match UnixStream::connect(&session_mic).await {
            Ok(mut session) => {
                if let Err(e) = tokio::io::copy(&mut producer, &mut session).await {
                    eprintln!("[mic] relay ended: {e}");
                }
            }
            Err(e) => eprintln!("[mic] connect session {session_mic}: {e}"),
        }
    }
}

// ---------------------------------------------------------------------------
// Live run
// ---------------------------------------------------------------------------

/// The label under the exit tile in Android Auto's launcher: the car's make, as on any
/// factory head unit (AA_HU_NAME overrides it).
fn hu_name(cfg: &Config) -> String {
    std::env::var("AA_HU_NAME")
        .ok()
        .filter(|n| !n.trim().is_empty())
        .unwrap_or_else(|| cfg.hu_make.clone())
}

/// AA_BT_ADVERTISE=adapters: the car's Bluetooth address only to wireless adapters.
fn bt_adapters_only() -> bool {
    std::env::var("AA_BT_ADVERTISE").is_ok_and(|v| v == "adapters")
}

fn build_aa_config(cfg: &Config) -> AaConfig {
    let (tw, th) = fitting_tier(cfg.display_w, cfg.display_h, !cfg.hevc);
    AaConfig {
        hu_name: Some(hu_name(cfg)),
        hu_make: Some(cfg.hu_make.clone()),
        hu_model: Some(cfg.hu_model.clone()),
        video_width: Some(tw),
        video_height: Some(th),
        video_dpi: Some(android_auto_dpi(tw, th)),
        video_fps: Some(30),
        pixel_aspect_ratio_e4: Some(10_000),
        display_width: cfg.display_w,
        display_height: cfg.display_h,
        hevc_supported: cfg.hevc,
        mic_device: cfg.mic_device.clone(),
        // The car's Bluetooth address, advertised in service discovery (else 00:00:00:00:00:00).
        // A wired phone ignores it; through a wireless adapter the phone pairs the car by it.
        // The shim reads the unit's own from its Bluetooth service and sends it (ctl `btmac`),
        // replacing this one.
        bt_mac_address: std::env::var("AA_BT_MAC").ok().filter(|m| m.len() == 17),
        // To every session; AA_BT_ADVERTISE=adapters keeps it from phones on USB. A phone that
        // gets it connects to the car over Bluetooth: it must get an honest answer whether the
        // car knows it (bt_pairing_by_host), else the unit's Bluetooth app asks to pair a phone
        // it does not know (step 3 runs 30, 31: we answered "already paired").
        bt_mac_adapters_only: bt_adapters_only(),
        // With the shim, the car's own Bluetooth answers whether the phone is paired, and
        // pairs it seamlessly when it is not (ctl `btpair` / `btpaired` / `btauth`).
        bt_pairing_by_host: !cfg.ctl_sock.is_empty(),
        // Numeric comparison only (BluetoothPairingMethod 2): the car's Bluetooth does SSP
        // numeric comparison whatever the phone asked for (its code dialog, step 3 run 35), so the
        // phone must expect that key. Offered PIN as well, a Pixel 9a always picked PIN (runs
        // 32, 33, 35) and then waited for a PIN the car never asked for.
        bt_pairing_methods: vec![2],
        // Only what the shim feeds (ctl `speed`, `gear`, `night`; driving status from the
        // speed). The unit has no GPS receiver (step 3 run 41: its ttyS4 has no UART behind
        // it), and nothing else reaches us yet.
        sensor_types: livi_aa_stack::discovery::DRIVING_SENSORS
            .iter()
            .map(|t| *t as i32)
            .collect(),
        ..AaConfig::default()
    }
}

async fn run(cfg: Config) {
    print_config(&cfg, false);

    if let Err(e) = std::fs::create_dir_all(&cfg.sessions_dir) {
        eprintln!("[headless] sessions dir {}: {e}", cfg.sessions_dir.display());
    }

    // 1) Bring up the in-process helper bridge (bind before the manager subscribes).
    let helper = Helper::new();
    let _ = std::fs::remove_file(&cfg.helper_sock);
    let helper_listener = match UnixListener::bind(&cfg.helper_sock) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("[headless] helper socket {}: {e}", cfg.helper_sock.display());
            return;
        }
    };
    let _ = std::fs::set_permissions(&cfg.helper_sock, std::fs::Permissions::from_mode(0o666));
    tokio::spawn(helper.clone().accept_loop(helper_listener));

    // 2) Start the AA protocol driver (the "node") pointed at our sockets.
    let media = Arc::new(HeadlessMedia::new(cfg.video_sock.clone(), cfg.mic_sock.clone()));
    let (cfg_tx, cfg_rx) = watch::channel(build_aa_config(&cfg));
    let (ev_tx, mut ev_rx) = mpsc::unbounded_channel::<AaEvent>();
    let handle = manager::start(AaHelperSock::new(&cfg.helper_sock), media, cfg_rx, ev_tx);
    // Kept for the lifetime of the process; the shim link updates the night mode through it.
    let cfg_tx = Arc::new(cfg_tx);

    // 3) The USB watcher's control. The serial filter is applied by the watcher BEFORE the
    //    AOAP switch, so a phone that does not match is never pushed into accessory mode (it
    //    stays usable for MTP, charging or the stock connectionservice). With the CarLife
    //    shim, the watcher is also gated: nothing is switched until the shim says "start"
    //    (gate.rs: when it closes again, and what a "stop" ends).
    let mut control = if cfg.usb_filter.is_empty() {
        Control::default()
    } else {
        Control::with_serial_filter(&cfg.usb_filter)
    };
    let notes = (!cfg.ctl_sock.is_empty()).then(|| {
        let (notes_tx, notes) = mpsc::unbounded_channel::<UsbNote>();
        control = control.clone().gated(notes_tx);
        notes
    });
    let gate = Gate::new(control.clone(), notes.is_some());
    let link = notes.map(|mut notes| {
        let link = ctl::spawn(
            cfg.ctl_sock.clone(),
            gate.clone(),
            handle.clone(),
            ctl::Geometry {
                width: cfg.display_w,
                height: cfg.display_h,
                knob_left: cfg.knob_left,
                knob_right: cfg.knob_right,
            },
            cfg_tx.clone(),
        );
        let to_shim = link.clone();
        let gate = gate.clone();
        tokio::spawn(async move {
            while let Some(n) = notes.recv().await {
                match n {
                    UsbNote::Switching { .. } => to_shim.state("connecting"),
                    UsbNote::SwitchFailed { .. } => {
                        to_shim.state("failed");
                        gate.switch_failed();
                    }
                }
            }
        });
        link
    });

    // Session events: log them, track the session the shim's input goes to, and turn them
    // into the shim's states (connected; projecting on the first projected video focus;
    // disconnected), audio / mic activity and the phone's "back to the car" icon.
    let events_gate = gate.clone();
    let events_handle = handle.clone();
    tokio::spawn(async move {
        while let Some(ev) = ev_rx.recv().await {
            if let Some(line) = event_line(&ev, livi_aa_stack::log::debug()) {
                println!("[aa-event] {line}");
            }
            match &ev {
                AaEvent::Spawned { id, usb_serial, .. } => events_gate.spawned(*id, usb_serial.clone()),
                AaEvent::Connected { .. } => {
                    if let Some(l) = &link {
                        l.state("connected");
                    }
                }
                AaEvent::VideoFocus { id, cluster: false, projected: true } => {
                    if events_gate.projected(*id)
                        && let Some(l) = &link
                    {
                        l.state("projecting");
                    }
                }
                AaEvent::Audio { stream, active, .. } => {
                    if let Some(l) = &link {
                        l.audio(*stream, *active);
                    }
                }
                AaEvent::Mic { active, .. } => {
                    if let Some(l) = &link {
                        l.mic(*active);
                    }
                }
                AaEvent::Calls { calls, .. } => {
                    if let Some(l) = &link {
                        l.calls(calls.len());
                    }
                }
                AaEvent::HostUiRequested { .. } => {
                    if let Some(l) = &link {
                        l.host_ui();
                    }
                }
                // The shim asks the car's Bluetooth; without an answer in 2 s the phone gets
                // the old "already paired".
                AaEvent::BtPairingRequest { id, address, method } => {
                    let id = *id;
                    let already_paired = livi_aa_stack::driver::SessionCmd::BtPairingResponse { already_paired: true };
                    let Some(l) = &link else {
                        events_handle.session(id, already_paired);
                        continue;
                    };
                    // The phone's address goes onto the line protocol as it sent it: only an
                    // address does (a space or a newline in it would split or add a line).
                    if ctl::bt_address(address).is_none() {
                        println!("[ctl] pairing request without a usable address {address:?}: already paired");
                        events_handle.session(id, already_paired);
                        continue;
                    }
                    l.bt_pair(id, address, *method);
                    let h = events_handle.clone();
                    tokio::spawn(async move {
                        tokio::time::sleep(Duration::from_secs(2)).await;
                        if ctl::bt_pair_take(id) {
                            println!("[ctl] no pairing answer from the shim: already paired");
                            h.session(id, already_paired);
                        }
                    });
                }
                AaEvent::Disconnected { id } => {
                    // A session that ends after a newer one started (the phone came back
                    // before the old one was closed) is not news to the shim: the newer one
                    // is what it shows.
                    if !events_gate.ended(*id) {
                        println!("[ctl] session {id} ended after a newer one started; not reported");
                    } else if let Some(l) = &link {
                        l.state("disconnected");
                        l.session_ended();
                    }
                }
                _ => {}
            }
        }
    });

    // 4) Start the USB watcher / AOAP switch. Each ready session is announced to
    //    the manager through the helper bridge. `subscribed` gates serving until
    //    the manager has attached.
    let announce = helper.clone();
    let subscribed = helper.clone();
    println!("[headless] starting USB watcher (sessions dir {})", cfg.sessions_dir.display());

    let tcp = std::env::var("AA_TCP_CONNECT").ok().filter(|a| !a.is_empty());
    let watch_control = control.clone();
    let sessions_dir = cfg.sessions_dir.clone();
    let watcher = async move {
        if let Some(addr) = tcp {
            println!("[headless] AA_TCP_CONNECT: the phone is the head unit server at {addr}, not USB");
            tcp_connect(addr, watch_control, sessions_dir, announce).await;
        } else {
            let _ = usb::run(
                watch_control,
                sessions_dir,
                move |socket, peer, serial| announce.announce_session(socket, peer, serial),
                async move { subscribed.subscribed().await },
            )
            .await;
        }
    };

    tokio::select! {
        () = watcher => eprintln!("[headless] USB watcher ended"),
        () = interrupted() => println!("[headless] interrupted: shutting down"),
        () = terminated() => println!("[headless] SIGTERM: shutting down"),
    }
    control.arm(false);
    shut_down(&handle).await;
}

/// AA_TCP_CONNECT=<host:port>, away from the car: the phone is a head unit server (Android
/// Auto's developer setting "Start head unit server", port 5277, reached through `adb forward`,
/// as Google's Desktop Head Unit does) instead of a USB accessory. One session at a time,
/// announced and run as a USB one is; with the shim, only while its gate is armed. The unit
/// never sets it.
async fn tcp_connect(addr: String, control: Control, sessions_dir: PathBuf, helper: Helper) {
    helper.subscribed().await;
    loop {
        if !control.armed() {
            tokio::time::sleep(TCP_GATE_POLL).await;
            continue;
        }
        let tcp = match TcpStream::connect(&addr).await {
            Ok(t) => t,
            Err(e) => {
                eprintln!("[aa-tcp] connect {addr}: {e}");
                tokio::time::sleep(TCP_RETRY).await;
                continue;
            }
        };
        let _ = tcp.set_nodelay(true);
        let Some((node, path)) = bind_session_socket(&sessions_dir, "aa-session") else {
            tokio::time::sleep(TCP_RETRY).await;
            continue;
        };
        println!("[aa-tcp] connected to {addr}, session socket {path}");
        helper.announce_session(&path, &addr, "");
        let peer = Peer {
            label: addr.clone(),
            ip: tcp.peer_addr().map_or(IpAddr::V4(Ipv4Addr::LOCALHOST), |a| a.ip()),
        };
        let end = session::run(tcp, peer, node, path, Arc::new(Notify::new())).await;
        println!("[aa-tcp] session ended (closed by this side: {})", end.closed_by_node);
        tokio::time::sleep(TCP_RETRY).await;
    }
}

/// Ctrl-C. Never resolves if the handler cannot be installed.
async fn interrupted() {
    if let Err(e) = tokio::signal::ctrl_c().await {
        eprintln!("[headless] Ctrl-C handler: {e}");
        std::future::pending::<()>().await;
    }
}

/// SIGTERM. Never resolves if the handler cannot be installed (the default action, an exit
/// by the signal, stays).
async fn terminated() {
    match tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate()) {
        Ok(mut term) => {
            term.recv().await;
        }
        Err(e) => {
            eprintln!("[headless] SIGTERM handler: {e}");
            std::future::pending::<()>().await;
        }
    }
}

/// End every session with a ByeBye before the exit, so the phone leaves accessory mode and
/// comes back as itself. The next receiver (procguard starts one) then meets it the way the
/// platform does, instead of an accessory mid-session with a host gone. Bounded: a phone that
/// does not answer does not hold up the exit.
async fn shut_down(handle: &AaHandle) {
    handle.send(AaCmd::Detach);
    if tokio::time::timeout(SHUTDOWN_WAIT, handle.phones_gone()).await.is_err() {
        eprintln!(
            "[headless] sessions still open after {} ms: exiting anyway",
            SHUTDOWN_WAIT.as_millis()
        );
    }
    tokio::time::sleep(SHUTDOWN_USB_RESET).await;
}

/// The log line of a session event. Lifecycle events (sessions, video and audio focus, audio
/// and mic, pairing, calls) always; the phone's media, navigation and status reports only with
/// DEBUG=1, as they come every second while music plays or a route is guided. An image (album
/// art, a turn icon) is logged by its size, never its bytes: one cover printed as numbers was
/// 650 KB of log (step 3 run 47).
fn event_line(ev: &AaEvent, debug: bool) -> Option<String> {
    match ev {
        AaEvent::NowPlaying { id, now_playing } => debug.then(|| {
            let NowPlaying {
                title,
                artist,
                album,
                app,
                duration_ms,
                elapsed_ms,
                playing,
                artwork,
            } = now_playing;
            format!(
                "NowPlaying {{ id: {id}, title: {title:?}, artist: {artist:?}, album: {album:?}, app: {app:?}, \
                 duration_ms: {duration_ms:?}, elapsed_ms: {elapsed_ms:?}, playing: {playing:?}, artwork: {} bytes }}",
                artwork.as_ref().map_or(0, Vec::len)
            )
        }),
        AaEvent::NavigationImage { id, image } => {
            debug.then(|| format!("NavigationImage {{ id: {id}, image: {} bytes }}", image.len()))
        }
        AaEvent::Navigation { .. } | AaEvent::Status { .. } => debug.then(|| format!("{ev:?}")),
        _ => Some(format!("{ev:?}")),
    }
}

// `aa-headless shim <args>` runs the CarLife shim (livi-carlife-shim) in this process instead of
// the receiver: one binary for both slots. Shim mode never starts tokio.
fn main() {
    let args: Vec<String> = std::env::args_os().map(|a| a.to_string_lossy().into_owned()).collect();
    if args.get(1).map(String::as_str) == Some("shim") {
        let shim_args = std::iter::once(args[0].clone()).chain(args[2..].iter().cloned()).collect();
        std::process::exit(livi_carlife_shim::main(shim_args));
    }
    tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .expect("Failed building the Runtime")
        .block_on(async_main());
}

async fn async_main() {
    match parse_args() {
        Parsed::Help => print_help(),
        // Exit non-zero: procguard (svp/bin/procguard) re-execs a service 50ms after a
        // status-0 exit but gives up on any non-zero exit code, so a bad config does not
        // turn into a restart loop. (`--debug-output`, which the platform adds, never lands
        // here.) Every other exit is 0, after a SIGTERM too: procguard starts the receiver
        // again, within the launcher's crash budget.
        Parsed::Invalid => {
            print_help();
            std::process::exit(1);
        }
        Parsed::Run(cfg) if cfg.dry_run => print_config(&cfg, true),
        Parsed::Run(cfg) => run(cfg).await,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use livi_aa_stack::bridge::Navigation;

    fn args(list: &[&str]) -> Parsed {
        parse_args_from(list.iter().map(|s| s.to_string()))
    }

    // procguard appends `--debug-output $SVP_DEBUG_OUTPUT`; with the variable empty there is
    // no value. Neither form may stop the receiver (a non-zero exit keeps AA down for the boot).
    #[test]
    fn debug_output_is_taken_with_or_without_a_value() {
        assert!(matches!(args(&["--debug-output", "none"]), Parsed::Run(_)));
        assert!(matches!(args(&["--debug-output=kmsg"]), Parsed::Run(_)));
        assert!(matches!(args(&["--debug-output"]), Parsed::Run(_)));
        assert!(matches!(args(&["--debug-output", "--dry-run"]), Parsed::Run(c) if c.dry_run));
        assert!(matches!(args(&["--debug-output", "none", "--bogus"]), Parsed::Run(_)));
        assert!(matches!(args(&["--display"]), Parsed::Invalid));
    }

    #[test]
    fn media_and_navigation_events_are_logged_only_with_debug() {
        let art = vec![137u8; 143_334];
        let playing = AaEvent::NowPlaying {
            id: 3,
            now_playing: NowPlaying {
                title: Some("Crab Rave".into()),
                artwork: Some(art.clone()),
                ..Default::default()
            },
        };
        assert_eq!(event_line(&playing, false), None);
        let line = event_line(&playing, true).unwrap();
        assert!(line.contains("Crab Rave") && line.contains("artwork: 143334 bytes"), "{line}");
        assert!(line.len() < 400, "the artwork is not in the line");
        let image = AaEvent::NavigationImage { id: 3, image: art };
        assert_eq!(event_line(&image, false), None);
        assert_eq!(event_line(&image, true).unwrap(), "NavigationImage { id: 3, image: 143334 bytes }");
        let nav = AaEvent::Navigation { id: 3, navigation: Navigation::default() };
        assert_eq!(event_line(&nav, false), None);
        assert!(event_line(&nav, true).is_some());
    }

    #[test]
    fn lifecycle_events_are_always_logged() {
        for ev in [
            AaEvent::Spawned { id: 1, wired: true, usb_serial: Some("S".into()), peer: "usb:S".into() },
            AaEvent::Connected { id: 1 },
            AaEvent::VideoFocus { id: 1, cluster: false, projected: true },
            AaEvent::HostUiRequested { id: 1 },
            AaEvent::Mic { id: 1, active: true },
            AaEvent::Disconnected { id: 1 },
        ] {
            assert!(event_line(&ev, false).is_some(), "{ev:?}");
        }
    }
}
