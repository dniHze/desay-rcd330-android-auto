// SPDX-FileCopyrightText: 2025-2026 Lasse Heitgres
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional terms: see NOTICE

use std::collections::{HashMap, HashSet, VecDeque};
use std::net::IpAddr;
use std::os::unix::fs::PermissionsExt;
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use livi_aa_proto::VersionResponseOptions;
use livi_host_proto::feed as feedproto;
use prost::Message as _;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::net::{UnixListener, UnixStream};
use tokio::sync::{Notify, mpsc};

use crate::av;
use crate::consts::*;
use crate::frame::{self, FrameParser, FrameSplitter, RawFrame};
use crate::tls::{Message, TlsEngine};
use livi_aa_proto::consts::{ch, ctrl_msg, frame_flags, media_msg};
use livi_session_io::feed::FeedWriter;
use livi_session_io::link;

const NODE_ACCEPT_TIMEOUT: Duration = Duration::from_secs(10);
const SETUP_TIMEOUT: Duration = Duration::from_secs(30);
const VIDEO_BACKLOG: usize = 240;
const AUDIO_BACKLOG: usize = 64;
const READ_CHUNK: usize = 65536;
/// How long a session outlives the phone's end of the link. The main process may close it
/// first (it gets an "eof" control); if not, the link is dead: over USB the phone left
/// accessory mode (Android Auto stopped from the phone's menu) and can come straight back
/// under the same serial, so no unplug cancels the old session. Left open it held the video
/// feed, and the next session waited behind it (step 3 on the unit, run 12).
const PHONE_EOF_GRACE: Duration = Duration::from_secs(3);
/// A feed the host closed is reopened after this long; one it never took, after twice the
/// last wait, up to FEED_REOPEN_MAX (Sinks::revive).
const FEED_REOPEN: Duration = Duration::from_secs(1);
const FEED_REOPEN_MAX: Duration = Duration::from_secs(60);

struct SocketPath(String);

impl Drop for SocketPath {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}

pub struct Peer {
    pub label: String,
    pub ip: IpAddr,
}

pub struct SessionEnd {
    pub closed_by_node: bool,
    pub running: bool,
}

#[derive(PartialEq, Eq)]
enum Phase {
    Version,
    Handshake,
    Running,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct PingConfig {
    interval_ms: u64,
    timeout_ms: u64,
}

fn ping_config(options: &[u8]) -> Option<PingConfig> {
    let options = VersionResponseOptions::decode(options).ok()?;
    let ping = options.connection_configuration?.ping_configuration?;
    Some(PingConfig {
        interval_ms: u64::from(ping.interval_ms?),
        timeout_ms: u64::from(ping.timeout_ms?),
    })
}

struct Sinks {
    feed: Option<(String, FeedWriter)>,
    video: HashMap<u8, (u32, u8)>,
    audio: HashMap<u8, u32>,
    backlog: HashMap<u8, VecDeque<(u64, Vec<u8>)>>,
    /// When the feed was last opened, and how long until a closed one is opened again (see
    /// revive).
    opened_at: Option<Instant>,
    reopen_after: Duration,
    /// The feed was reopened in mid-session: once it is connected, the main video needs a
    /// keyframe, as the new host's decoder cannot start on the frames that follow.
    keyframe_due: bool,
}

impl Default for Sinks {
    fn default() -> Self {
        Self {
            feed: None,
            video: HashMap::new(),
            audio: HashMap::new(),
            backlog: HashMap::new(),
            opened_at: None,
            reopen_after: FEED_REOPEN,
            keyframe_due: false,
        }
    }
}

impl Sinks {
    fn apply(&mut self, v: &serde_json::Value) {
        if let Some(path) = v.get("feed").and_then(|p| p.as_str())
            && !path.is_empty()
            && self
                .feed
                .as_ref()
                .is_none_or(|(p, writer)| p != path || writer.closed())
        {
            self.open(path.to_owned());
        }
        for entry in v
            .get("video")
            .and_then(|a| a.as_array())
            .into_iter()
            .flatten()
        {
            let (Some(ch), Some(id)) = (
                entry.get("ch").and_then(|c| c.as_u64()),
                entry.get("id").and_then(|i| i.as_u64()),
            ) else {
                continue;
            };
            let codec = match entry.get("codec").and_then(|c| c.as_str()) {
                Some("h264") => 0,
                Some("h265") => 1,
                _ => 0xff,
            };
            self.video.insert(ch as u8, (id as u32, codec));
            self.send(feedproto::encode(
                feedproto::KIND_VIDEO_START,
                id as u32,
                0,
                &[codec],
            ));
            self.flush(ch as u8);
        }
        for entry in v
            .get("audio")
            .and_then(|a| a.as_array())
            .into_iter()
            .flatten()
        {
            let (Some(ch), Some(id)) = (
                entry.get("ch").and_then(|c| c.as_u64()),
                entry.get("id").and_then(|i| i.as_u64()),
            ) else {
                continue;
            };
            self.audio.insert(ch as u8, id as u32);
            self.flush(ch as u8);
        }
    }

    /// (Re)open the feed to the host. Known video streams are announced again (the host may
    /// be a new process) and what waited is flushed.
    fn open(&mut self, path: String) {
        let reopened = self.feed.is_some();
        self.feed = Some((path.clone(), FeedWriter::open(path)));
        self.opened_at = Some(Instant::now());
        self.keyframe_due |= reopened;
        let known: Vec<(u8, u32, u8)> = self
            .video
            .iter()
            .map(|(ch, (id, codec))| (*ch, *id, *codec))
            .collect();
        for (_, id, codec) in known {
            self.send(feedproto::encode(
                feedproto::KIND_VIDEO_START,
                id,
                0,
                &[codec],
            ));
        }
        let waiting: Vec<u8> = self.backlog.keys().copied().collect();
        for ch in waiting {
            self.flush(ch);
        }
    }

    /// The host closed the feed (it restarted: the bridge under procguard) or never took it:
    /// open it again while media flows, else the session would stay without picture and
    /// sound to its end. One second after a feed that worked; doubling up to FEED_REOPEN_MAX
    /// while the host stays away, so a host that is down for good costs a line a minute.
    fn revive(&mut self) {
        let Some((path, writer)) = &self.feed else {
            return;
        };
        if !writer.closed() || self.opened_at.is_some_and(|t| t.elapsed() < self.reopen_after) {
            return;
        }
        let worked = writer.has_connected();
        let path = path.clone();
        self.reopen_after = if worked {
            FEED_REOPEN
        } else {
            (self.reopen_after * 2).clamp(FEED_REOPEN, FEED_REOPEN_MAX)
        };
        println!(
            "[aa-feed] {path} {}: reopening it",
            if worked { "closed by the host" } else { "not reached" }
        );
        self.open(path);
    }

    /// The reopened feed is connected and the main video needs a keyframe: true once.
    fn keyframe_wanted(&mut self) -> bool {
        let connected = self.feed.as_ref().is_some_and(|(_, w)| w.has_connected());
        if self.keyframe_due && connected {
            self.keyframe_due = false;
            return true;
        }
        false
    }

    fn push(&mut self, ch: u8, ts: u64, data: &[u8]) {
        self.revive();
        if self.feed.is_some()
            && let Some((kind, id)) = self.target(ch)
        {
            self.send(feedproto::encode(kind, id, ts, data));
            return;
        }
        let limit = if is_video_channel(ch) {
            VIDEO_BACKLOG
        } else {
            AUDIO_BACKLOG
        };
        let q = self.backlog.entry(ch).or_default();
        if q.len() >= limit {
            q.pop_front();
        }
        q.push_back((ts, data.to_vec()));
    }

    fn flush(&mut self, ch: u8) {
        if self.feed.is_none() {
            return;
        }
        let Some((kind, id)) = self.target(ch) else {
            return;
        };
        let Some(q) = self.backlog.remove(&ch) else {
            return;
        };
        for (ts, data) in q {
            self.send(feedproto::encode(kind, id, ts, &data));
        }
    }

    fn target(&self, ch: u8) -> Option<(u8, u32)> {
        if let Some((id, _)) = self.video.get(&ch) {
            return Some((feedproto::KIND_VIDEO, *id));
        }
        self.audio.get(&ch).map(|id| (feedproto::KIND_AUDIO, *id))
    }

    fn send(&self, record: Vec<u8>) {
        if let Some((_, w)) = &self.feed {
            w.send(record);
        }
    }
}

struct Session<W> {
    peer: Peer,
    mic_path: String,
    wr: W,
    node_tx: mpsc::Sender<Vec<u8>>,
    phase: Phase,
    parser: FrameParser,
    splitter: FrameSplitter,
    tls: Option<TlsEngine>,
    protocol: (u16, u16),
    ping: Option<PingConfig>,
    sinks: Sinks,
    session_ids: HashMap<u8, u32>,
    announced: HashSet<u8>,
    closed_sent: bool,
    closed_by_node: bool,
    video_frames: u64,
    audio_frames: u64,
}

pub async fn run<T>(
    io: T,
    peer: Peer,
    node: UnixListener,
    node_path: String,
    cancel: Arc<Notify>,
) -> SessionEnd
where
    T: AsyncRead + AsyncWrite + Send + 'static,
{
    let _path = SocketPath(node_path.clone());
    let node_stream = match tokio::time::timeout(NODE_ACCEPT_TIMEOUT, node.accept()).await {
        Ok(Ok((s, _))) => s,
        _ => {
            eprintln!(
                "[aa-session] {}: main process did not attach to {node_path}",
                peer.label
            );
            return SessionEnd {
                closed_by_node: false,
                running: false,
            };
        }
    };
    drop(node);

    let mic_path = format!("{node_path}.mic");
    let _mic_path = SocketPath(mic_path.clone());
    let (mic_tx, mut mic_rx) = mpsc::channel::<(u64, Vec<u8>)>(64);
    let _ = std::fs::remove_file(&mic_path);
    match UnixListener::bind(&mic_path) {
        Ok(listener) => {
            let _ = std::fs::set_permissions(&mic_path, std::fs::Permissions::from_mode(0o666));
            tokio::spawn(listen_mic(listener, mic_tx));
        }
        Err(e) => eprintln!("[aa-session] {}: mic socket {mic_path}: {e}", peer.label),
    }

    let (mut node_rd, mut node_wr) = node_stream.into_split();
    let (node_tx, mut node_rx) = mpsc::channel::<Vec<u8>>(1024);
    tokio::spawn(async move {
        while let Some(bytes) = node_rx.recv().await {
            if node_wr.write_all(&bytes).await.is_err() {
                break;
            }
        }
    });
    let (item_tx, mut items) = mpsc::channel::<link::Item>(1024);
    tokio::spawn(async move {
        let mut framer = link::Framer::default();
        let mut buf = vec![0u8; READ_CHUNK];
        loop {
            let n = match node_rd.read(&mut buf).await {
                Ok(0) | Err(_) => break,
                Ok(n) => n,
            };
            framer.push(&buf[..n]);
            while let Some(item) = framer.next_item() {
                if item_tx.send(item).await.is_err() {
                    return;
                }
            }
        }
    });

    let (mut rd, wr) = tokio::io::split(io);
    let mut s = Session {
        peer,
        mic_path,
        wr,
        node_tx,
        phase: Phase::Version,
        parser: FrameParser::default(),
        splitter: FrameSplitter::default(),
        tls: None,
        protocol: (0, 0),
        ping: None,
        sinks: Sinks::default(),
        session_ids: HashMap::new(),
        announced: HashSet::new(),
        closed_sent: false,
        closed_by_node: false,
        video_frames: 0,
        audio_frames: 0,
    };

    if let Err(e) = s.send_version_request().await {
        s.closed(&e).await;
        return SessionEnd {
            closed_by_node: false,
            running: false,
        };
    }

    let mut buf = vec![0u8; READ_CHUNK];
    let mut phone_open = true;
    let setup_deadline = tokio::time::sleep(SETUP_TIMEOUT);
    tokio::pin!(setup_deadline);
    let eof_deadline = tokio::time::sleep(Duration::MAX);
    tokio::pin!(eof_deadline);
    loop {
        let setting_up = s.phase != Phase::Running;
        tokio::select! {
            r = rd.read(&mut buf), if phone_open => match r {
                Ok(0) => {
                    phone_open = false;
                    eof_deadline.as_mut().reset(tokio::time::Instant::now() + PHONE_EOF_GRACE);
                    if !s.control("{\"type\":\"eof\"}").await {
                        break;
                    }
                }
                Ok(n) => {
                    if let Err(e) = s.on_tcp(&buf[..n]).await {
                        s.closed(&e).await;
                        break;
                    }
                }
                Err(e) => {
                    s.closed(&e.to_string()).await;
                    break;
                }
            },
            item = items.recv() => match item {
                Some(link::Item::Message { ch, flags, msg_id, payload }) => {
                    if let Err(e) = s.on_node_message(ch, flags, msg_id, &payload).await {
                        s.closed(&e).await;
                        break;
                    }
                }
                Some(link::Item::Control(json)) => {
                    if !s.on_control(&json).await {
                        break;
                    }
                }
                None => break,
            },
            mic = mic_rx.recv() => {
                if let Some((ts, pcm)) = mic
                    && let Err(e) = s.send_mic(ts, &pcm).await
                {
                    s.closed(&e).await;
                    break;
                }
            }
            _ = &mut setup_deadline, if setting_up => {
                s.closed("setup timeout").await;
                break;
            }
            _ = &mut eof_deadline, if !phone_open => {
                s.closed("phone closed the link").await;
                break;
            }
            _ = cancel.notified() => {
                s.closed("phone unplugged").await;
                break;
            }
        }
    }
    println!(
        "[aa-session] {}: session over, video={} audio={}",
        s.peer.label, s.video_frames, s.audio_frames
    );
    SessionEnd {
        closed_by_node: s.closed_by_node,
        running: s.phase == Phase::Running,
    }
}

impl<W: AsyncWrite + Unpin + Send> Session<W> {
    async fn send_version_request(&mut self) -> Result<(), String> {
        let mut data = VERSION_MAJOR.to_be_bytes().to_vec();
        data.extend_from_slice(&VERSION_MINOR.to_be_bytes());
        self.write(&frame::encode(
            ch::CONTROL,
            frame_flags::PLAINTEXT,
            ctrl_msg::VERSION_REQUEST,
            &data,
        ))
        .await
    }

    async fn write(&mut self, bytes: &[u8]) -> Result<(), String> {
        self.wr
            .write_all(bytes)
            .await
            .map_err(|e| format!("write: {e}"))
    }

    async fn control(&mut self, json: &str) -> bool {
        self.node_tx.send(link::encode_control(json)).await.is_ok()
    }

    async fn forward(
        &mut self,
        ch: u8,
        flags: u8,
        msg_id: u16,
        payload: &[u8],
    ) -> Result<(), String> {
        self.node_tx
            .send(link::encode_message(ch, flags, msg_id, payload))
            .await
            .map_err(|_| "main process gone".to_owned())
    }

    async fn closed(&mut self, reason: &str) {
        if self.closed_sent {
            return;
        }
        self.closed_sent = true;
        println!("[aa-session] {}: closed ({reason})", self.peer.label);
        let json = serde_json::json!({ "type": "closed", "reason": reason }).to_string();
        self.control(&json).await;
    }

    async fn on_tcp(&mut self, bytes: &[u8]) -> Result<(), String> {
        if self.phase == Phase::Running {
            self.splitter.push(bytes);
        } else {
            self.parser.push(bytes);
        }
        loop {
            if self.phase == Phase::Running {
                let Some(f) = self.splitter.next_frame() else {
                    break;
                };
                self.on_frame(f).await?;
            } else {
                let Some(f) = self.parser.next_frame() else {
                    break;
                };
                self.on_setup_frame(f).await?;
            }
        }
        Ok(())
    }

    async fn on_setup_frame(&mut self, f: RawFrame) -> Result<(), String> {
        if f.flags & frame_flags::ENCRYPTED != 0 {
            if self.tls.is_none() {
                return Ok(());
            }
            return self.on_encrypted(f).await;
        }
        let Some((msg_id, body)) = f.message() else {
            return Ok(());
        };
        match (msg_id, &self.phase) {
            (ctrl_msg::VERSION_RESPONSE, Phase::Version) => {
                if body.len() < 6 {
                    return Err("version response too short".into());
                }
                let status = u16::from_be_bytes([body[4], body[5]]);
                let (major, minor) = (
                    u16::from_be_bytes([body[0], body[1]]),
                    u16::from_be_bytes([body[2], body[3]]),
                );
                if status == VERSION_STATUS_MISMATCH {
                    return Err(format!("version mismatch {major}.{minor}"));
                }
                self.protocol = (major, minor);
                self.ping = ping_config(&body[6..]);
                let mut tls = TlsEngine::new(self.peer.ip).map_err(|e| e.to_string())?;
                let hello = tls.take_output();
                self.tls = Some(tls);
                self.phase = Phase::Handshake;
                self.write(&frame::encode(
                    ch::CONTROL,
                    frame_flags::PLAINTEXT,
                    ctrl_msg::ENCAPSULATED_SSL,
                    &hello,
                ))
                .await
            }
            (ctrl_msg::ENCAPSULATED_SSL, Phase::Handshake) => {
                let body = body.to_vec();
                let tls = self.tls.as_mut().ok_or("no tls")?;
                tls.inject_handshake(&body).map_err(|e| e.to_string())?;
                let out = tls.take_output();
                let done = !tls.is_handshaking();
                if !out.is_empty() {
                    self.write(&frame::encode(
                        ch::CONTROL,
                        frame_flags::PLAINTEXT,
                        ctrl_msg::ENCAPSULATED_SSL,
                        &out,
                    ))
                    .await?;
                }
                if done {
                    self.become_running().await?;
                }
                Ok(())
            }
            _ => Ok(()),
        }
    }

    async fn become_running(&mut self) -> Result<(), String> {
        self.splitter = std::mem::take(&mut self.parser).into_splitter();
        self.phase = Phase::Running;
        let (major, minor) = self.protocol;
        let ping = match self.ping {
            Some(p) => format!(
                ", the phone wants a ping every {} ms, gone after {} ms",
                p.interval_ms, p.timeout_ms
            ),
            None => String::new(),
        };
        println!(
            "[aa-session] {}: tls up, protocol {major}.{minor}{ping}",
            self.peer.label
        );
        let mut ready =
            serde_json::json!({ "type": "ready", "peer": self.peer.label, "mic": self.mic_path });
        if let Some(p) = self.ping {
            ready["ping"] =
                serde_json::json!({ "interval": p.interval_ms, "timeout": p.timeout_ms });
        }
        let json = ready.to_string();
        if !self.control(&json).await {
            return Err("main process gone".into());
        }
        Ok(())
    }

    async fn on_frame(&mut self, f: RawFrame) -> Result<(), String> {
        if f.flags & frame_flags::ENCRYPTED == 0 {
            if let Some((msg_id, body)) = f.message() {
                let body = body.to_vec();
                self.forward(f.ch, f.flags, msg_id, &body).await?;
            }
            return Ok(());
        }
        self.on_encrypted(f).await
    }

    async fn on_encrypted(&mut self, f: RawFrame) -> Result<(), String> {
        let tls = self.tls.as_mut().ok_or("encrypted frame before tls")?;
        let messages = tls
            .inject_record(f.ch, f.flags, &f.payload)
            .map_err(|e| e.to_string())?;
        for m in messages {
            self.dispatch(m).await?;
        }
        Ok(())
    }

    async fn dispatch(&mut self, m: Message) -> Result<(), String> {
        if is_video_channel(m.ch) || is_audio_channel(m.ch) {
            if is_media_message(m.msg_id) {
                return self.media(m).await;
            }
            if m.msg_id == media_msg::START {
                self.session_ids
                    .insert(m.ch, av::start_session_id(&m.payload).unwrap_or(0));
            }
        }
        self.forward(m.ch, m.flags, m.msg_id, &m.payload).await
    }

    async fn media(&mut self, m: Message) -> Result<(), String> {
        if av::wants_ack(self.protocol, m.ch) {
            let session_id = self.session_ids.get(&m.ch).copied().unwrap_or(0);
            let ack = self
                .tls
                .as_mut()
                .ok_or("no tls")?
                .encrypt(
                    m.ch,
                    frame_flags::ENC_SIGNAL,
                    media_msg::ACK,
                    &av::ack(session_id),
                )
                .map_err(|e| e.to_string())?;
            self.write(&ack).await?;
        }

        let (ts, data) = av::media(m.msg_id, &m.payload);
        let ts = ts.unwrap_or_else(now_ns);
        if is_video_channel(m.ch) {
            self.video_frames += 1;
        } else {
            self.audio_frames += 1;
        }
        self.sinks.push(m.ch, ts, data);
        if self.sinks.keyframe_wanted() {
            println!(
                "[aa-feed] {}: the feed is back, asking the phone for a keyframe",
                self.peer.label
            );
            if !self.control("{\"type\":\"feed-reopened\"}").await {
                return Err("main process gone".into());
            }
        }

        if self.announced.insert(m.ch) {
            let json = serde_json::json!({ "type": "first-frame", "ch": m.ch }).to_string();
            if !self.control(&json).await {
                return Err("main process gone".into());
            }
        }
        Ok(())
    }

    async fn send_mic(&mut self, ts_ns: u64, pcm: &[u8]) -> Result<(), String> {
        if self.phase != Phase::Running || pcm.is_empty() {
            return Ok(());
        }
        let mut payload = (ts_ns / 1000).to_be_bytes().to_vec();
        payload.extend_from_slice(pcm);
        let wire = self
            .tls
            .as_mut()
            .ok_or("no tls")?
            .encrypt(
                ch::MIC_INPUT,
                frame_flags::ENC_SIGNAL,
                media_msg::DATA,
                &payload,
            )
            .map_err(|e| e.to_string())?;
        self.write(&wire).await
    }

    async fn on_node_message(
        &mut self,
        ch: u8,
        flags: u8,
        msg_id: u16,
        payload: &[u8],
    ) -> Result<(), String> {
        let wire = if flags & frame_flags::ENCRYPTED != 0 {
            let Some(tls) = self.tls.as_mut() else {
                eprintln!(
                    "[aa-session] {}: encrypted message before tls, dropped",
                    self.peer.label
                );
                return Ok(());
            };
            tls.encrypt(ch, flags, msg_id, payload)
                .map_err(|e| e.to_string())?
        } else {
            frame::encode(ch, flags, msg_id, payload)
        };
        self.write(&wire).await
    }

    async fn on_control(&mut self, json: &str) -> bool {
        let Ok(v) = serde_json::from_str::<serde_json::Value>(json) else {
            return true;
        };
        match v.get("type").and_then(|t| t.as_str()) {
            Some("sink") => self.sinks.apply(&v),
            Some("end") => {
                self.closed_by_node = self.phase == Phase::Running;
                let _ = self.wr.shutdown().await;
            }
            Some("close") => {
                self.closed_by_node = self.phase == Phase::Running;
                return false;
            }
            _ => {}
        }
        true
    }
}

async fn listen_mic(listener: UnixListener, tx: mpsc::Sender<(u64, Vec<u8>)>) {
    loop {
        tokio::select! {
            _ = tx.closed() => return,
            conn = listener.accept() => match conn {
                Ok((sock, _)) => drop(tokio::spawn(read_mic(sock, tx.clone()))),
                Err(_) => return,
            },
        }
    }
}

async fn read_mic(mut sock: UnixStream, tx: mpsc::Sender<(u64, Vec<u8>)>) {
    let mut framer = feedproto::Framer::new();
    let mut buf = vec![0u8; 16384];
    loop {
        let n = tokio::select! {
            _ = tx.closed() => return,
            r = sock.read(&mut buf) => match r {
                Ok(0) | Err(_) => return,
                Ok(n) => n,
            },
        };
        framer.push(&buf[..n]);
        while let Some(record) = framer.next_record() {
            if record.kind == feedproto::KIND_MIC {
                let _ = tx.try_send((record.ts, record.payload));
            }
        }
    }
}

fn now_ns() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos() as u64)
        .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    async fn records(host: &mut UnixStream, n: usize) -> Vec<feedproto::Record> {
        let mut framer = feedproto::Framer::new();
        let mut got = Vec::new();
        let mut buf = [0u8; 4096];
        while got.len() < n {
            let k = tokio::time::timeout(Duration::from_secs(5), host.read(&mut buf))
                .await
                .expect("feed records")
                .unwrap();
            assert!(k > 0, "the feed closed");
            framer.push(&buf[..k]);
            while let Some(r) = framer.next_record() {
                got.push(r);
            }
        }
        got
    }

    // The host (the bridge) restarts in mid-session: at the next frame the feed is opened
    // again, the video stream announced to the new host, the frame delivered, and, once the
    // feed is connected, a keyframe asked for (once).
    #[tokio::test]
    async fn a_feed_the_host_closed_is_reopened() {
        let path = std::env::temp_dir()
            .join(format!("livi-revive-{}.sock", std::process::id()))
            .display()
            .to_string();
        let _ = std::fs::remove_file(&path);
        let listener = UnixListener::bind(&path).unwrap();
        let mut sinks = Sinks {
            reopen_after: Duration::ZERO,
            ..Default::default()
        };
        sinks.apply(&serde_json::json!({
            "type": "sink",
            "feed": path,
            "video": [{ "ch": ch::VIDEO, "id": 1, "codec": "h264" }],
        }));
        let (mut host, _) = tokio::time::timeout(Duration::from_secs(5), listener.accept())
            .await
            .unwrap()
            .unwrap();
        assert_eq!(records(&mut host, 1).await[0].kind, feedproto::KIND_VIDEO_START);
        assert!(!sinks.keyframe_wanted(), "the first feed needs no keyframe");

        drop(host);
        drop(listener);
        let _ = std::fs::remove_file(&path);
        tokio::time::timeout(Duration::from_secs(2), async {
            while !sinks.feed.as_ref().unwrap().1.closed() {
                tokio::time::sleep(Duration::from_millis(10)).await;
            }
        })
        .await
        .expect("the writer saw the host go");
        let listener = UnixListener::bind(&path).unwrap();
        sinks.push(ch::VIDEO, 7, b"frame");
        let (mut host, _) = tokio::time::timeout(Duration::from_secs(5), listener.accept())
            .await
            .expect("the feed was reopened")
            .unwrap();
        let got = records(&mut host, 2).await;
        assert_eq!((got[0].kind, got[0].id), (feedproto::KIND_VIDEO_START, 1));
        assert_eq!(
            (got[1].kind, got[1].ts, got[1].payload.as_slice()),
            (feedproto::KIND_VIDEO, 7, &b"frame"[..])
        );
        assert!(sinks.keyframe_wanted());
        assert!(!sinks.keyframe_wanted(), "asked once");
        let _ = std::fs::remove_file(&path);
    }

    // A closed feed is not reopened frame after frame: only once reopen_after has passed.
    #[tokio::test]
    async fn a_closed_feed_is_reopened_at_most_every_reopen_after() {
        let path = std::env::temp_dir()
            .join(format!("livi-revive-rate-{}.sock", std::process::id()))
            .display()
            .to_string();
        let _ = std::fs::remove_file(&path);
        let listener = UnixListener::bind(&path).unwrap();
        let mut sinks = Sinks::default();
        sinks.apply(&serde_json::json!({ "type": "sink", "feed": path }));
        let (host, _) = tokio::time::timeout(Duration::from_secs(5), listener.accept())
            .await
            .unwrap()
            .unwrap();
        drop(host);
        tokio::time::timeout(Duration::from_secs(2), async {
            while !sinks.feed.as_ref().unwrap().1.closed() {
                tokio::time::sleep(Duration::from_millis(10)).await;
            }
        })
        .await
        .unwrap();
        let opened = sinks.opened_at.unwrap();
        sinks.push(ch::VIDEO, 1, b"x");
        assert_eq!(sinks.opened_at, Some(opened), "within a second of the last open: left closed");
        sinks.opened_at = opened.checked_sub(FEED_REOPEN * 2);
        sinks.push(ch::VIDEO, 2, b"x");
        assert!(sinks.opened_at.unwrap() > opened, "reopened at the next frame");
        assert!(!sinks.feed.as_ref().unwrap().1.closed());
        let _ = std::fs::remove_file(&path);
    }

    // The phone takes the version request, then its end of the link closes; the main process
    // never closes the session. It must end on its own after the grace, telling the main
    // process why, instead of holding on (and holding the video feed) forever.
    #[tokio::test]
    async fn a_closed_phone_link_ends_the_session_after_the_grace() {
        let path = std::env::temp_dir()
            .join(format!("livi-eof-{}.sock", std::process::id()))
            .to_string_lossy()
            .into_owned();
        let _ = std::fs::remove_file(&path);
        let node = UnixListener::bind(&path).unwrap();
        let (mut phone, ours) = tokio::io::duplex(1 << 16);
        let peer = Peer {
            label: "test".into(),
            ip: IpAddr::V4(std::net::Ipv4Addr::LOCALHOST),
        };
        let session = tokio::spawn(run(ours, peer, node, path.clone(), Arc::new(Notify::new())));
        let mut main = UnixStream::connect(&path).await.unwrap();
        let mut version = [0u8; 64];
        assert!(phone.read(&mut version).await.unwrap() > 0);
        drop(phone);

        let end = tokio::time::timeout(Duration::from_secs(10), session)
            .await
            .expect("the session ended")
            .unwrap();
        assert!(!end.running);
        let mut got = Vec::new();
        let _ = tokio::time::timeout(Duration::from_secs(1), main.read_to_end(&mut got)).await;
        let text = String::from_utf8_lossy(&got);
        assert!(text.contains("\"eof\""), "{text}");
        assert!(text.contains("phone closed the link"), "{text}");
    }

    #[test]
    fn the_phone_ping_wishes_come_out_of_the_version_response() {
        let ping = [
            0x08, 0xc0, 0x3e, 0x10, 0xe8, 0x07, 0x18, 0xc8, 0x01, 0x20, 0x05,
        ];
        let connection = [&[0x0a, ping.len() as u8][..], &ping].concat();
        let options = [&[0x0a, connection.len() as u8][..], &connection].concat();
        assert_eq!(
            ping_config(&options),
            Some(PingConfig {
                interval_ms: 1000,
                timeout_ms: 8000
            })
        );
        assert_eq!(ping_config(&[]), None);
        assert_eq!(ping_config(&[0x0a, 0x02, 0x0a, 0x00]), None);
    }
}
