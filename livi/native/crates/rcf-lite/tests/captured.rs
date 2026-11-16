//! Frames captured (2026-11-16) between our C++ shim (RCF 2.1 built from source, runtime 12) and
//! its peers: cl_probe (ISVP_CarLifeService client and ISVP_CarLifeListener subscriber) and
//! fake_can (SVP_CanService and its SVP_CanListener publisher). Frame length prefixes removed.

use std::net::TcpListener;
use std::sync::{mpsc, Arc};
use std::time::Duration;

use rcf_lite::client::args;
use rcf_lite::msg::{oob_subscribe, oob_subscribed, Request, Response, FN_PING};
use rcf_lite::{sf_struct, subscribe, Client, Publisher, Reader, Server, Service, Writer};

fn hex(s: &str) -> Vec<u8> {
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
}

const CL_STATUS_REQ: &str = "010713495356505f4361724c696665536572766963650000080100000c00000000000000";
const CL_STATUS_RESP0: &str = "020300000000000001040000000002";
const CL_TOUCH_REQ: &str = "010713495356505f4361724c696665536572766963650000060100000c0000000000000000000104640000000200000104c800000002000001040000000002";
const VOID_RESP: &str = "020300000000";
const PING_REQ: &str = "0107000000ffffffffff0100000c00000000000000";
const SUB_REQ: &str = "0107000000ffffffffff0100000c0000000000001941030c14495356505f4361724c6966654c697374656e657200";
const SUB_RESP: &str = "02030000000642030c000000";
const CL_EVENT: &str = "0107000014495356505f4361724c6966654c697374656e6572010101000c00000000000000000001040200000002000001040000000002";
const CAN_INFO_REQ: &str = "01070e5356505f43616e536572766963650000030100000c00000000000000000000000101000200000101000200000101000202";
const CAN_INFO_RESP: &str = "020300000000000000000101010200000101010200000101000202";
const CAN_SPEED_EVENT: &str = "010700000f5356505f43616e4c697374656e6572010101000c0000000000000000000102100e02";

sf_struct!(CarplayInfo { day_night: u8, transmission: u8, right_hand_drive: u8 });

#[test]
fn requests_encode_as_the_cpp_client_does() {
    assert_eq!(Request::call("ISVP_CarLifeService", 8, false).encode(&[]), hex(CL_STATUS_REQ));
    let touch = args(|w| {
        w.put(&100u32).put(&200u32).put(&0i32);
    });
    assert_eq!(Request::call("ISVP_CarLifeService", 6, false).encode(&touch), hex(CL_TOUCH_REQ));
    assert_eq!(Request::call("", FN_PING, false).encode(&[]), hex(PING_REQ));
    let mut sub = Request::call("", FN_PING, false);
    sub.oob = oob_subscribe("ISVP_CarLifeListener");
    assert_eq!(sub.encode(&[]), hex(SUB_REQ));
    let info = args(|w| {
        w.put(&CarplayInfo::default());
    });
    assert_eq!(Request::call("SVP_CanService", 3, false).encode(&info), hex(CAN_INFO_REQ));
}

#[test]
fn responses_and_events_decode() {
    let msg = hex(CL_STATUS_RESP0);
    let (_, body) = Response::decode(&msg).unwrap();
    assert_eq!(Reader::new(body).get::<i32>().unwrap(), 0);
    let msg = hex(CAN_INFO_RESP);
    let (_, body) = Response::decode(&msg).unwrap();
    let info: CarplayInfo = Reader::new(body).get().unwrap();
    assert_eq!(info, CarplayInfo { day_night: 1, transmission: 1, right_hand_drive: 0 });
    let msg = hex(CAN_SPEED_EVENT);
    let (req, body) = Request::decode(&msg).unwrap();
    assert_eq!((req.interface(), req.fn_id, req.oneway), ("SVP_CanListener", 1, true));
    assert_eq!(Reader::new(body).get::<u16>().unwrap(), 3600);
}

#[test]
fn server_side_encodes_as_the_cpp_server_does() {
    assert_eq!(Response::default().encode(&[]), hex(VOID_RESP));
    let mut w = Writer::new();
    w.put(&0i32);
    assert_eq!(Response::default().encode(&w.buf), hex(CL_STATUS_RESP0));
    let sub = Response { oob: oob_subscribed(0), ..Default::default() };
    assert_eq!(sub.encode(&[]), hex(SUB_RESP));
    let event = Request {
        subinterface: "ISVP_CarLifeListener".into(),
        ..Request::call("", 1, true)
    };
    let a = args(|w| {
        w.put(&2i32).put(&0i32);
    });
    assert_eq!(event.encode(&a), hex(CL_EVENT));
    let (req, _) = Request::decode(&hex(CL_TOUCH_REQ)).unwrap();
    assert_eq!((req.interface(), req.fn_id, req.oneway), ("ISVP_CarLifeService", 6, false));
}

struct Echo;
impl Service for Echo {
    fn interface(&self) -> &str {
        "SVP_Echo"
    }
    fn call(&self, fn_id: i32, a: &mut Reader, out: &mut Writer) -> std::io::Result<()> {
        match fn_id {
            0 => {
                let s: String = a.get()?;
                out.put(&format!("{s}!"));
                Ok(())
            }
            _ => Err(std::io::Error::other("no such method")),
        }
    }
}

#[test]
fn loopback_call_and_subscription() {
    let pub_ = Publisher::new("SVP_EchoListener");
    let server = Arc::new(Server::new().bind(Arc::new(Echo)).publish(pub_.clone()));
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = l.local_addr().unwrap();
    std::thread::spawn(move || server.run(l));
    let t = Duration::from_secs(2);
    let mut c = Client::connect(addr, "SVP_Echo", t).unwrap();
    c.ping().unwrap();
    let body = c.call(0, &args(|w| {
        w.put(&"héllo".to_string());
    })).unwrap();
    assert_eq!(Reader::new(&body).get::<String>().unwrap(), "héllo!");
    assert!(c.call(7, &[]).is_err(), "an unknown method is a server error");
    let (tx, rx) = mpsc::channel();
    subscribe(addr, "SVP_EchoListener", t, move |fn_id, a| {
        tx.send((fn_id, Reader::new(a).get::<u32>().unwrap())).unwrap();
    })
    .unwrap();
    assert_eq!(pub_.subscribers(), 1);
    assert_eq!(pub_.publish(4, &args(|w| {
        w.put(&42u32);
    })), 1);
    assert_eq!(rx.recv_timeout(t).unwrap(), (4, 42));
    assert!(subscribe(addr, "SVP_Nobody", t, |_, _| {}).is_err(), "an unknown publisher refuses");
}
