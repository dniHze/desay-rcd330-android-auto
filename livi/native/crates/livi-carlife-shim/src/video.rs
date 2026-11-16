// SPDX-License-Identifier: GPL-3.0-or-later
//
// The video surface. appconnect calls Show(1) when its CarLife screen opens and Show(0) when
// it leaves; stock carlifeservice answers with SVP_iLMService::ShowSurface(getpid() +
// 20000000, 1 / 0) on its own video surface, and the ILM service sets visibility and opacity
// and places the video. Here the video is drawn by the bridge, whose waylandsink creates its
// ILM surface with the same convention, so the id is 20000000 + the bridge's pid. The calls
// run on a worker: WaitSurface blocks until the surface exists (it appears after the first
// decoded frame), and Show() itself must return at once. While the screen is up the worker
// looks for the bridge every 2 s: a bridge restarted by procguard (a new pid, so a new surface),
// or one that was not running yet, is shown without waiting for the next Show(1).

use std::io;
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::Duration;

use rcf_lite::{Client, Reader};

use crate::config::{errstr, Config};
use crate::control::read_comm;
use crate::idl::{self, ilm};
use crate::ilm::IlmControl;
use crate::{appc, logi, logv, logw};

struct State {
    want: bool,
    dirty: bool,
    stop: bool,
}

pub struct VideoSurface {
    cfg: Arc<Config>,
    state: Mutex<State>,
    cv: Condvar,
    worker: Mutex<Option<JoinHandle<()>>>,
}

impl VideoSurface {
    pub fn new(cfg: Arc<Config>) -> Arc<Self> {
        let me = Arc::new(VideoSurface {
            cfg,
            state: Mutex::new(State { want: false, dirty: false, stop: false }),
            cv: Condvar::new(),
            worker: Mutex::new(None),
        });
        if me.cfg.ilm_port > 0 {
            let w = me.clone();
            *me.worker.lock().unwrap() = Some(std::thread::spawn(move || w.run()));
        }
        me
    }

    pub fn stop(&self) {
        self.state.lock().unwrap().stop = true;
        self.cv.notify_all();
        if let Some(t) = self.worker.lock().unwrap().take() {
            let _ = t.join();
        }
    }

    /// From Show(n).
    pub fn show(&self, on: bool) {
        self.request(on, true);
    }

    /// A new AA session means a new pipeline and a new surface, and so does the bridge's video
    /// rebuild (BridgeNotify "video"): show it again if the screen is up.
    pub fn new_session(&self) {
        self.request(false, false);
    }

    fn request(&self, on: bool, set: bool) {
        {
            let mut st = self.state.lock().unwrap();
            if set {
                st.want = on;
            } else if !st.want {
                return;
            }
            st.dirty = true;
        }
        self.cv.notify_all();
    }

    // 20000000 + the pid of the newest process whose name is in bridge_comm (a bridge being
    // replaced may still be there); 0 if none runs.
    fn find_surface(&self) -> u32 {
        let Ok(d) = std::fs::read_dir("/proc") else { return 0 };
        let names = format!(",{},", self.cfg.bridge_comm);
        let mut best: i64 = 0;
        let mut best_start: u64 = 0;
        for e in d.flatten() {
            let Some(pid) = proc_pid(&e) else { continue };
            let comm = read_comm(pid);
            if comm.is_empty() || !names.contains(&format!(",{comm},")) {
                continue;
            }
            let start = appc::start_time(pid as i32);
            if best == 0 || start > best_start {
                best = pid;
                best_start = start;
            }
        }
        if best != 0 { idl::ILM_VIDEO_SURFACE_BASE.wrapping_add(best as u32) } else { 0 }
    }

    fn run(&self) {
        let mut control = IlmControl::default(); // used from the worker thread only
        let mut shown: u32 = 0; // the surface last shown; 0 while hidden
        let mut failed: u32 = 0; // the surface whose calls failed, tried again up to 3 times
        let mut failures = 0;
        loop {
            let (on, asked);
            {
                let mut st = self.state.lock().unwrap();
                if st.want {
                    st = self.cv.wait_timeout_while(st, Duration::from_secs(2), |s| !s.stop && !s.dirty).unwrap().0;
                } else {
                    st = self.cv.wait_while(st, |s| !s.stop && !s.dirty).unwrap();
                }
                if st.stop {
                    return;
                }
                asked = st.dirty;
                st.dirty = false;
                on = st.want;
            }
            let id = self.find_surface();
            if !asked {
                // The look every 2 s while shown: only a surface not shown yet needs anything.
                if !on || id == 0 || id == shown || (id == failed && failures >= 3) {
                    continue;
                }
                logi!("ILM: the bridge's surface is now {} (the bridge started or restarted)", id);
            }
            if id == 0 {
                logw!(
                    "ILM: no video bridge running ({}), cannot {} its surface{}",
                    self.cfg.bridge_comm,
                    if on { "show" } else { "hide" },
                    if on { " yet" } else { "" }
                );
                shown = 0;
                continue;
            }
            if let Err(e) = self.show_surface(&mut control, id, on, &mut shown, &mut failures) {
                logw!("ILM: {} surface {} failed: {}", if on { "showing" } else { "hiding" }, id, errstr(&e));
                shown = 0; // the next look tries again, up to 3 times for this surface
                if id != failed {
                    failures = 0;
                }
                failed = id;
                failures += 1;
            }
        }
    }

    fn show_surface(&self, control: &mut IlmControl, id: u32, on: bool, shown: &mut u32, failures: &mut i32)
        -> io::Result<()>
    {
        let mut ilm = Client::new(&self.cfg.ilm_host, self.cfg.ilm_port as u16, ilm::ILM_SERVICE);
        if on {
            ilm.set_call_timeout(Duration::from_millis(15000));
            let w = wait_surface(&mut ilm, id)?;
            logv!("ILM: WaitSurface({}) -> {}", id, w);
        }
        ilm.set_call_timeout(Duration::from_millis(5000));
        if on {
            // The source rectangle while the surface is still hidden: no uncropped frame.
            control.set_regions(id, &self.cfg.video_source, &self.cfg.video_dest);
        }
        let r = show(&mut ilm, id, on)?;
        logi!("ILM: ShowSurface({}, {}) -> {}", id, on as i32, r);
        *shown = if on { id } else { 0 };
        *failures = 0;
        if on {
            control.set_regions(id, &self.cfg.video_source, &self.cfg.video_dest);
        }
        // Show(1) comes just before the surface exists (the sink creates it after the first
        // frame). In case WaitSurface does not wait for a surface it has not seen, show it once
        // more a little later, unless something new was asked meanwhile.
        if on {
            let st = self.state.lock().unwrap();
            let (st, _) = self.cv.wait_timeout_while(st, Duration::from_secs(3), |s| !s.stop && !s.dirty).unwrap();
            let asked = st.stop || st.dirty;
            drop(st);
            if !asked {
                if self.find_surface() != id {
                    return Ok(()); // the bridge was replaced: the next look shows it
                }
                let r = show(&mut ilm, id, true)?;
                logv!("ILM: ShowSurface({}, 1) again -> {}", id, r);
                control.set_regions(id, &self.cfg.video_source, &self.cfg.video_dest);
            }
        }
        Ok(())
    }
}

/// The pid a /proc entry names (strtol of its whole name, > 0).
pub fn proc_pid(e: &std::fs::DirEntry) -> Option<i64> {
    let name = e.file_name();
    let name = name.to_str()?;
    crate::scan::strtol_whole(name).filter(|&p| p > 0)
}

fn wait_surface(c: &mut Client, id: u32) -> io::Result<i32> {
    let body = c.call(ilm::ILM_SERVICE_WAIT_SURFACE, &rcf_lite::client::args(|w| {
        w.put(&id);
    }))?;
    Reader::new(&body).get()
}

fn show(c: &mut Client, id: u32, on: bool) -> io::Result<i32> {
    let body = c.call(ilm::ILM_SERVICE_SHOW_SURFACE, &rcf_lite::client::args(|w| {
        w.put(&id).put(&(on as i32));
    }))?;
    Reader::new(&body).get()
}
