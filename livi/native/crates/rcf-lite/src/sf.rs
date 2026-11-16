//! SF binary archives (SF::OStream / IStream with EncodingBinaryPortable): every value is a node,
//! `Begin attrs ... End`; a fundamental's node holds `Data <length> <bytes>`; a struct's node holds
//! its fields' nodes in order; a string's holds its length node, then the raw bytes.

use std::io;

use crate::bad;
use crate::wire::Cursor;

const BEGIN: u8 = 0;
const DATA: u8 = 1;
const END: u8 = 2;
const BEGIN_ARCHIVE_METADATA: u8 = 4;

#[derive(Default)]
pub struct Writer {
    pub buf: Vec<u8>,
}

impl Writer {
    pub fn new() -> Self {
        Writer::default()
    }

    /// A node with no id, type or label (what RCF writes for arguments).
    pub fn begin(&mut self) {
        self.buf.push(BEGIN);
        self.buf.push(0);
    }

    pub fn end(&mut self) {
        self.buf.push(END);
    }

    /// OStream::write_int for runtime versions 9 and up: one byte below 128, else 128 and four.
    fn count(&mut self, n: u32) {
        if n <= 127 {
            self.buf.push(n as u8);
        } else {
            self.buf.push(128);
            self.buf.extend_from_slice(&n.to_le_bytes());
        }
    }

    /// A fundamental value's node.
    pub fn data(&mut self, bytes: &[u8]) {
        self.begin();
        self.buf.push(DATA);
        self.count(bytes.len() as u32);
        self.buf.extend_from_slice(bytes);
        self.end();
    }

    pub fn put<T: Sf>(&mut self, v: &T) -> &mut Self {
        v.put(self);
        self
    }

    pub fn into_bytes(self) -> Vec<u8> {
        self.buf
    }
}

pub struct Reader<'a> {
    c: Cursor<'a>,
}

impl<'a> Reader<'a> {
    pub fn new(buf: &'a [u8]) -> Self {
        Reader { c: Cursor::new(buf) }
    }

    fn count(&mut self) -> io::Result<u32> {
        match self.c.byte()? {
            128 => Ok(u32::from_le_bytes(self.c.take(4)?.try_into().unwrap())),
            b if b < 128 => Ok(b as u32),
            _ => Err(bad("bad count")),
        }
    }

    pub fn begin(&mut self) -> io::Result<()> {
        let mut tag = self.c.byte()?;
        if tag == BEGIN_ARCHIVE_METADATA {
            let runtime = self.c.int()?;
            let _archive = self.c.int()?;
            if runtime >= 10 {
                self.c.bool()?; // pointer tracking
            }
            tag = self.c.byte()?;
        }
        if tag != BEGIN {
            return Err(bad("expected a node"));
        }
        let attrs = self.c.byte()?;
        if attrs & 1 != 0 {
            self.count()?; // object id
        }
        if attrs & 4 != 0 {
            let n = self.count()? as usize;
            self.c.take(n)?; // type name
        }
        if attrs & 8 != 0 {
            let n = self.count()? as usize;
            self.c.take(n)?; // label
        }
        Ok(())
    }

    pub fn end(&mut self) -> io::Result<()> {
        if self.c.byte()? != END {
            return Err(bad("expected the end of a node"));
        }
        Ok(())
    }

    pub fn data(&mut self) -> io::Result<&'a [u8]> {
        self.begin()?;
        if self.c.byte()? != DATA {
            return Err(bad("expected data"));
        }
        let n = self.count()? as usize;
        let d = self.c.take(n)?;
        self.end()?;
        Ok(d)
    }

    pub fn raw(&mut self, n: usize) -> io::Result<&'a [u8]> {
        self.c.take(n)
    }

    pub fn get<T: Sf>(&mut self) -> io::Result<T> {
        T::get(self)
    }

    pub fn at_end(&self) -> bool {
        self.c.rest().is_empty()
    }
}

/// A value RCF can carry.
pub trait Sf: Sized {
    fn put(&self, w: &mut Writer);
    fn get(r: &mut Reader) -> io::Result<Self>;
}

macro_rules! fundamental {
    ($($t:ty),*) => {$(
        impl Sf for $t {
            fn put(&self, w: &mut Writer) {
                w.data(&self.to_le_bytes());
            }
            fn get(r: &mut Reader) -> io::Result<Self> {
                let d = r.data()?;
                Ok(<$t>::from_le_bytes(d.try_into().map_err(|_| bad("value size"))?))
            }
        }
    )*};
}
fundamental!(i8, u8, i16, u16, i32, u32, i64, u64, f32, f64);

impl Sf for bool {
    fn put(&self, w: &mut Writer) {
        w.data(&[*self as u8]);
    }
    fn get(r: &mut Reader) -> io::Result<Self> {
        Ok(r.data()?.first().copied().ok_or_else(|| bad("value size"))? != 0)
    }
}

impl Sf for String {
    fn put(&self, w: &mut Writer) {
        w.begin();
        (self.len() as u32).put(w);
        w.buf.extend_from_slice(self.as_bytes());
        w.end();
    }
    fn get(r: &mut Reader) -> io::Result<Self> {
        r.begin()?;
        let n = r.get::<u32>()? as usize;
        let s = String::from_utf8_lossy(r.raw(n)?).into_owned();
        r.end()?;
        Ok(s)
    }
}

/// `std::vector<unsigned char>` (SF's serializeVectorFast): like a string, its count's node, then
/// the raw bytes. Also what a string holding raw bytes (a Bluetooth address) is read as.
impl Sf for Vec<u8> {
    fn put(&self, w: &mut Writer) {
        w.begin();
        (self.len() as u32).put(w);
        w.buf.extend_from_slice(self);
        w.end();
    }
    fn get(r: &mut Reader) -> io::Result<Self> {
        r.begin()?;
        let n = r.get::<u32>()? as usize;
        let v = r.raw(n)?.to_vec();
        r.end()?;
        Ok(v)
    }
}

/// A struct whose RCF serialize() writes its fields in order (`ar & a & b & c`), each a node.
/// Enums travel as i32.
#[macro_export]
macro_rules! sf_struct {
    ($(#[$m:meta])* $name:ident { $($f:ident : $t:ty),* $(,)? }) => {
        $(#[$m])*
        #[derive(Debug, Clone, Default, PartialEq)]
        pub struct $name { $(pub $f: $t),* }
        impl $crate::sf::Sf for $name {
            fn put(&self, w: &mut $crate::sf::Writer) {
                w.begin();
                $($crate::sf::Sf::put(&self.$f, w);)*
                w.end();
            }
            fn get(r: &mut $crate::sf::Reader) -> std::io::Result<Self> {
                r.begin()?;
                let v = $name { $($f: r.get::<$t>()?),* };
                r.end()?;
                Ok(v)
            }
        }
    };
}
