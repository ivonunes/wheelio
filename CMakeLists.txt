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

set(PURR_FORCE_FEEDBACK "${CMAKE_CURRENT_SOURCE_DIR}/purr/modules/force_feedback")

set(WHEELIO_BRIDGE_COMMON_SOURCES
    bridge/common/dinput_effects.cpp
)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    # The game's DirectInput effects summed into the bridge's state, and that
    # state mapped to what Purr's wheel session plays.
    add_executable(wheelio_tests
        tests/force_feedback_tests.cpp
        bridge/macos/force_mapping.cpp
        ${WHEELIO_BRIDGE_COMMON_SOURCES}
    )

    target_include_directories(wheelio_tests PRIVATE
        bridge/include
        ${PURR_FORCE_FEEDBACK}
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
endif()

# The Steam Play patch (steam/, arm64 macOS) brings its own tests.
if(APPLE AND NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(steam)
endif()

if(BUILD_WINDOWS_PROXY)
    # Purr's wheel list (known_wheels.cpp) says which devices get force feedback.
    add_library(wheelio_dinput8 SHARED
        ${WHEELIO_BRIDGE_COMMON_SOURCES}
        bridge/windows/bridge_client.cpp
        bridge/windows/dinput8_proxy.cpp
        ${PURR_FORCE_FEEDBACK}/known_wheels.cpp
    )

    target_include_directories(wheelio_dinput8 PRIVATE
        bridge/include
        bridge/windows/include
        ${PURR_FORCE_FEEDBACK}
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

    # Purr's OpenXR runtime (purr/modules/vr_streamer/runtime), loaded by the
    # game's OpenXR loader through the manifest bridge/windows/openxr/wheelio_openxr.json.
    # It writes each frame to a file in the bottle's temp folder, where the app
    # finds it and runs the streamer. 64-bit games only: no 32-bit game runs in VR.
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(PURR_THIRDPARTY "${CMAKE_CURRENT_SOURCE_DIR}/purr/godot/thirdparty")
    file(GLOB PURR_VR_RUNTIME_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/purr/modules/vr_streamer/runtime/*.cpp")
    add_library(wheelio_openxr_runtime SHARED ${PURR_VR_RUNTIME_SOURCES})

    target_include_directories(wheelio_openxr_runtime PRIVATE
        ${PURR_THIRDPARTY}/openxr/include
        ${PURR_THIRDPARTY}/vulkan/include
        ${PURR_THIRDPARTY}/directx_headers/include/directx
        ${PURR_THIRDPARTY}/directx_headers/include
    )

    set_target_properties(wheelio_openxr_runtime PROPERTIES
        PREFIX ""
        OUTPUT_NAME wheelio_openxr
    )

    # Purr's code builds to Purr's warnings, not Wheelio's -Werror. Everything
    # is linked statically: the bottle has no mingw runtime DLLs.
    target_compile_options(wheelio_openxr_runtime PRIVATE -O2)
    target_link_options(wheelio_openxr_runtime PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
    )

    target_link_libraries(wheelio_openxr_runtime
        d3d11
        d3d12
        dxgi
        dxguid
        ole32
    )

    # Stand-in for a game: exercises the runtime on D3D11 and D3D12 inside a
    # bottle. A development tool, not shipped.
    add_executable(wheelio_xr_smoke bridge/windows/openxr/smoke/xr_smoke.cpp)
    target_include_directories(wheelio_xr_smoke PRIVATE ${PURR_THIRDPARTY}/openxr/include)
    # OpenXR structs are initialised {XR_TYPE_...} with the rest zeroed.
    target_compile_options(wheelio_xr_smoke PRIVATE -Wno-missing-field-initializers)
    target_link_options(wheelio_xr_smoke PRIVATE -static-libgcc -static-libstdc++)
    target_link_libraries(wheelio_xr_smoke wheelio_compile_options d3d11 d3d12 dxgi dxguid)
    endif()
endif()
