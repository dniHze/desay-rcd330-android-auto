// SPDX-License-Identifier: GPL-3.0-or-later
//
// The GENIVI ILM control library on the unit (/usr/lib/libilmControl.so.1.9.1, ilm_init in its
// libilmCommon), loaded at run time: SVP_iLMService::ShowSurface sets visibility, opacity and
// a destination rectangle (the surface's own size), but never the source rectangle. The
// decoder's buffers are padded (896x576 for an 800x480 picture, which starts at 32,24 in the
// sink's surface: gst-ducati's PADX/PADY, and the step 3 screen captures), so without these the
// picture is off-centre with a green edge and its bottom cut off.
//
// The shim is a static musl binary, which cannot load the unit's glibc libraries ("Dynamic
// loading not supported"), so the bridge sets them: a "crop" datagram to the running bridge's
// socket (AA_BRIDGE_CTL), which holds the library loaded and connected and answers in a few
// milliseconds, sent before ShowSurface (the source rectangle, which ShowSurface leaves alone,
// is set while the surface is still hidden) and after it (the destination it resets). Without
// that socket, `aa-bridge --ilm-crop <id> <source> <destination>` (aa-bridge/src/ilm_crop.cpp):
// a process per call, slow enough on the unit (rs1) to show the uncropped picture, its green
// left column, for a few frames. AA_ILM_HELPER
// names it (default: aa-bridge beside this binary; "-" for none). As the C++ shim tried the
// library once: when the library is missing, or ilm_init fails, it is said once and not tried
// again.

use std::io::Read;
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use crate::scan::Scan;
use crate::{logi, logv, logw};

/// How long the helper may take (it connects to the compositor, sets, commits and exits).
const HELPER_TIMEOUT: Duration = Duration::from_secs(3);

pub struct IlmControl {
    helper: Option<PathBuf>,
    /// The running bridge's crop socket (AA_BRIDGE_CTL, aa-bridge/src/ilm_crop.cpp).
    bridge_sock: String,
}

impl Default for IlmControl {
    fn default() -> Self {
        let helper = match std::env::var("AA_ILM_HELPER") {
            Ok(v) if v == "-" => None,
            Ok(v) if !v.is_empty() => Some(PathBuf::from(v)),
            _ => std::env::current_exe().ok().and_then(|e| e.parent().map(|d| d.join("aa-bridge"))),
        };
        let bridge_sock = std::env::var("AA_BRIDGE_CTL").ok().filter(|v| !v.is_empty())
            .unwrap_or_else(|| "/tmp/run/aa/bridge.sock".into());
        IlmControl { helper, bridge_sock }
    }
}

impl IlmControl {
    /// "x,y,w,h" -> rectangle; None for "-" or anything malformed.
    pub fn parse(s: &str) -> Option<[i32; 4]> {
        let mut sc = Scan::new(s);
        let x = sc.int()?;
        sc.lit(",")?;
        let y = sc.int()?;
        sc.lit(",")?;
        let w = sc.int()?;
        sc.lit(",")?;
        let h = sc.int()?;
        (w > 0 && h > 0).then_some([x, y, w, h])
    }

    /// Sets the rectangles and commits, through the helper; says once if it cannot.
    pub fn set_regions(&mut self, id: u32, source: &str, dest: &str) {
        if Self::parse(source).is_none() && Self::parse(dest).is_none() {
            return;
        }
        // The running bridge first: its crop socket answers in a few milliseconds.
        if let Ok(sock) = std::os::unix::net::UnixDatagram::unbound() {
            let msg = format!("crop {id} {source} {dest}");
            if sock.send_to(msg.as_bytes(), &self.bridge_sock).is_ok() {
                logv!("ILM control: surface {} source {}, destination {}: to the bridge", id, source, dest);
                return;
            }
        }
        let Some(helper) = self.helper.clone() else {
            return;
        };
        match run(&helper, id, source, dest) {
            Ok((0, line)) => logv!("{}", line),
            Ok((3, line)) => {
                logi!("{}", line); // "ILM control: ilm_init -> <r>"
                self.helper = None;
            }
            Ok((_, line)) => {
                logw!("{}", line); // the library is missing or lacks the functions
                self.helper = None;
            }
            Err(e) => {
                logw!("ILM control: libilmControl not available (helper {}: {}); surface rectangles left as they are",
                      helper.display(), e);
                self.helper = None;
            }
        }
    }
}

/// Runs the helper; its exit code and its line.
fn run(helper: &PathBuf, id: u32, source: &str, dest: &str) -> std::io::Result<(i32, String)> {
    let mut child = Command::new(helper)
        .args(["--ilm-crop", &id.to_string(), source, dest])
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()?;
    let start = Instant::now();
    let status = loop {
        if let Some(st) = child.try_wait()? {
            break st;
        }
        if start.elapsed() > HELPER_TIMEOUT {
            let _ = child.kill();
            let _ = child.wait();
            return Err(std::io::Error::new(std::io::ErrorKind::TimedOut, "no answer within 3 s"));
        }
        std::thread::sleep(Duration::from_millis(20));
    };
    let mut out = String::new();
    if let Some(mut o) = child.stdout.take() {
        let _ = o.read_to_string(&mut out);
    }
    let line = out.lines().next().unwrap_or("").to_string();
    Ok((status.code().unwrap_or(-1), line))
}

#[cfg(test)]
mod tests {
    use super::IlmControl;

    #[test]
    fn rectangles_parse_as_sscanf_reads_them() {
        assert_eq!(IlmControl::parse("32,24,800,480"), Some([32, 24, 800, 480]));
        assert_eq!(IlmControl::parse(" 0,0,800,480"), Some([0, 0, 800, 480]));
        assert_eq!(IlmControl::parse("-"), None);
        assert_eq!(IlmControl::parse("0,0,0,480"), None);
        assert_eq!(IlmControl::parse("0, 0,800,480x"), Some([0, 0, 800, 480]));
        assert_eq!(IlmControl::parse("0 ,0,800,480"), None);
    }
}
