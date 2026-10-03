cmake_minimum_required(VERSION 3.21)

# Single source of truth for the version, shared with the app build (read by
# scripts/release.sh and passed to xcodebuild as MARKETING_VERSION).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" WHEELIO_VERSION)

project(wheelio VERSION ${WHEELIO_VERSION})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include(CTest)

# The macOS app (Wheelio.app) is built with Xcode from project.yml (XcodeGen),
# not CMake. CMake builds the Windows dinput8.dll proxy and the unit tests.
option(BUILD_WINDOWS_PROXY "Build the Windows dinput8 bridge proxy" OFF)

add_library(wheelio_compile_options INTERFACE)
target_compile_options(wheelio_compile_options INTERFACE
    -Wall
    -Wextra
    -pedantic
    -Werror
    -fno-exceptions
    -fno-rtti
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:RelWithDebInfo>:-O3>
    $<$<CONFIG:MinSizeRel>:-O3>
)
target_compile_definitions(wheelio_compile_options INTERFACE
    $<$<CONFIG:Release>:UTI_RELEASE>
    $<$<CONFIG:RelWithDebInfo>:UTI_RELEASE>
    $<$<CONFIG:MinSizeRel>:UTI_RELEASE>
)

set(WHEELIO_BRIDGE_COMMON_SOURCES
    bridge/common/ffb_mapping.cpp
    bridge/common/dinput_effects.cpp
)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(wheelio_tests
        tests/ffb_mapping_tests.cpp
        ${WHEELIO_BRIDGE_COMMON_SOURCES}
    )

    target_include_directories(wheelio_tests PRIVATE
        bridge/include
    )

    target_link_libraries(wheelio_tests
        wheelio_compile_options
    )

    add_test(NAME wheelio_tests COMMAND wheelio_tests)

    # Wire-format contract shared between the macOS server and the Windows DLL.
    add_executable(wheelio_protocol_tests
        tests/protocol_tests.cpp
    )

    target_include_directories(wheelio_protocol_tests PRIVATE
        bridge/include
    )

    target_link_libraries(wheelio_protocol_tests
        wheelio_compile_options
    )

    add_test(NAME wheelio_protocol_tests COMMAND wheelio_protocol_tests)

    # Logitech command encoding + the pure FFB command planner (force mapping,
    # gain/smoothing, redundant-write suppression) + the wheel registry. These
    # link the macOS app's command/plan sources (no IOKit calls, header-only).
    add_executable(wheelio_driver_tests
        tests/driver_tests.cpp
        src/command.cpp
        src/types.cpp
        src/ffb_command_plan.cpp
        ${WHEELIO_BRIDGE_COMMON_SOURCES}
    )

    target_include_directories(wheelio_driver_tests PRIVATE
        include
        bridge/include
    )

    target_link_libraries(wheelio_driver_tests
        wheelio_compile_options
    )

    add_test(NAME wheelio_driver_tests COMMAND wheelio_driver_tests)
endif()

if(BUILD_WINDOWS_PROXY)
    add_library(wheelio_dinput8 SHARED
        ${WHEELIO_BRIDGE_COMMON_SOURCES}
        bridge/windows/bridge_client.cpp
        bridge/windows/dinput8_proxy.cpp
    )

    target_include_directories(wheelio_dinput8 PRIVATE
        include
        bridge/include
        bridge/windows/include
    )

    set_target_properties(wheelio_dinput8 PROPERTIES
        PREFIX ""
        OUTPUT_NAME dinput8
    )

    # The same proxy builds for 32-bit games with the i686 toolchain. There
    # the stdcall exports would be decorated (DirectInput8Create@20) and the
    # game's loader would not find them; --kill-at exports the plain names.
    target_link_options(wheelio_dinput8 PRIVATE
        -static-libgcc
        -static-libstdc++
        $<$<EQUAL:${CMAKE_SIZEOF_VOID_P},4>:-Wl,--kill-at>
    )

    target_link_libraries(wheelio_dinput8
        wheelio_compile_options
        dxguid
        ws2_32
    )

    # OpenXR runtime loaded by the game's OpenXR loader (registered via the
    # runtime manifest bridge/windows/openxr/wheelio_openxr.json). 64-bit games
    # only: no 32-bit game runs in VR.
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_library(wheelio_openxr_runtime SHARED
        bridge/windows/openxr/frame_export.cpp
        bridge/windows/openxr/openxr_runtime.cpp
        bridge/windows/openxr/png_writer.cpp
        bridge/windows/openxr/readback_d3d11.cpp
        bridge/windows/openxr/readback_d3d12.cpp
        bridge/windows/openxr/runtime_log.cpp
    )

    target_include_directories(wheelio_openxr_runtime PRIVATE
        bridge/include
        bridge/windows/include
        bridge/windows/openxr
    )

    set_target_properties(wheelio_openxr_runtime PROPERTIES
        PREFIX ""
        OUTPUT_NAME wheelio_openxr
    )

    target_link_options(wheelio_openxr_runtime PRIVATE
        -static-libgcc
        -static-libstdc++
    )

    target_link_libraries(wheelio_openxr_runtime
        wheelio_compile_options
        dxgi
        dxguid
    )

    # Stand-in for a game: exercises the runtime on D3D11 and D3D12 inside a
    # bottle. A development tool, not shipped.
    add_executable(wheelio_xr_smoke bridge/windows/openxr/smoke/xr_smoke.cpp)
    target_include_directories(wheelio_xr_smoke PRIVATE bridge/windows/include)
    # OpenXR structs are initialised {XR_TYPE_...} with the rest zeroed.
    target_compile_options(wheelio_xr_smoke PRIVATE -Wno-missing-field-initializers)
    target_link_options(wheelio_xr_smoke PRIVATE -static-libgcc -static-libstdc++)
    target_link_libraries(wheelio_xr_smoke wheelio_compile_options d3d11 d3d12 dxgi dxguid)
    endif()
endif()
