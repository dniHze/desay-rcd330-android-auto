// SPDX-License-Identifier: GPL-3.0-or-later
//
// aa-carlife-shim/src/appc_patch.hpp stays the source of truth for the appconnect patch tables
// (kzb-re/patch_appconnect.py bakes the image's appconnect from it): the Rust tables
// (src/appc_tables.rs) must equal it, every guard and edit word and every variant, in order.

use std::collections::HashMap;
use std::path::PathBuf;

use livi_carlife_shim::appc::{Edit, Word, PATCHES};

fn header() -> String {
    let p = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../../../aa-carlife-shim/src/appc_patch.hpp");
    std::fs::read_to_string(&p).unwrap_or_else(|e| panic!("{}: {e}", p.display()))
}

fn hex(s: &str) -> u64 {
    u64::from_str_radix(s.trim().trim_start_matches("0x"), 16).unwrap_or_else(|_| panic!("not hex: {s}"))
}

// The arrays `const Word kName[] = { {a, w}, ... };` and `const Edit kName[] = { {a, s, p}, ... };`.
fn arrays(h: &str) -> (HashMap<String, Vec<Word>>, HashMap<String, Vec<Edit>>) {
    let (mut words, mut edits) = (HashMap::new(), HashMap::new());
    let mut cur: Option<(bool, String)> = None;
    for line in h.lines() {
        let l = line.trim();
        if let Some(rest) = l.strip_prefix("const Word ").or_else(|| l.strip_prefix("const Edit "))
            && let Some(name) = rest.strip_suffix("[] = {") {
                let is_word = l.starts_with("const Word ");
                cur = Some((is_word, name.to_string()));
                if is_word {
                    words.insert(name.to_string(), Vec::new());
                } else {
                    edits.insert(name.to_string(), Vec::new());
                }
                continue;
            }
        if l == "};" {
            cur = None;
            continue;
        }
        let Some((is_word, name)) = &cur else { continue };
        let Some(body) = l.strip_prefix('{').and_then(|b| b.split_once('}')).map(|(b, _)| b) else { continue };
        let f: Vec<&str> = body.split(',').collect();
        if *is_word {
            assert_eq!(f.len(), 2, "{line}");
            words.get_mut(name).unwrap().push(Word { addr: hex(f[0]), word: hex(f[1]) as u32 });
        } else {
            assert_eq!(f.len(), 3, "{line}");
            edits.get_mut(name).unwrap().push(Edit { addr: hex(f[0]), stock: hex(f[1]) as u32, patched: hex(f[2]) as u32 });
        }
    }
    (words, edits)
}

#[test]
fn the_rust_tables_equal_appc_patch_hpp() {
    let h = header();
    let (words, edits) = arrays(&h);
    let whats: HashMap<String, String> = h
        .lines()
        .filter_map(|l| {
            let rest = l.trim().strip_prefix("const char* const ")?;
            let (name, v) = rest.split_once(" = \"")?;
            Some((name.to_string(), v.strip_suffix("\";")?.to_string()))
        })
        .collect();
    // APPC_P("callfix", "0358", kCallfixWhat, kCallfixGuards, kCallfixEdits),
    let patches: Vec<Vec<String>> = h
        .lines()
        .filter_map(|l| l.trim().strip_prefix("APPC_P(")?.strip_suffix("),").map(|b| b.split(", ").map(|s| s.trim_matches('"').to_string()).collect()))
        .collect();
    assert_eq!(patches.len(), PATCHES.len(), "the number of variants");
    for (p, ours) in patches.iter().zip(PATCHES) {
        let what = format!("{} {}", p[0], p[1]);
        assert_eq!((p[0].as_str(), p[1].as_str()), (ours.name, ours.build), "{what}: name and build");
        assert_eq!(whats[&p[2]], ours.what, "{what}: what");
        assert_eq!(words[&p[3]].as_slice(), ours.guards, "{what}: guard words ({})", p[3]);
        assert_eq!(edits[&p[4]].as_slice(), ours.edits, "{what}: edits ({})", p[4]);
    }
}
