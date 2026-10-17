#  Copyright (C) 2026 KeePassXC Team <team@keepassxc.org>
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

# Finds the libopaque C core with liboprf and libsodium, used for OPAQUE sign-in to KeePassXC accounts.
# Set OPAQUE_ROOT (or CMAKE_PREFIX_PATH) to the prefix they were installed to.
#
#   OPAQUE_FOUND          - all three libraries and their headers were found
#   OPAQUE_INCLUDE_DIRS   - include directories (opaque.h, oprf/toprf.h, sodium.h)
#   OPAQUE_LIBRARIES      - libopaque, liboprf and libsodium, in link order

find_path(OPAQUE_INCLUDE_DIR NAMES opaque.h HINTS ${OPAQUE_ROOT} ENV OPAQUE_ROOT PATH_SUFFIXES include)
find_path(OPRF_INCLUDE_DIR NAMES oprf/toprf.h HINTS ${OPAQUE_ROOT} ENV OPAQUE_ROOT PATH_SUFFIXES include)
find_path(SODIUM_INCLUDE_DIR NAMES sodium.h HINTS ${OPAQUE_ROOT} ENV OPAQUE_ROOT PATH_SUFFIXES include)

find_library(OPAQUE_LIBRARY NAMES opaque HINTS ${OPAQUE_ROOT} ENV OPAQUE_ROOT PATH_SUFFIXES lib)
find_library(OPRF_LIBRARY NAMES oprf HINTS ${OPAQUE_ROOT} ENV OPAQUE_ROOT PATH_SUFFIXES lib)
find_library(SODIUM_LIBRARY NAMES sodium libsodium HINTS ${OPAQUE_ROOT} ENV OPAQUE_ROOT PATH_SUFFIXES lib)

mark_as_advanced(OPAQUE_INCLUDE_DIR OPRF_INCLUDE_DIR SODIUM_INCLUDE_DIR OPAQUE_LIBRARY OPRF_LIBRARY SODIUM_LIBRARY)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Opaque
        REQUIRED_VARS OPAQUE_LIBRARY OPRF_LIBRARY SODIUM_LIBRARY OPAQUE_INCLUDE_DIR OPRF_INCLUDE_DIR SODIUM_INCLUDE_DIR)

if(OPAQUE_FOUND)
    set(OPAQUE_INCLUDE_DIRS ${OPAQUE_INCLUDE_DIR} ${OPRF_INCLUDE_DIR} ${SODIUM_INCLUDE_DIR})
    list(REMOVE_DUPLICATES OPAQUE_INCLUDE_DIRS)
    set(OPAQUE_LIBRARIES ${OPAQUE_LIBRARY} ${OPRF_LIBRARY} ${SODIUM_LIBRARY})
endif()
