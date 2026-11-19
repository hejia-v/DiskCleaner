cmake_minimum_required(VERSION 3.21)
file(REAL_PATH "${CMAKE_SOURCE_DIR}/build" expected_build)
file(REAL_PATH "${CMAKE_BINARY_DIR}" actual_build)
string(TOLOWER "${expected_build}" expected_build)
string(TOLOWER "${actual_build}" actual_build)
if(NOT actual_build STREQUAL expected_build)
    message(FATAL_ERROR "Configure DiskCleaner in its shared build directory: cmake -S . -B build")
endif()
project(DiskCleaner VERSION 0.1 LANGUAGES NONE)

if(NOT WIN32)
    message(FATAL_ERROR "DiskCleaner requires Windows")
endif()
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()

find_program(CARGO_EXECUTABLE cargo REQUIRED)
# Ship the MSVC runtime imported by the Windows Rust toolchain alongside the executable.
file(GLOB msvc_runtimes "$ENV{ProgramFiles}/Microsoft Visual Studio/*/*/VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT/vcruntime140.dll")
list(SORT msvc_runtimes COMPARE NATURAL ORDER DESCENDING)
if(msvc_runtimes)
    list(GET msvc_runtimes 0 msvc_runtime)
else()
    find_file(msvc_runtime vcruntime140.dll HINTS "$ENV{SystemRoot}/System32" REQUIRED)
endif()
set(cargo_target "${CMAKE_SOURCE_DIR}/build/cargo")
set(cargo_profile "$<IF:$<CONFIG:Debug>,dev,release>")
set(cargo_output "$<IF:$<CONFIG:Debug>,debug,release>")
add_custom_target(DiskCleaner ALL
    COMMAND "${CARGO_EXECUTABLE}" build --locked --profile "${cargo_profile}" --target-dir "${cargo_target}"
    COMMAND "${CMAKE_COMMAND}"
        "-DAPP=${cargo_target}/${cargo_output}/DiskCleaner.exe"
        "-DRUNTIME=${msvc_runtime}"
        "-DPACKAGE_DIR=$<TARGET_GENEX_EVAL:DiskCleaner,$<TARGET_PROPERTY:DiskCleaner,RUNTIME_OUTPUT_DIRECTORY>>"
        -P "${CMAKE_SOURCE_DIR}/tools/deploy.cmake"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    USES_TERMINAL VERBATIM)
set_target_properties(DiskCleaner PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist/$<CONFIG>")

option(DISK_CLEANER_BUILD_TESTS "Enable Rust regression tests" ON)
if(DISK_CLEANER_BUILD_TESTS)
    enable_testing()
    add_test(NAME rust-regressions COMMAND "${CARGO_EXECUTABLE}" test --locked --profile "${cargo_profile}"
        --manifest-path "${CMAKE_SOURCE_DIR}/Cargo.toml" --target-dir "${cargo_target}")
    add_test(NAME ntfs-selftest COMMAND "${CMAKE_SOURCE_DIR}/dist/$<CONFIG>/DiskCleaner.exe" --selftest)
    add_test(NAME distribution-contents COMMAND "${CMAKE_COMMAND}"
        "-DPACKAGE_DIR=${CMAKE_SOURCE_DIR}/dist/$<CONFIG>" -P "${CMAKE_SOURCE_DIR}/tools/check_package.cmake")
endif()


