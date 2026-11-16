//! The header encodings (SF::encodeInt/encodeBool/encodeString) and the frame.

use std::io::{self, Read, Write};

use crate::bad;

/// An int: one byte for 0..=254, else 255 and four bytes.
pub fn put_int(v: &mut Vec<u8>, n: i32) {
    if (0..255).contains(&n) {
        v.push(n as u8);
    } else {
        v.push(255);
        v.extend_from_slice(&n.to_le_bytes());
    }
}

pub fn put_bool(v: &mut Vec<u8>, b: bool) {
    v.push(b as u8);
}

/// A string or byte buffer: its length as an int, then the bytes.
pub fn put_bytes(v: &mut Vec<u8>, s: &[u8]) {
    put_int(v, s.len() as i32);
    v.extend_from_slice(s);
}

/// Reads what the put_* functions write.
pub struct Cursor<'a> {
    pub buf: &'a [u8],
    pub pos: usize,
}

impl<'a> Cursor<'a> {
    pub fn new(buf: &'a [u8]) -> Self {
        Cursor { buf, pos: 0 }
    }

    pub fn byte(&mut self) -> io::Result<u8> {
        let b = *self.buf.get(self.pos).ok_or_else(|| bad("short message"))?;
        self.pos += 1;
        Ok(b)
    }

    pub fn take(&mut self, n: usize) -> io::Result<&'a [u8]> {
        if self.pos + n > self.buf.len() {
            return Err(bad("short message"));
        }
        let s = &self.buf[self.pos..self.pos + n];
        self.pos += n;
        Ok(s)
    }

    pub fn int(&mut self) -> io::Result<i32> {
        match self.byte()? {
            255 => Ok(i32::from_le_bytes(self.take(4)?.try_into().unwrap())),
            b => Ok(b as i32),
        }
    }

    pub fn bool(&mut self) -> io::Result<bool> {
        Ok(self.byte()? != 0)
    }

    pub fn bytes(&mut self) -> io::Result<&'a [u8]> {
        let n = self.int()?;
        if n < 0 {
            return Err(bad("negative length"));
        }
        self.take(n as usize)
    }

    pub fn rest(&self) -> &'a [u8] {
        &self.buf[self.pos..]
    }
}

/// The largest frame accepted (the unit's messages are a few hundred bytes).
pub const MAX_FRAME: usize = 1 << 20;

pub fn write_frame(w: &mut impl Write, msg: &[u8]) -> io::Result<()> {
    let mut f = Vec::with_capacity(4 + msg.len());
    f.extend_from_slice(&(msg.len() as u32).to_le_bytes());
    f.extend_from_slice(msg);
    w.write_all(&f)
}

pub fn read_frame(r: &mut impl Read) -> io::Result<Vec<u8>> {
    let mut len = [0u8; 4];
    r.read_exact(&mut len)?;
    let n = u32::from_le_bytes(len) as usize;
    if n > MAX_FRAME {
        return Err(bad("frame too long"));
    }
    let mut msg = vec![0u8; n];
    r.read_exact(&mut msg)?;
    Ok(msg)
}
