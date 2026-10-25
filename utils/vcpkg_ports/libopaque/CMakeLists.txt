cmake_minimum_required(VERSION 3.16)
project(libopaque C)

# Builds the same libraries and headers as .github/scripts/build-opaque.sh, which cmake/FindOpaque.cmake finds.
# libopaque only needs oprf.c and toprf.c from liboprf; the rest of liboprf needs POSIX headers.

option(DISABLE_INSTALL_HEADERS "Skip installing headers" OFF)
set(OPRF_SOURCE_DIR "" CACHE PATH "liboprf source directory")

find_path(SODIUM_INCLUDE_DIR sodium.h REQUIRED)

set(CMAKE_C_STANDARD 99)
# libsodium is 1.0.19 or newer, so neither library needs its bundled HKDF fallback
add_compile_definitions(HAVE_SODIUM_HKDF=1)

# opaque.c includes <oprf/oprf.h>, as installed
file(GLOB OPRF_HEADERS "${OPRF_SOURCE_DIR}/src/*.h")
file(COPY ${OPRF_HEADERS} DESTINATION "${CMAKE_BINARY_DIR}/include/oprf")

add_library(oprf STATIC "${OPRF_SOURCE_DIR}/src/oprf.c" "${OPRF_SOURCE_DIR}/src/toprf.c")
target_include_directories(oprf PRIVATE "${SODIUM_INCLUDE_DIR}")
if(WIN32)
    # toprf.c includes <arpa/inet.h> for htons
    target_include_directories(oprf PRIVATE compat)
endif()

add_library(opaque STATIC src/opaque.c src/common.c)
target_include_directories(opaque PRIVATE "${CMAKE_BINARY_DIR}/include" "${SODIUM_INCLUDE_DIR}")

install(TARGETS oprf opaque ARCHIVE DESTINATION lib)
if(NOT DISABLE_INSTALL_HEADERS)
    install(FILES ${OPRF_HEADERS} DESTINATION include/oprf)
    install(FILES src/opaque.h DESTINATION include)
endif()
