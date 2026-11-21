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
# static library of src/account/ovcore, a wrapper around the core's C++ binding (ov-ffi-cpp) that
# pins it to one git commit (its Cargo.toml and Cargo.lock), so builds are reproducible; cargo
# downloads the sources on the first build (or `cargo vendor` them for offline builds).
#
# Defines:
#   ovcore              - interface target: link it to get the library and its headers
#   OVCORE_INCLUDE_DIR  - where the generated headers go: <ovcore.h> (namespace ov: the core facade,
#                         the Host interfaces and the crypto, OPAQUE and merge functions below it)
#
# Options:
#   OPENVIREO_CORE_SOURCE_DIR - a local openvireo-core checkout to build instead of the pinned commit,
#                               for working on the core and the desktop together. Never for releases.

# Corrosion builds one Rust target per configure. A macOS build for one architecture (release-tool.py sets
# CMAKE_OSX_ARCHITECTURES) builds the core for it; that target needs `rustup target add <triple>`.
if(APPLE AND NOT Rust_CARGO_TARGET)
    list(LENGTH CMAKE_OSX_ARCHITECTURES _ovcore_arch_count)
    if(_ovcore_arch_count GREATER 1)
        message(FATAL_ERROR "openvireo-core builds for one macOS architecture per configure; set CMAKE_OSX_ARCHITECTURES to one of them")
    elseif(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
        set(Rust_CARGO_TARGET "x86_64-apple-darwin" CACHE STRING "Rust target triple")
    elseif(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
        set(Rust_CARGO_TARGET "aarch64-apple-darwin" CACHE STRING "Rust target triple")
    endif()
endif()

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
    list(APPEND _ovcore_flags "--config"
         "patch.'https://github.com/openvireo/openvireo-core'.ov-ffi-cpp.path='${OPENVIREO_CORE_SOURCE_DIR}/bindings/ov-ffi-cpp'")
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
# ov-ffi-cpp's build script writes the headers to a staging folder; ovcore_headers below copies the ones
# that changed into OVCORE_INCLUDE_DIR
set(_ovcore_staging_dir "${CMAKE_BINARY_DIR}/ovcore-include-cargo")
set(_ovcore_env "OV_CXX_INCLUDE_DIR=${_ovcore_staging_dir}")
# aes 0.8 uses the ARMv8 AES instructions only with this cfg. openvireo-core sets it in its
# .cargo/config.toml, which cargo doesn't read here; without it AES-KDF runs ~30x slower. The
# per-target RUSTFLAGS reach every crate of the target, not only the top one.
if(Rust_CARGO_TARGET_ARCH STREQUAL "aarch64")
    string(TOUPPER "${Rust_CARGO_TARGET_CACHED}" _ovcore_triple)
    string(REPLACE "-" "_" _ovcore_triple "${_ovcore_triple}")
    list(APPEND _ovcore_env "CARGO_TARGET_${_ovcore_triple}_RUSTFLAGS=--cfg aes_armv8")
endif()
if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
    list(APPEND _ovcore_env "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
corrosion_set_env_vars(ov_desktop ${_ovcore_env})

# Cargo rewrites the headers when the pin changes, behind Ninja's back: Ninja stats them before cargo runs
# and wouldn't rebuild what includes them until the next build. Declaring them as the byproducts of a
# step that runs after cargo makes Ninja look at them again once it has, so one build is enough. Copying
# only what changed (copy_if_different) keeps a rerun of cargo with the same headers from rebuilding
# every file that includes <ovcore.h>, and a `clean` that removes them gets them back from the staging
# folder without cargo.
set(_ovcore_headers ovcore.h rust/cxx.h ov-ffi-cpp/src/lib.rs.h ov-ffi-cpp/include/host.h ov-ffi-cpp/include/ovcore.h)
set(_ovcore_header_outputs)
set(_ovcore_header_commands)
foreach(_ovcore_header IN LISTS _ovcore_headers)
    get_filename_component(_ovcore_header_dir "${OVCORE_INCLUDE_DIR}/${_ovcore_header}" DIRECTORY)
    list(APPEND _ovcore_header_outputs "${OVCORE_INCLUDE_DIR}/${_ovcore_header}")
    list(APPEND _ovcore_header_commands
         COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ovcore_header_dir}"
         COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_ovcore_staging_dir}/${_ovcore_header}"
                 "${OVCORE_INCLUDE_DIR}/${_ovcore_header}")
endforeach()
add_custom_target(ovcore_headers
        ${_ovcore_header_commands}
        BYPRODUCTS ${_ovcore_header_outputs}
        COMMENT "Updating the openvireo-core headers"
        VERBATIM)
add_dependencies(ovcore_headers cargo-build_ov_desktop)

file(MAKE_DIRECTORY "${OVCORE_INCLUDE_DIR}")
add_library(ovcore INTERFACE)
target_include_directories(ovcore SYSTEM INTERFACE "${OVCORE_INCLUDE_DIR}")
target_link_libraries(ovcore INTERFACE ov_desktop)
if(APPLE)
    target_link_libraries(ovcore INTERFACE "-framework Security" "-framework CoreFoundation")
endif()
# The headers exist once cargo has run and ovcore_headers has copied them
add_dependencies(ovcore cargo-build_ov_desktop ovcore_headers)
