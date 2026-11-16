// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bridge's notify socket (AA_SHIM_NOTIFY, datagrams of one line each):
//   * "open <stream name>": the volume of the prompt streams (audio_idl.hpp). audiomgr sets a
//     CarLife source's volume on the "sv-as-carlife-prompt" streams that exist when the source
//     becomes active, and stock carlifeservice registers each stream it opens (RegisterAudio
//     right after pa_simple_new), which gives a stream opened while CL_NAVI is active the
//     navigation volume. The bridge opens our streams, so it sends this each time, and this
//     calls RegisterAudio for it. Without it, a stream opened after the source was taken started
//     at PulseAudio's default, full volume (the assistant's first chime of each boot: CL_NAVI is
//     taken when the mic opens, before the chime's stream exists). Not with AA_AUDIO_PORT=0;
//   * "video": the bridge rebuilt its video pipeline in place (after a video error or stall, the
//     same process) and the new one has its first frame. waylandsink2 then makes its ILM surface
//     again, with the same id, hidden: the surface is shown again if the screen is up (as for a
//     new session; nothing to do while it is not, the next Show(1) shows it).
// Anything else is ignored, so a newer bridge can say more.

use std::os::unix::fs::PermissionsExt;
use std::os::unix::net::UnixDatagram;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::Duration;

use rcf_lite::client::args;
use rcf_lite::{Client, Reader};

use crate::config::{dir_name, errstr, mkdirs, Config};
use crate::idl::{self, audio};
use crate::log::printable;
use crate::{loge, logi, logv, logw};

pub struct BridgeNotify {
    cfg: Arc<Config>,
    on_video: Box<dyn Fn() + Send + Sync>,
    stop: AtomicBool,
    worker: Mutex<Option<JoinHandle<()>>>,
}

impl BridgeNotify {
    pub fn new(cfg: Arc<Config>, on_video: Box<dyn Fn() + Send + Sync>) -> Arc<Self> {
        let me = Arc::new(BridgeNotify { cfg, on_video, stop: AtomicBool::new(false), worker: Mutex::new(None) });
        if me.cfg.notify_sock != "-" {
            let w = me.clone();
            *me.worker.lock().unwrap() = Some(std::thread::spawn(move || w.run()));
        }
        me
    }

    pub fn stop(&self) {
        self.stop.store(true, Ordering::SeqCst);
        if let Some(t) = self.worker.lock().unwrap().take() {
            let _ = t.join();
        }
    }

    fn run(&self) {
        let path = &self.cfg.notify_sock;
        if path.len() >= 108 {
            loge!("bridge: notify socket {}: {}", path, "path too long");
            return;
        }
        let dir = dir_name(path);
        if !dir.is_empty() {
            let _ = mkdirs(&dir, 0o755);
        }
        let _ = std::fs::remove_file(path);
        let sock = match UnixDatagram::bind(path) {
            Ok(s) => s,
            Err(e) => {
                loge!("bridge: notify socket {}: {}", path, errstr(&e));
                return;
            }
        };
        // the bridge runs as root, as every service on the unit
        let _ = std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600));
        logi!(
            "bridge: notify socket {} (prompt streams{}, video rebuilds)",
            path,
            if self.cfg.audio_port > 0 { "" } else { " off" }
        );
        let _ = sock.set_read_timeout(Some(Duration::from_millis(300)));
        let mut client: Option<Client> = None;
        let mut failing = false;
        while !self.stop.load(Ordering::SeqCst) {
            let mut buf = [0u8; 127];
            let n = match sock.recv(&mut buf) {
                Ok(n) if n > 0 => n,
                _ => continue,
            };
            // As a C string: up to the first NUL.
            let raw = &buf[..n];
            let raw = &raw[..raw.iter().position(|&c| c == 0).unwrap_or(raw.len())];
            let mut msg = raw.to_vec();
            while matches!(msg.last(), Some(b'\n') | Some(b'\r')) {
                msg.pop();
            }
            if msg == b"video" {
                logi!("bridge: video pipeline rebuilt; its surface is shown again if the screen is up");
                (self.on_video)();
                continue;
            }
            if !msg.starts_with(b"open ") || self.cfg.audio_port <= 0 {
                logv!("bridge: notify \"{}\" ignored", printable(&msg));
                continue;
            }
            let name = String::from_utf8_lossy(&msg[5..]).into_owned();
            if !name.starts_with("sv-as-") {
                continue; // only the platform's prompt streams
            }
            let c = client.get_or_insert_with(|| {
                let mut c = Client::new(&self.cfg.audio_host, self.cfg.audio_port as u16, audio::AUDIO_SERVICE);
                c.set_connect_timeout(Duration::from_millis(1000));
                c.set_call_timeout(Duration::from_millis(2000));
                c
            });
            let r = c
                .call(audio::AUDIO_SERVICE_REGISTER_AUDIO, &args(|a| {
                    a.put(&name).put(&idl::REGISTER_ALL_PIDS);
                }))
                .and_then(|b| Reader::new(&b).get::<i32>());
            match r {
                Ok(r) => {
                    logi!("audio: RegisterAudio({}) -> {}", name, r);
                    failing = false;
                }
                Err(e) => {
                    if !failing {
                        logw!(
                            "audio: RegisterAudio({}) on {}:{} failed: {}",
                            name,
                            self.cfg.audio_host,
                            self.cfg.audio_port,
                            errstr(&e)
                        );
                    }
                    failing = true;
                    client = None;
                }
            }
        }
        drop(sock);
        let _ = std::fs::remove_file(path);
    }
}
