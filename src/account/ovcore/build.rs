// Generates the C++ half of the `ovd` bridge. With OV_CXX_INCLUDE_DIR set (by CMake), it also writes
// the headers there, next to the ones ov-ffi-cpp's build script exports:
//
//   $OV_CXX_INCLUDE_DIR/ovdesktop.h                 include this (namespace ovd, and rust::Error)
//   $OV_CXX_INCLUDE_DIR/ov-desktop/src/lib.rs.h     generated bridge
//   $OV_CXX_INCLUDE_DIR/rust/cxx.h                  cxx runtime (also from ov-ffi-cpp)

use std::env;
use std::fs;
use std::path::{Path, PathBuf};

fn main() {
    cxx_build::bridge("src/lib.rs").std("c++17").compile("ov-desktop-bridge");
    println!("cargo:rerun-if-changed=src/lib.rs");
    println!("cargo:rerun-if-env-changed=OV_CXX_INCLUDE_DIR");

    if let Some(dest) = env::var_os("OV_CXX_INCLUDE_DIR") {
        let dest = Path::new(&dest);
        let out = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR"));
        let generated = out.join("cxxbridge").join("include");
        copy(&generated.join("ov-desktop/src/lib.rs.h"), &dest.join("ov-desktop/src/lib.rs.h"));
        copy(&generated.join("rust/cxx.h"), &dest.join("rust/cxx.h"));
        fs::write(dest.join("ovdesktop.h"), "#pragma once\n#include \"rust/cxx.h\"\n#include \"ov-desktop/src/lib.rs.h\"\n")
            .unwrap_or_else(|e| panic!("writing {}/ovdesktop.h: {e}", dest.display()));
    }
}

fn copy(from: &Path, to: &Path) {
    fs::create_dir_all(to.parent().unwrap()).unwrap_or_else(|e| panic!("creating {}: {e}", to.display()));
    fs::copy(from, to).unwrap_or_else(|e| panic!("copying {} to {}: {e}", from.display(), to.display()));
}
