//! Request and response headers (MethodInvocationRequest), and the out-of-band subscription.

use std::io;

use crate::wire::{put_bool, put_bytes, put_int, Cursor};
use crate::{bad, RUNTIME_VERSION};

pub const DESCRIPTOR_ERROR: i32 = 0;
pub const DESCRIPTOR_REQUEST: i32 = 1;
pub const DESCRIPTOR_RESPONSE: i32 = 2;
/// SerializationProtocol 1: SF binary.
pub const SF_BINARY: i32 = 1;
/// The fnId of a ping, and of a request that only carries an out-of-band message.
pub const FN_PING: i32 = -1;
/// RcfError_NoServerStub / RcfError_UnknownInterface, as an error reply.
pub const ERROR_NO_SERVER_STUB: i32 = 25;

#[derive(Debug, Clone, Default, PartialEq)]
pub struct Request {
    pub service: String,
    pub token: i32,
    /// The interface called; empty on the wire when it is the service itself.
    pub subinterface: String,
    pub fn_id: i32,
    pub oneway: bool,
    pub close: bool,
    pub runtime: i32,
    pub oob: Vec<u8>,
}

impl Request {
    pub fn call(service: &str, fn_id: i32, oneway: bool) -> Self {
        Request { service: service.into(), fn_id, oneway, runtime: RUNTIME_VERSION, ..Default::default() }
    }

    /// The interface the call is for.
    pub fn interface(&self) -> &str {
        if self.subinterface.is_empty() { &self.service } else { &self.subinterface }
    }

    /// Header version 7 (runtime 12), then `body`.
    pub fn encode(&self, body: &[u8]) -> Vec<u8> {
        let mut v = Vec::with_capacity(48 + body.len());
        put_int(&mut v, DESCRIPTOR_REQUEST);
        put_int(&mut v, 7);
        put_bytes(&mut v, self.service.as_bytes());
        put_int(&mut v, self.token);
        let sub = if self.subinterface == self.service { "" } else { &self.subinterface };
        put_bytes(&mut v, sub.as_bytes());
        put_int(&mut v, self.fn_id);
        put_int(&mut v, SF_BINARY);
        put_bool(&mut v, self.oneway);
        put_bool(&mut v, self.close);
        put_int(&mut v, self.runtime);
        put_bool(&mut v, false); // ignore runtime version
        put_int(&mut v, 0); // ping-back interval
        put_int(&mut v, 0); // archive version
        put_bytes(&mut v, b""); // user data
        put_bool(&mut v, false); // native wstring serialization
        put_bool(&mut v, false); // SF pointer tracking
        put_bytes(&mut v, &self.oob);
        v.extend_from_slice(body);
        v
    }

    /// The header (versions 1 to 7, as MethodInvocationRequest::decodeRequest reads them) and
    /// where the arguments start.
    pub fn decode(msg: &[u8]) -> io::Result<(Request, &[u8])> {
        let mut c = Cursor::new(msg);
        if c.int()? != DESCRIPTOR_REQUEST {
            return Err(bad("not a request"));
        }
        let ver = c.int()?;
        if !(0..=7).contains(&ver) {
            return Err(bad("request header version"));
        }
        let mut r = Request {
            service: String::from_utf8_lossy(c.bytes()?).into_owned(),
            token: c.int()?,
            subinterface: String::from_utf8_lossy(c.bytes()?).into_owned(),
            fn_id: c.int()?,
            runtime: 1,
            ..Default::default()
        };
        if c.int()? != SF_BINARY {
            return Err(bad("serialization protocol"));
        }
        r.oneway = c.bool()?;
        r.close = c.bool()?;
        if ver >= 1 {
            r.runtime = c.int()?;
            c.bool()?;
        }
        if ver >= 2 {
            c.int()?;
        }
        if ver >= 3 {
            c.int()?;
        }
        if ver >= 4 {
            c.bytes()?;
        }
        if ver >= 5 {
            c.bool()?;
        }
        if ver >= 6 {
            c.bool()?;
        }
        if ver >= 7 {
            r.oob = c.bytes()?.to_vec();
        }
        Ok((r, c.rest()))
    }
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct Response {
    pub exception: bool,
    pub oob: Vec<u8>,
}

impl Response {
    /// Header version 3 (runtime 12), then `body`.
    pub fn encode(&self, body: &[u8]) -> Vec<u8> {
        let mut v = Vec::with_capacity(8 + self.oob.len() + body.len());
        put_int(&mut v, DESCRIPTOR_RESPONSE);
        put_int(&mut v, 3);
        put_bool(&mut v, self.exception);
        put_bytes(&mut v, b""); // user data
        put_bool(&mut v, false); // SF pointer tracking
        put_bytes(&mut v, &self.oob);
        v.extend_from_slice(body);
        v
    }

    /// The header and where the return value starts; a server error is an Err.
    pub fn decode(msg: &[u8]) -> io::Result<(Response, &[u8])> {
        let mut c = Cursor::new(msg);
        let id = c.int()?;
        let ver = c.int()?;
        if id == DESCRIPTOR_ERROR {
            let code = c.int()?;
            return Err(io::Error::other(format!("rcf: server error {code}")));
        }
        if id != DESCRIPTOR_RESPONSE {
            return Err(bad("not a response"));
        }
        let mut r = Response { exception: c.bool()?, ..Default::default() };
        if ver >= 1 {
            c.bytes()?;
        }
        if ver >= 2 {
            c.bool()?;
        }
        if ver >= 3 {
            r.oob = c.bytes()?.to_vec();
        }
        if r.exception {
            return Err(io::Error::other("rcf: the server threw"));
        }
        Ok((r, c.rest()))
    }
}

/// encodeServerError: what RCF answers a call it cannot dispatch.
pub fn server_error(code: i32) -> Vec<u8> {
    let mut v = Vec::new();
    put_int(&mut v, DESCRIPTOR_ERROR);
    put_int(&mut v, 0);
    put_int(&mut v, code);
    v
}

const OOB_REQUEST_MAGIC: i32 = 65;
const OOB_RESPONSE_MAGIC: i32 = 66;
const OMT_CREATE_CALLBACK_CONNECTION: i32 = 2;
const OMT_REQUEST_SUBSCRIPTION: i32 = 3;

/// OobCreateCallbackConnection's request: the server is to take this connection as a callback
/// connection (RCF::createCallbackConnection).
pub fn oob_create_callback() -> Vec<u8> {
    let mut v = Vec::new();
    put_int(&mut v, OOB_REQUEST_MAGIC);
    put_int(&mut v, OMT_CREATE_CALLBACK_CONNECTION);
    put_int(&mut v, RUNTIME_VERSION);
    v
}

/// The error code in the server's answer to oob_create_callback.
pub fn parse_oob_callback_created(oob: &[u8]) -> io::Result<i32> {
    let mut c = Cursor::new(oob);
    if c.int()? != OOB_RESPONSE_MAGIC || c.int()? != OMT_CREATE_CALLBACK_CONNECTION {
        return Err(bad("callback connection answer"));
    }
    c.int()?;
    c.int()
}

/// OobRequestSubscription's request: subscribe to the publisher of `listener`.
pub fn oob_subscribe(listener: &str) -> Vec<u8> {
    let mut v = Vec::new();
    put_int(&mut v, OOB_REQUEST_MAGIC);
    put_int(&mut v, OMT_REQUEST_SUBSCRIPTION);
    put_int(&mut v, RUNTIME_VERSION);
    put_bytes(&mut v, listener.as_bytes());
    put_int(&mut v, 0); // subscriber-to-publisher ping interval
    v
}

/// The publisher's answer to oob_subscribe: no error, no ping.
pub fn oob_subscribed(error: i32) -> Vec<u8> {
    let mut v = Vec::new();
    put_int(&mut v, OOB_RESPONSE_MAGIC);
    put_int(&mut v, OMT_REQUEST_SUBSCRIPTION);
    put_int(&mut v, RUNTIME_VERSION);
    put_int(&mut v, error);
    put_bytes(&mut v, b""); // error string
    put_int(&mut v, 0); // publisher-to-subscriber ping interval
    v
}

/// The listener name of a subscription request, or None for another out-of-band message.
pub fn parse_oob_subscribe(oob: &[u8]) -> io::Result<Option<String>> {
    let mut c = Cursor::new(oob);
    if c.int()? != OOB_REQUEST_MAGIC {
        return Err(bad("out-of-band magic"));
    }
    if c.int()? != OMT_REQUEST_SUBSCRIPTION {
        return Ok(None);
    }
    c.int()?;
    Ok(Some(String::from_utf8_lossy(c.bytes()?).into_owned()))
}

/// The error code in the publisher's answer to a subscription.
pub fn parse_oob_subscribed(oob: &[u8]) -> io::Result<i32> {
    let mut c = Cursor::new(oob);
    if c.int()? != OOB_RESPONSE_MAGIC || c.int()? != OMT_REQUEST_SUBSCRIPTION {
        return Err(bad("subscription answer"));
    }
    c.int()?;
    c.int()
}
