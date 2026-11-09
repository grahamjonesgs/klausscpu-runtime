//! hello: runs on the board as a Zephyr extension.
//!
//! Build and run it from VS Code (Tasks: Run Task -> "klausscpu: Rust: run on
//! board") or with `tools/klauss-rust run rust/hello`.  On the board, after
//! that: `run rust/hello.llext`.
//!
//! `no_std`: `core` and `alloc` (Vec, String, BTreeMap, Box, format!) are
//! there; `std` (files, threads, the network) is not.

#![no_std]

extern crate alloc;

use alloc::collections::BTreeMap;
use alloc::vec::Vec;
use klauss_io::println;

klauss_llext_rt::entry!(run);

/// The program.  The value it returns is the exit code `run` prints.
fn run() -> i32 {
    println!("Hello from Rust on KlaussCPU!");

    let squares: Vec<u32> = (1..=10).map(|n| n * n).collect();
    println!("squares   = {:?}", squares);
    println!("sum       = {}", squares.iter().sum::<u32>());

    let mut letters = BTreeMap::new();
    for c in "hello from klausscpu".chars().filter(|c| c.is_alphabetic()) {
        *letters.entry(c).or_insert(0) += 1;
    }
    println!("letters   = {:?}", letters);
    0
}
