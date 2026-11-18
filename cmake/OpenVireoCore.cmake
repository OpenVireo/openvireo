#  Copyright (C) 2026 Bongani Mbigi
#
#  This program is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 2 or (at your option)
#  version 3 of the License.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>.

# Builds openvireo-core (github.com/openvireo/openvireo-core) from source with Corrosion, as the
# static library of src/account/ovcore. That crate pins every core crate to one git commit (its
# Cargo.toml and Cargo.lock), so builds are reproducible; cargo downloads the sources on the first
# build (or `cargo vendor` them for offline builds).
#
# Defines:
#   ovcore              - interface target: link it to get the library and its headers
#   OVCORE_INCLUDE_DIR  - where the generated headers go: <ovcore.h> (namespace ov, the core facade
#                         and the Host interfaces) and <ovdesktop.h> (namespace ovd)
#
# Options:
#   OPENVIREO_CORE_SOURCE_DIR - a local openvireo-core checkout to build instead of the pinned commit,
#                               for working on the core and the desktop together. Never for releases.

include(FetchContent)
FetchContent_Declare(Corrosion
        GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
        GIT_TAG 1499b14e4906a2890f5cee1547c8848db261753d # v0.6.1
)
FetchContent_MakeAvailable(Corrosion)

set(OPENVIREO_CORE_SOURCE_DIR "" CACHE PATH "Local openvireo-core checkout to build instead of the pinned commit (development only)")

set(_ovcore_manifest "${CMAKE_SOURCE_DIR}/src/account/ovcore/Cargo.toml")
set(_ovcore_args)
if(OPENVIREO_CORE_SOURCE_DIR)
    # Replace the pinned git sources with the checkout; the lock file no longer matches, so no --locked
    message(WARNING "Building openvireo-core from ${OPENVIREO_CORE_SOURCE_DIR}, not the pinned commit")
    set(_ovcore_flags)
    foreach(_crate ov-ffi-cpp ov-crypto ov-error ov-kdbx ov-merge ov-opaque)
        set(_dir "${OPENVIREO_CORE_SOURCE_DIR}/crates/${_crate}")
        if(_crate STREQUAL "ov-ffi-cpp")
            set(_dir "${OPENVIREO_CORE_SOURCE_DIR}/bindings/${_crate}")
        endif()
        list(APPEND _ovcore_flags "--config" "patch.'https://github.com/openvireo/openvireo-core'.${_crate}.path='${_dir}'")
    endforeach()
    list(APPEND _ovcore_args FLAGS ${_ovcore_flags})
else()
    list(APPEND _ovcore_args LOCKED)
endif()
if(WITH_TESTS)
    # The tests stand in for the auth server with opaque-ke's server half
    list(APPEND _ovcore_args FEATURES test-server)
endif()

corrosion_import_crate(
        MANIFEST_PATH "${_ovcore_manifest}"
        CRATES ov-desktop
        CRATE_TYPES staticlib
        ${_ovcore_args}
)

set(OVCORE_INCLUDE_DIR "${CMAKE_BINARY_DIR}/ovcore-include")
set(_ovcore_env "OV_CXX_INCLUDE_DIR=${OVCORE_INCLUDE_DIR}")
# aes 0.8 uses the ARMv8 AES instructions only with this cfg. openvireo-core sets it in its
# .cargo/config.toml, which cargo doesn't read here; without it AES-KDF runs ~30x slower. The
# per-target RUSTFLAGS reach every crate of the target, not only the top one.
if(Rust_CARGO_TARGET_ARCH STREQUAL "aarch64")
    string(TOUPPER "${Rust_CARGO_TARGET_CACHED}" _ovcore_triple)
    string(REPLACE "-" "_" _ovcore_triple "${_ovcore_triple}")
    list(APPEND _ovcore_env "CARGO_TARGET_${_ovcore_triple}_RUSTFLAGS=--cfg aes_armv8")
endif()
corrosion_set_env_vars(ov_desktop ${_ovcore_env})

file(MAKE_DIRECTORY "${OVCORE_INCLUDE_DIR}")
add_library(ovcore INTERFACE)
target_include_directories(ovcore SYSTEM INTERFACE "${OVCORE_INCLUDE_DIR}")
target_link_libraries(ovcore INTERFACE ov_desktop)
if(APPLE)
    target_link_libraries(ovcore INTERFACE "-framework Security" "-framework CoreFoundation")
endif()
# The headers exist once cargo has run
add_dependencies(ovcore cargo-build_ov_desktop)
