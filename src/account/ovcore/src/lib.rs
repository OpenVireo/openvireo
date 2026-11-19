// Copyright (C) 2026 Bongani Mbigi
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 2 or (at
// your option) version 3 of the License.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// General Public License for more details.

//! The desktop's link to openvireo-core: the core's C++ binding (ov-ffi-cpp, `<ovcore.h>`), linked into
//! one static library. Its `extern "C"` symbols and its C++ half come from the dependency.

pub use ov_ffi_cpp;
