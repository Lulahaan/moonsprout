//! Join codes, secrets, tokens and player names.
//!
//! A join code is 8 Crockford base32 symbols (40 bits from the OS RNG), shown "ABCD-EFGH". Input is
//! read in any case with spaces and hyphens ignored; I and L read as 1, O as 0; U is invalid.

use rand::rngs::OsRng;
use rand::TryRngCore;

pub const ALPHABET: &[u8; 32] = b"0123456789ABCDEFGHJKMNPQRSTVWXYZ";
pub const CODE_LEN: usize = 8;
pub const NAME_MAX_CHARS: usize = 24;

/// Fills `buf` from the operating system's RNG.
pub fn os_random(buf: &mut [u8]) {
    OsRng.try_fill_bytes(buf).expect("the OS random number generator failed");
}

/// 8 canonical symbols, no hyphen.
pub fn generate_code() -> String {
    let mut b = [0u8; 5];
    os_random(&mut b);
    let mut v: u64 = 0;
    for x in b {
        v = (v << 8) | u64::from(x);
    }
    (0..CODE_LEN).map(|i| ALPHABET[((v >> (35 - 5 * i)) & 31) as usize] as char).collect()
}

/// "ABCDEFGH" -> "ABCD-EFGH".
pub fn display_code(canonical: &str) -> String {
    if canonical.len() == CODE_LEN {
        format!("{}-{}", &canonical[..4], &canonical[4..])
    } else {
        canonical.to_string()
    }
}

/// The 8 canonical symbols of a code typed by a person, or None.
pub fn normalize_code(input: &str) -> Option<String> {
    if input.len() > 64 {
        return None;
    }
    let mut out = String::with_capacity(CODE_LEN);
    for c in input.chars() {
        let c = match c.to_ascii_uppercase() {
            ' ' | '-' => continue,
            'I' | 'L' => '1',
            'O' => '0',
            c if c.is_ascii() && ALPHABET.contains(&(c as u8)) => c,
            _ => return None, // U and anything outside the alphabet
        };
        if out.len() == CODE_LEN {
            return None;
        }
        out.push(c);
    }
    (out.len() == CODE_LEN).then_some(out)
}

pub fn random_hex(bytes: usize) -> String {
    let mut b = vec![0u8; bytes];
    os_random(&mut b);
    hex(&b)
}

pub fn hex(b: &[u8]) -> String {
    const D: &[u8; 16] = b"0123456789abcdef";
    let mut s = String::with_capacity(b.len() * 2);
    for x in b {
        s.push(D[(x >> 4) as usize] as char);
        s.push(D[(x & 15) as usize] as char);
    }
    s
}

/// Exactly `bytes` bytes of hex (either case).
pub fn parse_hex(s: &str, bytes: usize) -> Option<Vec<u8>> {
    if s.len() != bytes * 2 || !s.bytes().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    (0..bytes).map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).ok()).collect()
}

/// Trimmed, control characters removed, at most 24 characters; empty becomes "Player".
pub fn sanitize_name(input: &str) -> String {
    let cleaned: String = input.chars().filter(|c| !c.is_control()).collect();
    let mut out: String = cleaned.trim().chars().take(NAME_MAX_CHARS).collect();
    let trimmed_len = out.trim_end().len();
    out.truncate(trimmed_len);
    if out.is_empty() {
        "Player".to_string()
    } else {
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn generated_codes_are_canonical() {
        for _ in 0..1000 {
            let c = generate_code();
            assert_eq!(c.len(), 8);
            assert!(c.bytes().all(|b| ALPHABET.contains(&b)));
            assert_eq!(normalize_code(&c).as_deref(), Some(c.as_str()));
            assert_eq!(normalize_code(&display_code(&c)).as_deref(), Some(c.as_str()));
        }
    }

    #[test]
    fn codes_use_40_bits() {
        // Every symbol position takes many values (a constant position would mean lost bits).
        let codes: Vec<String> = (0..2000).map(|_| generate_code()).collect();
        for pos in 0..8 {
            let distinct: std::collections::HashSet<u8> = codes.iter().map(|c| c.as_bytes()[pos]).collect();
            assert!(distinct.len() > 20, "position {pos} has only {} values", distinct.len());
        }
        assert_eq!(ALPHABET.len(), 32);
        assert!(!ALPHABET.contains(&b'I') && !ALPHABET.contains(&b'L'));
        assert!(!ALPHABET.contains(&b'O') && !ALPHABET.contains(&b'U'));
    }

    #[test]
    fn normalization() {
        assert_eq!(normalize_code("abcd-efgh").as_deref(), Some("ABCDEFGH"));
        assert_eq!(normalize_code(" AbCd EfGh ").as_deref(), Some("ABCDEFGH"));
        assert_eq!(normalize_code("ilo0-1234").as_deref(), Some("11001234"));
        assert_eq!(normalize_code("IL0O-ZZZZ").as_deref(), Some("1100ZZZZ"));
        assert_eq!(normalize_code("--AB--CD--EF--GH--").as_deref(), Some("ABCDEFGH"));
        assert_eq!(normalize_code("ABCD-EFGU"), None); // U is invalid
        assert_eq!(normalize_code("ABCD-EFG"), None); // 7 symbols
        assert_eq!(normalize_code("ABCD-EFGHJ"), None); // 9 symbols
        assert_eq!(normalize_code(""), None);
        assert_eq!(normalize_code("ABCD_EFGH"), None);
        assert_eq!(normalize_code("ÄBCD-EFGH"), None);
        assert_eq!(display_code("ABCDEFGH"), "ABCD-EFGH");
    }

    #[test]
    fn hex_roundtrip() {
        let b = [0x00, 0x7f, 0x80, 0xff, 0x12];
        assert_eq!(hex(&b), "007f80ff12");
        assert_eq!(parse_hex("007F80ff12", 5).unwrap(), b);
        assert!(parse_hex("007f80ff1", 5).is_none());
        assert!(parse_hex("007f80ff1g", 5).is_none());
        assert_eq!(random_hex(16).len(), 32);
    }

    #[test]
    fn names() {
        assert_eq!(sanitize_name("  Olimar  "), "Olimar");
        assert_eq!(sanitize_name(""), "Player");
        assert_eq!(sanitize_name(" \t\n "), "Player");
        assert_eq!(sanitize_name("Lo\u{0}ui\u{7}e\r\n"), "Louie");
        assert_eq!(sanitize_name("abcdefghijklmnopqrstuvwxyz"), "abcdefghijklmnopqrstuvwx");
        assert_eq!(sanitize_name("ÄÖÜäöüßéèêëàáâãåæçñ漢字テストXY"), "ÄÖÜäöüßéèêëàáâãåæçñ漢字テスト");
        assert_eq!(sanitize_name("abcdefghijklmnopqrstuvw yz").chars().count(), 23); // trailing space trimmed
        assert_eq!(sanitize_name("\u{1b}[31mRed"), "[31mRed");
    }
}
