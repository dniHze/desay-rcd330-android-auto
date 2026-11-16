//! The parts the CarLife shim uses beyond a call and a subscription, over loopback: a client
//! that connects at its first call and again after the server went, a subscription's state and
//! close, the publisher's subscriber callback, a callback connection (RCF's
//! createCallbackConnection), stopping a server, and byte vectors.

use std::io;
use std::net::TcpListener;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{mpsc, Arc};
use std::time::{Duration, Instant};

use rcf_lite::client::args;
use rcf_lite::{Client, Publisher, Reader, Server, Service, Subscription, Writer};

const T: Duration = Duration::from_secs(2);

struct Counter;
impl Service for Counter {
    fn interface(&self) -> &str {
        "SVP_Counter"
    }
    fn call(&self, fn_id: i32, a: &mut Reader, out: &mut Writer) -> io::Result<()> {
        match fn_id {
            // (n, bytes&) -> n + 1, bytes reversed
            0 => {
                let (n, mut v): (i32, Vec<u8>) = (a.get()?, a.get()?);
                v.reverse();
                out.put(&(n + 1)).put(&v);
                Ok(())
            }
            _ => Err(io::Error::other("no such method")),
        }
    }
}

fn wait_for(mut f: impl FnMut() -> bool) -> bool {
    let end = Instant::now() + T;
    while Instant::now() < end {
        if f() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    false
}

#[test]
fn a_lazy_client_connects_at_its_first_call_and_again_after_a_restart() {
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = l.local_addr().unwrap().port();
    let mut h = Arc::new(Server::new().bind(Arc::new(Counter))).start(l).unwrap();
    let mut c = Client::new("127.0.0.1", port, "SVP_Counter");
    assert!(!c.is_connected());
    let body = c.call(0, &args(|w| {
        w.put(&41i32).put(&vec![1u8, 2, 3]);
    })).unwrap();
    let mut r = Reader::new(&body);
    assert_eq!((r.get::<i32>().unwrap(), r.get::<Vec<u8>>().unwrap()), (42, vec![3, 2, 1]));
    assert!(c.is_connected());
    // The server stops: its port closes and the connection ends.
    h.stop();
    assert!(wait_for(|| !c.is_connected()));
    assert!(c.call(0, &args(|w| {
        w.put(&0i32).put(&Vec::<u8>::new());
    })).is_err());
    // A new server on the same port: the next call connects again.
    let l = TcpListener::bind(("127.0.0.1", port)).unwrap();
    let _h = Arc::new(Server::new().bind(Arc::new(Counter))).start(l).unwrap();
    let body = c.call(0, &args(|w| {
        w.put(&1i32).put(&Vec::<u8>::new());
    })).unwrap();
    assert_eq!(Reader::new(&body).get::<i32>().unwrap(), 2);
}

#[test]
fn a_subscription_tells_when_it_ends_and_the_publisher_hears_of_subscribers() {
    let joined = Arc::new(AtomicUsize::new(0));
    let j = joined.clone();
    let p = Publisher::with_on_subscribe("SVP_CounterListener", move || {
        j.fetch_add(1, Ordering::SeqCst);
    });
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = l.local_addr().unwrap();
    let mut h = Arc::new(Server::new().publish(p.clone())).start(l).unwrap();
    let (tx, rx) = mpsc::channel();
    let sub = Subscription::create(addr, "SVP_CounterListener", T, T, move |f, a| {
        let _ = tx.send((f, Reader::new(a).get::<Vec<u8>>().unwrap()));
    })
    .unwrap();
    assert_eq!(joined.load(Ordering::SeqCst), 1, "told before the subscriber is added");
    assert!(sub.is_connected());
    assert_eq!(p.publish(3, &args(|w| {
        w.put(&b"\xa8\x87\xb3".to_vec());
    })), 1);
    assert_eq!(rx.recv_timeout(T).unwrap(), (3, vec![0xa8, 0x87, 0xb3]), "raw bytes, not UTF-8");
    // Closed by the subscriber: the publisher drops it.
    sub.close();
    assert!(wait_for(|| !sub.is_connected()));
    assert!(wait_for(|| p.subscribers() == 0));
    // Ended by the publisher's side: the subscription says so.
    let sub = Subscription::create(addr, "SVP_CounterListener", T, T, |_, _| {}).unwrap();
    h.stop();
    assert!(wait_for(|| !sub.is_connected()));
    assert!(Subscription::create(addr, "SVP_CounterListener", T, T, |_, _| {}).is_err(), "the port is closed");
}

struct Callback(mpsc::Sender<(i32, i32)>);
impl Service for Callback {
    fn interface(&self) -> &str {
        "SVP_RegistryCallback"
    }
    fn call(&self, fn_id: i32, a: &mut Reader, out: &mut Writer) -> io::Result<()> {
        let (x, y): (i32, i32) = (a.get()?, a.get()?);
        self.0.send((fn_id, x + y)).unwrap();
        out.put(&1i32);
        Ok(())
    }
}

#[test]
fn a_callback_connection_serves_the_clients_servant() {
    // The "service" (sourcemgr's part, by hand): answers the registration, then expects the out-of-band callback request
    // and calls back over the same connection, as an RCF server with a callback connection does.
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = l.local_addr().unwrap();
    let (reg_tx, reg_rx) = mpsc::channel();
    let (conn_tx, conn_rx) = mpsc::channel();
    std::thread::spawn(move || {
        use rcf_lite::msg::{Request, Response};
        use rcf_lite::wire::{read_frame, write_frame};
        let (mut s, _) = l.accept().unwrap();
        // RegisterClient(pid)
        let m = read_frame(&mut s).unwrap();
        let (req, a) = Request::decode(&m).unwrap();
        assert_eq!((req.interface(), req.fn_id), ("SVP_Registry", 0));
        reg_tx.send(Reader::new(a).get::<i32>().unwrap()).unwrap();
        write_frame(&mut s, &Response::default().encode(&[])).unwrap();
        // The callback request: a ping with the out-of-band message.
        let m = read_frame(&mut s).unwrap();
        let (req, _) = Request::decode(&m).unwrap();
        assert_eq!(req.oob, rcf_lite::msg::oob_create_callback());
        let mut oob = Vec::new();
        for v in [66, 2, 12, 0] {
            rcf_lite::wire::put_int(&mut oob, v);
        }
        rcf_lite::wire::put_bytes(&mut oob, b"");
        write_frame(&mut s, &Response { oob, ..Default::default() }.encode(&[])).unwrap();
        // Now the client's servant: cbSomething(20, 22) -> 1.
        let call = Request::call("SVP_RegistryCallback", 0, false).encode(&args(|w| {
            w.put(&20i32).put(&22i32);
        }));
        write_frame(&mut s, &call).unwrap();
        let reply = read_frame(&mut s).unwrap();
        let (_, body) = Response::decode(&reply).unwrap();
        conn_tx.send(Reader::new(body).get::<i32>().unwrap()).unwrap();
    });
    let (cb_tx, cb_rx) = mpsc::channel();
    let callback = Arc::new(Server::new().bind(Arc::new(Callback(cb_tx))));
    let mut c = Client::new("127.0.0.1", addr.port(), "SVP_Registry");
    c.call(0, &args(|w| {
        w.put(&1234i32);
    }))
    .unwrap();
    assert_eq!(reg_rx.recv_timeout(T).unwrap(), 1234);
    let conn = c.create_callback_connection(&callback).unwrap();
    assert!(!c.is_connected(), "the connection went to the callback server");
    assert_eq!(cb_rx.recv_timeout(T).unwrap(), (0, 42));
    assert_eq!(conn_rx.recv_timeout(T).unwrap(), 1);
    conn.close();
}
