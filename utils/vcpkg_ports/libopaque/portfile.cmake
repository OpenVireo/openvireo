# Pins follow docs/plans/accounts/SPEC.md §8, like .github/scripts/build-opaque.sh
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH OPRF_SOURCE_PATH
    REPO stef/liboprf
    REF f925b87759d06ac97abc5b926fe0e7d0668a05ce
    SHA512 9c288b918a6b1ebb5954d51538812bc414e69bf27900e9b4525fdc39d34baa826c5cea745d35b6e15a50a8fc0e786a0f6fce548a732df1424b73dcbe68e06050
    HEAD_REF master
)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO stef/libopaque
    REF 6e9ac92f9a2289679e04b0b0c5fdc307bb3de54e
    SHA512 03c377ff28326d3e9dc883f66b27503aef89f4747bb1d92ec8592ed1a9fec209dffc30f97f2d79e13722914b56a5bbac264055c72d8959ea1847f0843b6c5ce4
    HEAD_REF master
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" "${CMAKE_CURRENT_LIST_DIR}/compat" DESTINATION "${SOURCE_PATH}")

set(OPTIONS "-DOPRF_SOURCE_DIR=${OPRF_SOURCE_PATH}")
if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
    # Both libraries use C99 variable length arrays, which MSVC does not support. clang-cl does,
    # and its libraries link into MSVC builds.
    find_program(CLANG_CL clang-cl PATHS "$ENV{VCINSTALLDIR}/Tools/Llvm/x64/bin" "$ENV{ProgramFiles}/LLVM/bin")
    if(NOT CLANG_CL)
        message(FATAL_ERROR "clang-cl not found: install the \"C++ Clang tools for Windows\" component of Visual Studio")
    endif()
    list(APPEND OPTIONS "-DCMAKE_C_COMPILER=${CLANG_CL}")
    if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
        list(APPEND OPTIONS -DCMAKE_C_COMPILER_TARGET=aarch64-pc-windows-msvc)
    endif()
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${OPTIONS}
    OPTIONS_DEBUG
        -DDISABLE_INSTALL_HEADERS=ON
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
