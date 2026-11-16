//! Serving RCF interfaces, and publishing listener interfaces (blocking, a thread per connection).

use std::io;
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, Shutdown, SocketAddr, TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use crate::msg::{self, Request, Response, FN_PING};
use crate::sf::{Reader, Writer};
use crate::wire::{read_frame, write_frame};

/// RcfError_UnknownInterface: the call names no interface this server binds.
const ERROR_UNKNOWN_INTERFACE: i32 = 38;

/// How long a publish may wait on one subscriber's full socket before that subscriber is
/// dropped (RCF queues its publishes instead; a subscriber that stops reading must not stall
/// the publisher).
const PUBLISH_WRITE_TIMEOUT: Duration = Duration::from_secs(2);

/// An interface a server binds (RcfServer::bind<I>).
pub trait Service: Send + Sync {
    fn interface(&self) -> &str;
    /// Reads the arguments and writes the return value (nothing for void), then every
    /// by-reference argument. An Err (an unknown fnId, a bad argument) answers a server error.
    fn call(&self, fn_id: i32, args: &mut Reader, out: &mut Writer) -> io::Result<()>;
}

type OnSubscribe = Box<dyn Fn() + Send + Sync>;

/// The publisher of a listener interface (PublishingService::createPublisher<I>).
pub struct Publisher {
    name: String,
    subscribers: Mutex<Vec<(u64, TcpStream)>>,
    on_subscribe: Option<OnSubscribe>,
}

impl Publisher {
    pub fn new(listener: &str) -> Arc<Self> {
        Arc::new(Publisher { name: listener.into(), subscribers: Mutex::new(Vec::new()), on_subscribe: None })
    }

    /// With PublisherParms::setOnSubscriberConnect: `on_subscribe` runs for each new subscriber,
    /// on its connection's thread, before the subscriber is added (a publish from it would not
    /// reach the newcomer).
    pub fn with_on_subscribe(listener: &str, on_subscribe: impl Fn() + Send + Sync + 'static) -> Arc<Self> {
        Arc::new(Publisher {
            name: listener.into(),
            subscribers: Mutex::new(Vec::new()),
            on_subscribe: Some(Box::new(on_subscribe)),
        })
    }

    /// Sends a one-way call to every subscriber; one whose connection fails is dropped. Returns
    /// how many it reached.
    pub fn publish(&self, fn_id: i32, args: &[u8]) -> usize {
        let req = Request { subinterface: self.name.clone(), ..Request::call("", fn_id, true) };
        let msg = req.encode(args);
        let mut subs = self.subscribers.lock().unwrap();
        subs.retain_mut(|(_, s)| {
            let ok = write_frame(s, &msg).is_ok();
            if !ok {
                let _ = s.shutdown(Shutdown::Both);
            }
            ok
        });
        subs.len()
    }

    pub fn subscribers(&self) -> usize {
        self.subscribers.lock().unwrap().len()
    }

    fn remove(&self, id: u64) {
        self.subscribers.lock().unwrap().retain(|(i, _)| *i != id);
    }
}

/// The interfaces and publishers one listening port serves.
#[derive(Default)]
pub struct Server {
    services: Vec<Arc<dyn Service>>,
    publishers: Vec<Arc<Publisher>>,
    sessions: Mutex<Vec<(u64, TcpStream)>>,
    next_id: AtomicU64,
    stopped: AtomicBool,
}

/// One connection being served (a session, or a callback connection): close() ends it.
pub struct Connection {
    stream: Option<TcpStream>,
}

impl Connection {
    pub fn close(&self) {
        if let Some(s) = &self.stream {
            let _ = s.shutdown(Shutdown::Both);
        }
    }
}

/// A server started with Server::start: stop() (or dropping it) closes the listening socket and
/// every connection it accepted (RcfServer::stop).
pub struct ServerHandle {
    server: Arc<Server>,
    addr: SocketAddr,
    thread: Option<thread::JoinHandle<io::Result<()>>>,
}

impl ServerHandle {
    pub fn local_addr(&self) -> SocketAddr {
        self.addr
    }

    pub fn stop(&mut self) {
        let Some(t) = self.thread.take() else { return };
        self.server.stopped.store(true, Ordering::SeqCst);
        // Wake the accept: it sees the flag and drops the listener.
        let mut a = self.addr;
        if a.ip().is_unspecified() {
            a.set_ip(if a.is_ipv4() { IpAddr::V4(Ipv4Addr::LOCALHOST) } else { IpAddr::V6(Ipv6Addr::LOCALHOST) });
        }
        let _ = TcpStream::connect_timeout(&a, Duration::from_secs(1));
        let _ = t.join();
        for (_, s) in self.server.sessions.lock().unwrap().drain(..) {
            let _ = s.shutdown(Shutdown::Both);
        }
    }
}

impl Drop for ServerHandle {
    fn drop(&mut self) {
        self.stop();
    }
}

impl Server {
    pub fn new() -> Self {
        Server::default()
    }

    pub fn bind(mut self, s: Arc<dyn Service>) -> Self {
        self.services.push(s);
        self
    }

    pub fn publish(mut self, p: Arc<Publisher>) -> Self {
        self.publishers.push(p);
        self
    }

    /// Accepts connections, each on its own thread, until the server is stopped.
    pub fn run(self: Arc<Self>, listener: TcpListener) -> io::Result<()> {
        for conn in listener.incoming() {
            if self.stopped.load(Ordering::SeqCst) {
                break;
            }
            match conn {
                Ok(s) => {
                    self.serve(s);
                }
                // Out of descriptors and the like: RCF's acceptor keeps going.
                Err(_) => thread::sleep(Duration::from_millis(10)),
            }
        }
        Ok(())
    }

    /// Runs the server on a thread of its own (RcfServer::start on a listening socket).
    pub fn start(self: Arc<Self>, listener: TcpListener) -> io::Result<ServerHandle> {
        let addr = listener.local_addr()?;
        let me = self.clone();
        let thread = thread::spawn(move || me.run(listener));
        Ok(ServerHandle { server: self, addr, thread: Some(thread) })
    }

    /// Serves one connection on a thread of its own: an accepted one, or a client connection
    /// turned around (Client::create_callback_connection).
    pub fn serve(self: &Arc<Self>, s: TcpStream) -> Connection {
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        if let Ok(c) = s.try_clone() {
            self.sessions.lock().unwrap().push((id, c));
        }
        let handle = s.try_clone().ok();
        let me = self.clone();
        thread::spawn(move || {
            let _ = me.session(id, s);
            me.sessions.lock().unwrap().retain(|(i, _)| *i != id);
            for p in &me.publishers {
                p.remove(id);
            }
        });
        Connection { stream: handle }
    }

    fn session(&self, id: u64, mut s: TcpStream) -> io::Result<()> {
        s.set_nodelay(true)?;
        loop {
            let msg = read_frame(&mut s)?;
            let (req, args) = Request::decode(&msg)?;
            if req.fn_id == FN_PING {
                let mut resp = Response::default();
                let listener = if req.oob.is_empty() { None } else { msg::parse_oob_subscribe(&req.oob)? };
                if let Some(name) = listener {
                    let p = self.publishers.iter().find(|p| p.name == name);
                    resp.oob = msg::oob_subscribed(if p.is_some() { 0 } else { ERROR_UNKNOWN_INTERFACE });
                    write_frame(&mut s, &resp.encode(&[]))?;
                    if let Some(p) = p {
                        if let Some(cb) = &p.on_subscribe {
                            cb();
                        }
                        let sub = s.try_clone()?;
                        sub.set_write_timeout(Some(PUBLISH_WRITE_TIMEOUT))?;
                        p.subscribers.lock().unwrap().push((id, sub));
                    }
                } else {
                    write_frame(&mut s, &resp.encode(&[]))?;
                }
                continue;
            }
            let reply = match self.services.iter().find(|x| x.interface() == req.interface()) {
                Some(svc) => {
                    let mut out = Writer::new();
                    match svc.call(req.fn_id, &mut Reader::new(args), &mut out) {
                        Ok(()) => Response::default().encode(&out.buf),
                        Err(_) => msg::server_error(msg::ERROR_NO_SERVER_STUB),
                    }
                }
                None => msg::server_error(ERROR_UNKNOWN_INTERFACE),
            };
            if !req.oneway {
                write_frame(&mut s, &reply)?;
            }
            if req.close {
                return Ok(());
            }
        }
    }
}
