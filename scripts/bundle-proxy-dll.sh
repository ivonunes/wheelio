#!/usr/bin/env bash
# Xcode build phase: copy the Windows proxy dinput8.dll into the app bundle's
# Resources so the in-app "Set Up a Game" installer has a DLL to install — in
# Debug builds too, not just release.sh packaging.
#
# Best effort, never fails the build: uses a prebuilt DLL if present, builds it
# once if mingw-w64 is available, otherwise warns and continues (the installer
# is then disabled in that build).
#
# Relies on Xcode-provided env: SRCROOT, TARGET_BUILD_DIR,
# UNLOCALIZED_RESOURCES_FOLDER_PATH. Honors WHEELIO_PROXY_DLL as an override.

# Xcode build phases run with a restricted PATH that omits Homebrew, so cmake
# and the mingw cross-compiler aren't found by default. Add the usual locations.
export PATH="/opt/homebrew/bin:/usr/local/bin:${PATH}"

src="${WHEELIO_PROXY_DLL:-}"

if [ -z "${src}" ]; then
    for candidate in \
        "${SRCROOT}/build-windows-release/dinput8.dll" \
        "${SRCROOT}/build-windows/dinput8.dll"; do
        if [ -f "${candidate}" ]; then
            src="${candidate}"
            break
        fi
    done
fi

# Build it once if we still don't have one and the cross-compiler is available.
if [ -z "${src}" ] && command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
    build_dir="${SRCROOT}/build-windows-release"
    if cmake -S "${SRCROOT}" -B "${build_dir}" \
            -DCMAKE_BUILD_TYPE=Release -DBUILD_WINDOWS_PROXY=ON \
            -DCMAKE_TOOLCHAIN_FILE="${SRCROOT}/bridge/windows/toolchains/x86_64-w64-mingw32.cmake" >/dev/null 2>&1 \
        && cmake --build "${build_dir}" --target wheelio_dinput8 >/dev/null 2>&1; then
        src="${build_dir}/dinput8.dll"
    fi
fi

dest_dir="${TARGET_BUILD_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}"

if [ -n "${src}" ] && [ -f "${src}" ]; then
    mkdir -p "${dest_dir}"
    cp "${src}" "${dest_dir}/dinput8.dll"
    echo "Bundled proxy DLL from ${src}"
elif [ -n "${WHEELIO_RELEASE:-}" ]; then
    echo "error: dinput8.dll is required for a release build" >&2
    exit 1
else
    echo "warning: proxy dinput8.dll not found; the in-app installer will be disabled in this build. Build it with scripts/build-proxy.sh (needs: brew install mingw-w64)."
fi

# The 32-bit proxy for 32-bit games, bundled as dinput8_x86.dll.
src_x86=""
for candidate in \
    "${SRCROOT}/build-windows-x86-release/dinput8.dll" \
    "${SRCROOT}/build-windows-x86/dinput8.dll"; do
    if [ -f "${candidate}" ]; then
        src_x86="${candidate}"
        break
    fi
done
if [ -z "${src_x86}" ] && command -v i686-w64-mingw32-g++ >/dev/null 2>&1; then
    build_dir_x86="${SRCROOT}/build-windows-x86-release"
    if cmake -S "${SRCROOT}" -B "${build_dir_x86}" \
            -DCMAKE_BUILD_TYPE=Release -DBUILD_WINDOWS_PROXY=ON \
            -DCMAKE_TOOLCHAIN_FILE="${SRCROOT}/bridge/windows/toolchains/i686-w64-mingw32.cmake" >/dev/null 2>&1 \
        && cmake --build "${build_dir_x86}" --target wheelio_dinput8 >/dev/null 2>&1; then
        src_x86="${build_dir_x86}/dinput8.dll"
    fi
fi
if [ -n "${src_x86}" ] && [ -f "${src_x86}" ]; then
    cp "${src_x86}" "${dest_dir}/dinput8_x86.dll"
    echo "Bundled 32-bit proxy DLL from ${src_x86}"
elif [ -n "${WHEELIO_RELEASE:-}" ]; then
    echo "error: the 32-bit dinput8.dll is required for a release build" >&2
    exit 1
else
    echo "warning: 32-bit proxy dinput8.dll not found; 32-bit games cannot be set up in this build."
fi

exit 0
