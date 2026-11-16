// SPDX-License-Identifier: GPL-3.0-or-later
//
// The usage text is the C++ shim's, byte for byte (aa-carlife-shim/src/shim.cpp, usage()).

use std::path::PathBuf;

// The concatenated string literals of usage()'s fprintf, unescaped (\n and \").
fn cpp_usage() -> String {
    let p = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../../../aa-carlife-shim/src/shim.cpp");
    let src = std::fs::read_to_string(p).unwrap();
    let start = src.find("void usage() {").unwrap();
    let body = &src[start..];
    let body = &body[..body.find("\n}\n").unwrap()];
    let mut out = String::new();
    for line in body.lines().skip(2) {
        let l = line.trim();
        let Some(l) = l.strip_prefix('"') else { continue };
        let end = l.rfind('"').unwrap();
        out.push_str(&l[..end].replace("\\n", "\n").replace("\\\"", "\""));
    }
    out
}

#[test]
fn usage_is_the_cpp_shims() {
    assert_eq!(livi_carlife_shim::usage_text(), cpp_usage());
}
