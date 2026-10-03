#!/usr/bin/env bash
# Xcode build phase: copy the OpenXR runtime (wheelio_openxr.dll and its
# manifest) into the app's Resources so "Set Up a Game" can install VR support.
# Best effort like bundle-proxy-dll.sh: uses a prebuilt DLL if present, builds
# one if mingw-w64 is available, otherwise warns and continues.
#
# Relies on Xcode-provided env: SRCROOT, TARGET_BUILD_DIR,
# UNLOCALIZED_RESOURCES_FOLDER_PATH.

export PATH="/opt/homebrew/bin:/usr/local/bin:${PATH}"

src=""
for candidate in \
    "${SRCROOT}/build-windows-release/wheelio_openxr.dll" \
    "${SRCROOT}/build-windows/wheelio_openxr.dll"; do
    if [ -f "${candidate}" ]; then
        src="${candidate}"
        break
    fi
done

if [ -z "${src}" ] && command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
    build_dir="${SRCROOT}/build-windows-release"
    if cmake -S "${SRCROOT}" -B "${build_dir}" \
            -DCMAKE_BUILD_TYPE=Release -DBUILD_WINDOWS_PROXY=ON \
            -DCMAKE_TOOLCHAIN_FILE="${SRCROOT}/bridge/windows/toolchains/x86_64-w64-mingw32.cmake" >/dev/null 2>&1 \
        && cmake --build "${build_dir}" --target wheelio_openxr_runtime >/dev/null 2>&1; then
        src="${build_dir}/wheelio_openxr.dll"
    fi
fi

dest_dir="${TARGET_BUILD_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}"

if [ -n "${src}" ] && [ -f "${src}" ]; then
    mkdir -p "${dest_dir}"
    cp "${src}" "${dest_dir}/wheelio_openxr.dll"
    cp "${SRCROOT}/bridge/windows/openxr/wheelio_openxr.json" "${dest_dir}/wheelio_openxr.json"
    echo "Bundled OpenXR runtime from ${src}"
elif [ -n "${WHEELIO_RELEASE:-}" ]; then
    echo "error: wheelio_openxr.dll is required for a release build" >&2
    exit 1
else
    echo "warning: wheelio_openxr.dll not found; VR setup will be unavailable in this build. Build it with scripts/build-proxy.sh (needs: brew install mingw-w64)."
fi

exit 0
