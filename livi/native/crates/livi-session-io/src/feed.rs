// SPDX-FileCopyrightText: 2025-2026 Lasse Heitgres
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional terms: see NOTICE

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::UnixStream;
use tokio::sync::mpsc;

const QUEUE: usize = 512;
const CONNECT_ATTEMPTS: u32 = 40;
const CONNECT_RETRY: Duration = Duration::from_millis(250);

pub struct FeedWriter {
    tx: mpsc::Sender<Vec<u8>>,
    /// The connection to the host was made (it may have closed since).
    connected: Arc<AtomicBool>,
}

impl FeedWriter {
    pub fn open(path: String) -> Self {
        let (tx, mut rx) = mpsc::channel::<Vec<u8>>(QUEUE);
        let connected = Arc::new(AtomicBool::new(false));
        let up = connected.clone();
        tokio::spawn(async move {
            // The host may still be starting (or restarting) when the session configures its
            // sink; keep trying for a while instead of losing the feed for the whole session.
            let mut attempt = 0;
            let sock = loop {
                match UnixStream::connect(&path).await {
                    Ok(s) => break s,
                    Err(e) if attempt < CONNECT_ATTEMPTS => {
                        if attempt == 0 {
                            eprintln!("[aa-feed] connect {path}: {e}, retrying");
                        }
                        attempt += 1;
                        tokio::time::sleep(CONNECT_RETRY).await;
                    }
                    Err(e) => {
                        eprintln!("[aa-feed] connect {path}: {e}");
                        return;
                    }
                }
            };
            up.store(true, Ordering::Relaxed);
            let (mut rd, mut wr) = sock.into_split();
            let mut probe = [0u8; 1];
            loop {
                tokio::select! {
                    record = rx.recv() => {
                        let Some(record) = record else { return };
                        if let Err(e) = wr.write_all(&record).await {
                            eprintln!("[aa-feed] write: {e}");
                            return;
                        }
                    }
                    _ = rd.read(&mut probe) => {
                        eprintln!("[aa-feed] the host closed {path}");
                        return;
                    }
                }
            }
        });
        Self { tx, connected }
    }

    pub fn send(&self, record: Vec<u8>) -> bool {
        self.tx.try_send(record).is_ok()
    }

    /// The writer is done: the host closed the feed, a write failed, or it never connected.
    pub fn closed(&self) -> bool {
        self.tx.is_closed()
    }

    /// The writer reached the host (records queued before were delivered first). Stays true
    /// after the connection closed.
    pub fn has_connected(&self) -> bool {
        self.connected.load(Ordering::Relaxed)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;
    use tokio::io::AsyncReadExt;
    use tokio::net::UnixListener;

    #[tokio::test]
    async fn the_writer_closes_when_the_host_goes() {
        let path = std::env::temp_dir().join(format!("livi-feed-{}.sock", std::process::id()));
        let _ = std::fs::remove_file(&path);
        let listener = UnixListener::bind(&path).unwrap();
        let writer = FeedWriter::open(path.display().to_string());
        let (mut host, _) = listener.accept().await.unwrap();
        assert!(writer.send(b"rec".to_vec()));
        let mut got = [0u8; 3];
        host.read_exact(&mut got).await.unwrap();
        assert_eq!(&got, b"rec");
        assert!(!writer.closed());
        drop(host);
        tokio::time::timeout(Duration::from_secs(2), async {
            while !writer.closed() {
                tokio::time::sleep(Duration::from_millis(10)).await;
            }
        })
        .await
        .unwrap();
        assert!(writer.has_connected(), "it did reach the host before");
        let _ = std::fs::remove_file(&path);
    }

    #[tokio::test]
    async fn the_writer_waits_for_a_late_host() {
        let path =
            std::env::temp_dir().join(format!("livi-feed-late-{}.sock", std::process::id()));
        let _ = std::fs::remove_file(&path);
        let writer = FeedWriter::open(path.display().to_string());
        assert!(writer.send(b"rec".to_vec()));
        tokio::time::sleep(Duration::from_millis(600)).await;
        assert!(!writer.has_connected() && !writer.closed());
        let listener = UnixListener::bind(&path).unwrap();
        let (mut host, _) = tokio::time::timeout(Duration::from_secs(5), listener.accept())
            .await
            .unwrap()
            .unwrap();
        let mut got = [0u8; 3];
        host.read_exact(&mut got).await.unwrap();
        assert_eq!(&got, b"rec");
        assert!(writer.has_connected());
        let _ = std::fs::remove_file(&path);
    }
}
