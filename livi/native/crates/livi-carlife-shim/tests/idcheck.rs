// SPDX-License-Identifier: GPL-3.0-or-later
//
// The method-ID check of aa-carlife-shim/build.sh, for the Rust tables: every method of every
// interface (idl/*.rs: its fnId, name and parameter types, transcribed from the C++ IDL
// headers) must equal tools/stock_ids.txt, the table read from the stock binaries,
// line for line.

use std::collections::BTreeSet;
use std::path::PathBuf;

use livi_carlife_shim::idl::TABLES;

fn tools() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../../../aa-carlife-shim/tools")
}

#[test]
fn method_ids_equal_the_stock_table() {
    let stock = std::fs::read_to_string(tools().join("stock_ids.txt")).expect("tools/stock_ids.txt");
    let stock: BTreeSet<String> = stock.lines().filter(|l| !l.is_empty()).map(str::to_string).collect();
    let mut ours = BTreeSet::new();
    for (iface, methods) in TABLES {
        for m in *methods {
            assert!(ours.insert(format!("{}\t{}\t{}\t{}", iface, m.id, m.name, m.sig)), "{iface} {} twice", m.name);
        }
    }
    let missing: Vec<_> = stock.difference(&ours).collect();
    let extra: Vec<_> = ours.difference(&stock).collect();
    assert!(missing.is_empty() && extra.is_empty(), "not in ours: {missing:#?}\nnot in stock: {extra:#?}");
    assert_eq!(ours.len(), 216);
}

#[test]
fn every_interface_numbers_its_methods_once() {
    for (iface, methods) in TABLES {
        let ids: BTreeSet<i32> = methods.iter().map(|m| m.id).collect();
        assert_eq!(ids.len(), methods.len(), "{iface}: a fnId twice");
    }
}
