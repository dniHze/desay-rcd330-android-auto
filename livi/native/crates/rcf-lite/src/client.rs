//! Calls to an RCF service, and subscriptions to an RCF publisher (blocking, std::net).

use std::io;
use std::net::{Shutdown, SocketAddr, TcpStream, ToSocketAddrs};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use crate::msg::{self, Request, Response, FN_PING};
use crate::server::{Connection, Server};
use crate::sf::Writer;
use crate::wire::{read_frame, write_frame};

/// RCF's defaults (ClientStub.cpp): 2 s to connect, 10 s for a call.
pub const DEFAULT_CONNECT_TIMEOUT: Duration = Duration::from_secs(2);
pub const DEFAULT_CALL_TIMEOUT: Duration = Duration::from_secs(10);

fn resolve(addr: impl ToSocketAddrs) -> io::Result<SocketAddr> {
    addr.to_socket_addrs()?.next().ok_or_else(|| io::Error::other("rcf: no address"))
}

fn connect_to(addr: SocketAddr, connect: Duration, call: Duration) -> io::Result<TcpStream> {
    let s = TcpStream::connect_timeout(&addr, connect)?;
    s.set_read_timeout(Some(call))?;
    s.set_write_timeout(Some(call))?;
    s.set_nodelay(true)?;
    Ok(s)
}

/// Whether the peer has not closed the connection (ClientTransport::isConnected: a pending
/// end of stream means it is gone).
fn alive(s: &TcpStream) -> bool {
    if s.set_nonblocking(true).is_err() {
        return false;
    }
    let mut b = [0u8; 1];
    let r = s.peek(&mut b);
    let back = s.set_nonblocking(false).is_ok();
    back && match r {
        Ok(0) => false,
        Ok(_) => true,
        Err(e) => e.kind() == io::ErrorKind::WouldBlock,
    }
}

/// The arguments of a call, written in order.
pub fn args(f: impl FnOnce(&mut Writer)) -> Vec<u8> {
    let mut w = Writer::new();
    f(&mut w);
    w.into_bytes()
}

enum Endpoint {
    Addr(SocketAddr),
    Host(String, u16),
}

/// One connection to one interface of a service (RcfClient<I>).
pub struct Client {
    endpoint: Endpoint,
    interface: String,
    stream: Option<TcpStream>,
    connect_timeout: Duration,
    call_timeout: Duration,
}

impl Client {
    /// Connects at once, with `timeout` for the connection and every call.
    pub fn connect(addr: impl ToSocketAddrs, interface: &str, timeout: Duration) -> io::Result<Self> {
        let a = resolve(addr)?;
        let stream = connect_to(a, timeout, timeout)?;
        Ok(Client {
            endpoint: Endpoint::Addr(a),
            interface: interface.into(),
            stream: Some(stream),
            connect_timeout: timeout,
            call_timeout: timeout,
        })
    }

    /// RcfClient<I>(TcpEndpoint(host, port)): nothing happens until the first call, which
    /// connects; a call after the server closed the connection connects again (RCF's
    /// auto-reconnect), and so does one after a call failed. RCF's default timeouts.
    pub fn new(host: &str, port: u16, interface: &str) -> Self {
        Client {
            endpoint: Endpoint::Host(host.into(), port),
            interface: interface.into(),
            stream: None,
            connect_timeout: DEFAULT_CONNECT_TIMEOUT,
            call_timeout: DEFAULT_CALL_TIMEOUT,
        }
    }

    /// ClientStub::setConnectTimeoutMs.
    pub fn set_connect_timeout(&mut self, d: Duration) {
        self.connect_timeout = d;
    }

    /// ClientStub::setRemoteCallTimeoutMs.
    pub fn set_call_timeout(&mut self, d: Duration) {
        self.call_timeout = d;
    }

    pub fn is_connected(&self) -> bool {
        self.stream.as_ref().is_some_and(alive)
    }

    fn stream(&mut self) -> io::Result<&mut TcpStream> {
        if self.stream.as_ref().is_some_and(|s| !alive(s)) {
            self.stream = None;
        }
        if self.stream.is_none() {
            let a = match &self.endpoint {
                Endpoint::Addr(a) => *a,
                Endpoint::Host(h, p) => resolve((h.as_str(), *p))?,
            };
            self.stream = Some(connect_to(a, self.connect_timeout, self.call_timeout)?);
        }
        let s = self.stream.as_mut().unwrap();
        s.set_read_timeout(Some(self.call_timeout))?;
        s.set_write_timeout(Some(self.call_timeout))?;
        Ok(s)
    }

    /// One request and its answer; a transport failure drops the connection.
    fn exchange(&mut self, req: &[u8]) -> io::Result<Vec<u8>> {
        let s = self.stream()?;
        let r = write_frame(s, req).and_then(|_| read_frame(s));
        if r.is_err() {
            self.stream = None;
        }
        r
    }

    /// A two-way call: `args` are the arguments' nodes; the answer holds the return value's node
    /// (none for void), then each by-reference argument's.
    pub fn call(&mut self, fn_id: i32, args: &[u8]) -> io::Result<Vec<u8>> {
        let req = Request::call(&self.interface, fn_id, false).encode(args);
        let msg = self.exchange(&req)?;
        Ok(Response::decode(&msg)?.1.to_vec())
    }

    pub fn oneway(&mut self, fn_id: i32, args: &[u8]) -> io::Result<()> {
        let req = Request::call(&self.interface, fn_id, true).encode(args);
        let s = self.stream()?;
        let r = write_frame(s, &req);
        if r.is_err() {
            self.stream = None;
        }
        r
    }

    pub fn ping(&mut self) -> io::Result<()> {
        let msg = self.exchange(&Request::call("", FN_PING, false).encode(&[]))?;
        Response::decode(&msg).map(|_| ())
    }

    /// RCF::createCallbackConnection(client, server): this client's connection (made now if
    /// there is none) is turned around. An out-of-band ping asks the service to take it as its
    /// callback connection; from then on `server` serves the calls that come down it, on a
    /// thread of its own, and this client has no connection (the next call makes a new one).
    pub fn create_callback_connection(&mut self, server: &Arc<Server>) -> io::Result<Connection> {
        let mut req = Request::call("", FN_PING, false);
        req.oob = msg::oob_create_callback();
        let reply = self.exchange(&req.encode(&[]))?;
        let (resp, _) = Response::decode(&reply)?;
        let err = msg::parse_oob_callback_created(&resp.oob)?;
        if err != 0 {
            return Err(io::Error::other(format!("rcf: callback connection refused ({err})")));
        }
        let s = self.stream.take().ok_or_else(|| io::Error::other("rcf: no connection"))?;
        s.set_read_timeout(None)?;
        s.set_write_timeout(Some(self.call_timeout))?;
        Ok(server.serve(s))
    }
}

/// Connects to the publisher of `listener` and subscribes (SubscriptionService::
/// doRequestSubscription): a ping first, as RCF's client stub does on a new connection, then the
/// subscription request.
fn request_subscription(addr: impl ToSocketAddrs, listener: &str, connect: Duration, call: Duration)
    -> io::Result<TcpStream>
{
    let mut s = connect_to(resolve(addr)?, connect, call)?;
    write_frame(&mut s, &Request::call("", FN_PING, false).encode(&[]))?;
    Response::decode(&read_frame(&mut s)?)?;
    let mut req = Request::call("", FN_PING, false);
    req.oob = msg::oob_subscribe(listener);
    write_frame(&mut s, &req.encode(&[]))?;
    let reply = read_frame(&mut s)?;
    let (resp, _) = Response::decode(&reply)?;
    let err = msg::parse_oob_subscribed(&resp.oob)?;
    if err != 0 {
        return Err(io::Error::other(format!("rcf: subscription to {listener} refused ({err})")));
    }
    s.set_read_timeout(None)?;
    Ok(s)
}

/// The events of a subscription, until its connection ends.
fn events<F: FnMut(i32, &[u8])>(mut s: TcpStream, mut on_call: F) -> io::Result<()> {
    loop {
        let msg = read_frame(&mut s)?;
        let (req, args) = Request::decode(&msg)?;
        if req.fn_id >= 0 {
            on_call(req.fn_id, args);
        }
        if !req.oneway {
            write_frame(&mut s, &Response::default().encode(&[]))?;
        }
    }
}

/// Subscribes to the publisher of `listener` at `addr` (RCF's createSubscription) and calls
/// `on_call(fn_id, args)` for each event, on a thread of its own, until the connection ends.
pub fn subscribe<F>(addr: impl ToSocketAddrs, listener: &str, timeout: Duration, on_call: F)
    -> io::Result<thread::JoinHandle<io::Result<()>>>
where
    F: FnMut(i32, &[u8]) + Send + 'static,
{
    let s = request_subscription(addr, listener, timeout, timeout)?;
    Ok(thread::spawn(move || events(s, on_call)))
}

/// A subscription (RCF::SubscriptionPtr): its events go to the callback on a thread of its own;
/// is_connected() tells whether its connection is still up; close(), or dropping it, ends it.
pub struct Subscription {
    stream: TcpStream,
    connected: Arc<AtomicBool>,
}

impl Subscription {
    /// RcfServer::createSubscription<I>(servant, parms with the publisher's endpoint), with the
    /// connect and call timeouts for the subscription request.
    pub fn create<F>(addr: impl ToSocketAddrs, listener: &str, connect: Duration, call: Duration, on_call: F)
        -> io::Result<Subscription>
    where
        F: FnMut(i32, &[u8]) + Send + 'static,
    {
        let s = request_subscription(addr, listener, connect, call)?;
        let connected = Arc::new(AtomicBool::new(true));
        let reader = s.try_clone()?;
        let flag = connected.clone();
        thread::spawn(move || {
            let _ = events(reader, on_call);
            flag.store(false, Ordering::SeqCst);
        });
        Ok(Subscription { stream: s, connected })
    }

    pub fn is_connected(&self) -> bool {
        self.connected.load(Ordering::SeqCst)
    }

    pub fn close(&self) {
        let _ = self.stream.shutdown(Shutdown::Both);
    }
}

impl Drop for Subscription {
    fn drop(&mut self) {
        self.close();
    }
}
