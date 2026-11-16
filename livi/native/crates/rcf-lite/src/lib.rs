//! RCF 2.x over TCP, as the VW RCD330's (Desay SVP) services speak it: runtime version 12,
//! request header version 7, SF binary archives, no transport filters, publish/subscribe.
//!
//! Read from RCF 2.1.0.0's source (`MethodInvocation.cpp`, `SF/Encoding.cpp`, `SF/Stream.cpp`)
//! and checked against frames captured between our C++ shim (RCF 2.1, built from source) and its
//! peers. Every multi-byte value is little-endian (RCF's "network order").
//!
//! - A frame: u32 length, then the message.
//! - A request: header (`msg::Request`), then the arguments as SF nodes.
//! - A response: header (`msg::Response`), then the return value and every by-reference
//!   argument, as SF nodes.
//! - Publish/subscribe: the subscriber connects to the publisher's port and sends an out-of-band
//!   subscription request (`msg::oob_subscribe`); the publisher then sends one-way requests down
//!   that connection, with the listener interface's name as the subinterface.

pub mod client;
pub mod msg;
pub mod server;
pub mod sf;
pub mod wire;

pub use client::{subscribe, Client, Subscription};
pub use server::{Connection, Publisher, Server, ServerHandle, Service};
pub use sf::{Reader, Sf, Writer};

/// The runtime version the unit's RCF (libRCF.so.2.2.0.0) and our C++ build speak.
pub const RUNTIME_VERSION: i32 = 12;

fn bad(what: &str) -> std::io::Error {
    std::io::Error::new(std::io::ErrorKind::InvalidData, format!("rcf: {what}"))
}
