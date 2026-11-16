//! Live checks against the C++ RCF peers (aa-carlife-shim's fake_can and cl_probe):
//!   rcf-interop can <port>              SVP_CanService.GetCanCarplayInfo (fnId 3)
//!   rcf-interop can-sub <port> <n>      the first n SVP_CanListener events
//!   rcf-interop carlife <svc> <pub>     serve ISVP_CarLifeService and publish ISVP_CarLifeListener

use std::io;
use std::net::TcpListener;
use std::sync::atomic::{AtomicI32, Ordering};
use std::sync::{mpsc, Arc};
use std::time::Duration;

use rcf_lite::client::args;
use rcf_lite::{sf_struct, subscribe, Client, Publisher, Reader, Server, Service, Writer};

sf_struct!(CarplayInfo { day_night: u8, transmission: u8, right_hand_drive: u8 });

struct CarLife {
    status: AtomicI32,
    listener: Arc<Publisher>,
}

impl Service for CarLife {
    fn interface(&self) -> &str {
        "ISVP_CarLifeService"
    }
    fn call(&self, fn_id: i32, a: &mut Reader, out: &mut Writer) -> io::Result<()> {
        match fn_id {
            4 => {
                // CarLifeConnection: connected, video (3), told to every subscriber.
                println!("CarLifeConnection()");
                self.status.store(3, Ordering::SeqCst);
                let n = self.listener.publish(1, &args(|w| {
                    w.put(&3i32).put(&0i32);
                }));
                println!("OnCarLifeConnectStatusChange(3, 0) to {n} subscriber(s)");
                out.put(&0i32);
            }
            6 => {
                let (x, y, ev): (u32, u32, i32) = (a.get()?, a.get()?, a.get()?);
                println!("SingleTouchInfo({x}, {y}, {ev})");
            }
            8 => {
                println!("GetCarLifeConnectStatus() = {}", self.status.load(Ordering::SeqCst));
                out.put(&self.status.load(Ordering::SeqCst));
            }
            _ => {
                println!("fnId {fn_id}: not served by this test");
                return Err(io::Error::other("not in this test"));
            }
        }
        Ok(())
    }
}

fn main() -> io::Result<()> {
    let mut a: Vec<String> = std::env::args().collect();
    // In the emulator's CarLife slot, in the shim's place: the launcher runs
    // `aa-carlife-shim --debug-output console` (serve), and `--log-relay <tag>` (pass lines on).
    match a.get(1).map(String::as_str) {
        Some("--debug-output") => a = vec![a[0].clone(), "carlife".into(), "30407".into(), "30408".into()],
        Some("--log-relay") => {
            io::copy(&mut io::stdin(), &mut io::stdout())?;
            return Ok(());
        }
        _ => {}
    }
    let t = Duration::from_secs(3);
    match a.get(1).map(String::as_str) {
        Some("can") => {
            let mut c = Client::connect(("127.0.0.1", a[2].parse::<u16>().unwrap()), "SVP_CanService", t)?;
            let body = c.call(3, &args(|w| {
                w.put(&CarplayInfo::default());
            }))?;
            println!("GetCanCarplayInfo -> {:?}", Reader::new(&body).get::<CarplayInfo>()?);
        }
        Some("can-sub") => {
            let n: usize = a[3].parse().unwrap();
            let (tx, rx) = mpsc::channel();
            subscribe(("127.0.0.1", a[2].parse::<u16>().unwrap()), "SVP_CanListener", t, move |f, args| {
                let _ = tx.send((f, args.to_vec()));
            })?;
            for _ in 0..n {
                let (f, args) = rx.recv_timeout(Duration::from_secs(5)).map_err(io::Error::other)?;
                println!("SVP_CanListener fnId {f}: {} bytes, u16 {:?}", args.len(), Reader::new(&args).get::<u16>().ok());
            }
        }
        Some("carlife") => {
            let listener = Publisher::new("ISVP_CarLifeListener");
            let svc = Arc::new(CarLife { status: AtomicI32::new(0), listener: listener.clone() });
            let s = Arc::new(Server::new().bind(svc));
            let p = Arc::new(Server::new().publish(listener));
            let ls = TcpListener::bind(("0.0.0.0", a[2].parse::<u16>().unwrap()))?;
            let lp = TcpListener::bind(("127.0.0.1", a[3].parse::<u16>().unwrap()))?;
            std::fs::create_dir_all("/tmp/run/service/registry")?;
            std::fs::write("/tmp/run/service/registry/svp.svc.carlife", b"")?;
            println!("serving");
            std::thread::spawn(move || p.run(lp));
            s.run(ls)?;
        }
        _ => eprintln!("usage: rcf-interop can <port> | can-sub <port> <n> | carlife <svc> <pub>"),
    }
    Ok(())
}
