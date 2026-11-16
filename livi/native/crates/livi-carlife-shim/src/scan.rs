// SPDX-License-Identifier: GPL-3.0-or-later
//
// The few sscanf() patterns the C++ shim reads lines with ("hello %u", "audio %15s %d",
// "x,y,w,h" ...), with sscanf's rules: a space in the pattern takes any whitespace, or none;
// %d / %u skip whitespace and take an optional sign; %Ns takes up to N non-whitespace bytes.

pub struct Scan<'a> {
    s: &'a [u8],
    pos: usize,
}

impl<'a> Scan<'a> {
    pub fn new(s: &'a str) -> Self {
        Scan { s: s.as_bytes(), pos: 0 }
    }

    pub fn ws(&mut self) -> &mut Self {
        while self.pos < self.s.len() && self.s[self.pos].is_ascii_whitespace() {
            self.pos += 1;
        }
        self
    }

    /// A literal: matched byte for byte.
    pub fn lit(&mut self, l: &str) -> Option<()> {
        if self.s[self.pos..].starts_with(l.as_bytes()) {
            self.pos += l.len();
            Some(())
        } else {
            None
        }
    }

    fn number(&mut self) -> Option<(bool, u64)> {
        self.ws();
        let mut neg = false;
        if let Some(&c) = self.s.get(self.pos)
            && (c == b'+' || c == b'-') {
                neg = c == b'-';
                self.pos += 1;
            }
        let start = self.pos;
        let mut v: u64 = 0;
        while let Some(&c) = self.s.get(self.pos) {
            if !c.is_ascii_digit() {
                break;
            }
            v = v.saturating_mul(10).saturating_add((c - b'0') as u64);
            self.pos += 1;
        }
        if self.pos == start { None } else { Some((neg, v)) }
    }

    /// %d (an out-of-range value is cut to 32 bits, as glibc stores it).
    pub fn int(&mut self) -> Option<i32> {
        let (neg, v) = self.number()?;
        let v = v as i64;
        Some(if neg { (v.wrapping_neg()) as i32 } else { v as i32 })
    }

    /// %u (a negative value wraps, as sscanf's does).
    pub fn uint(&mut self) -> Option<u32> {
        let (neg, v) = self.number()?;
        let v = v as u32;
        Some(if neg { v.wrapping_neg() } else { v })
    }

    /// %Ns.
    pub fn word(&mut self, max: usize) -> Option<String> {
        self.ws();
        let start = self.pos;
        while self.pos < self.s.len() && self.pos - start < max && !self.s[self.pos].is_ascii_whitespace() {
            self.pos += 1;
        }
        if self.pos == start {
            return None;
        }
        Some(String::from_utf8_lossy(&self.s[start..self.pos]).into_owned())
    }
}

/// sscanf(line, "<prefix> %u"): the number after the literal prefix.
pub fn prefixed_uint(line: &str, prefix: &str) -> Option<u32> {
    let mut s = Scan::new(line);
    s.lit(prefix)?;
    s.ws().uint()
}

/// sscanf(line, "<prefix> %d").
pub fn prefixed_int(line: &str, prefix: &str) -> Option<i32> {
    let mut s = Scan::new(line);
    s.lit(prefix)?;
    s.ws().int()
}

/// strtol(v, &end, 10) with the whole string taken: Some(value) only when it is all a number
/// (leading whitespace and a sign allowed) that fits a long.
pub fn strtol_whole(v: &str) -> Option<i64> {
    let t = v.trim_start_matches(|c: char| c.is_ascii_whitespace());
    let (neg, digits) = match t.as_bytes().first() {
        Some(b'-') => (true, &t[1..]),
        Some(b'+') => (false, &t[1..]),
        _ => (false, t),
    };
    if digits.is_empty() || !digits.bytes().all(|c| c.is_ascii_digit()) {
        return None;
    }
    let n: i64 = digits.parse().ok()?;
    let n = if neg { -n } else { n };
    // A long is 32 bits on the unit.
    if n < i32::MIN as i64 || n > i32::MAX as i64 { None } else { Some(n) }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn patterns_read_as_sscanf_reads_them() {
        assert_eq!(prefixed_uint("hello 2", "hello"), Some(2));
        assert_eq!(prefixed_uint("hello2", "hello"), Some(2));
        assert_eq!(prefixed_uint("hello x", "hello"), None);
        assert_eq!(prefixed_uint("pong 7", "pong"), Some(7));
        assert_eq!(prefixed_int("mic 1", "mic"), Some(1));
        assert_eq!(prefixed_int("mic -1", "mic"), Some(-1));
        let mut s = Scan::new("audio media 1");
        assert!(s.lit("audio").is_some());
        assert_eq!(s.word(15).as_deref(), Some("media"));
        assert_eq!(s.int(), Some(1));
        assert_eq!(strtol_whole(" 42"), Some(42));
        assert_eq!(strtol_whole("4x"), None);
        assert_eq!(strtol_whole(""), None);
        assert_eq!(strtol_whole("99999999999"), None);
    }
}
