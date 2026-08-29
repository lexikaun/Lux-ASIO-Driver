cmake_minimum_required(VERSION 3.20)
project(LuxAsioDriver VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Shared library for the COM DLL
add_library(lux_asio SHARED
    src/dllmain.cpp
    src/lux_asio.cpp
    src/lux_asio.def
    src/wasapi_backend.cpp
    src/ks_backend.cpp
    src/audio_thread.cpp
    src/settings.cpp
    src/control_panel.cpp
    src/panel.rc
    external/asiosdk/common/asiodrvr.cpp
    external/asiosdk/common/combase.cpp
    external/asiosdk/common/dllentry.cpp
    external/asiosdk/common/register.cpp
)

# Required include directories for ASIO SDK
target_include_directories(lux_asio PRIVATE
    external/asiosdk/common
    external/asiosdk/host
    external/asiosdk/host/pc
    src
)

# Preprocessor definitions
target_compile_definitions(lux_asio PRIVATE
    WINDOWS=1
    _WINDOWS=1
    WIN32=1
    _USRDLL=1
)

# Compiler flags for MSVC
# Note: no /arch:AVX2 (a universal driver must run on any x64 CPU) and no /GS-
# (stack protection stays on; it costs nothing measurable in this code).
if(MSVC)
    # /O2: Optimize for speed, /Oi: intrinsic functions, /GL: whole program optimization
    target_compile_options(lux_asio PRIVATE
        $<$<CONFIG:Release>:/O2 /Oi /GL>
        /Zc:strictStrings-
    )
    # /LTCG for whole program optimization
    set_property(TARGET lux_asio APPEND_STRING PROPERTY LINK_FLAGS_RELEASE " /LTCG")
endif()

# Link necessary Windows libraries for COM and Registry
target_link_libraries(lux_asio PRIVATE
    ole32
    uuid
    advapi32
    user32
    avrt
    comctl32
    setupapi
)

# ---------------------------------------------------------------------------
# Test host: loads lux_asio.dll directly (no COM registration needed) and
# exercises the full ASIO lifecycle — init, buffer creation, start/stop/start,
# sample position, subset channel sets. Run after building:
#   build\Release\lux_test_host.exe build\Release\lux_asio.dll
# ---------------------------------------------------------------------------
add_executable(lux_test_host tools/test_host.cpp)
target_include_directories(lux_test_host PRIVATE
    external/asiosdk/common
    src
)
target_compile_definitions(lux_test_host PRIVATE
    WINDOWS=1
    _WINDOWS=1
    WIN32=1
)
target_link_libraries(lux_test_host PRIVATE ole32 uuid)

# Device capability probe: prints each endpoint's IAudioClient3 shared-mode
# period range — the ground truth for the achievable latency floor per device.
add_executable(lux_probe_periods tools/probe_periods.cpp)
target_link_libraries(lux_probe_periods PRIVATE ole32 uuid)

# Loopback latency benchmark (roadmap Phase 1/6): measures the REAL round-trip
# latency of any ASIO driver via chirp emission + cross-correlation, and
# compares it against what the driver reports.
# KS filter probe: pin factories, offload node detection, trial pin opens
add_executable(lux_probe_ks tools/probe_ks.cpp)
target_link_libraries(lux_probe_ks PRIVATE setupapi)

add_executable(lux_latency_bench tools/latency_bench.cpp)
target_include_directories(lux_latency_bench PRIVATE external/asiosdk/common src)
target_compile_definitions(lux_latency_bench PRIVATE WINDOWS=1 _WINDOWS=1 WIN32=1)
target_compile_options(lux_latency_bench PRIVATE $<$<CONFIG:Release>:/O2>)
target_link_libraries(lux_latency_bench PRIVATE ole32 uuid advapi32 user32)
