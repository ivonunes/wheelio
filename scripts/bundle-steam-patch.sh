#!/usr/bin/env bash
# Xcode build phase: build Wheelio's Steam Play patch (steam/) and copy it into
# the app's Resources, from where the app installs it for Steam. Best effort
# like the other bundling phases: without it, a build can't switch on Windows
# games in Steam.
#
# Relies on Xcode-provided env: SRCROOT, TARGET_BUILD_DIR,
# UNLOCALIZED_RESOURCES_FOLDER_PATH.

export PATH="/opt/homebrew/bin:/usr/local/bin:${PATH}"

build_dir="${SRCROOT}/build-steam"
src="${build_dir}/libwheelio_steam.dylib"

if command -v cmake >/dev/null 2>&1; then
    cmake -S "${SRCROOT}/steam" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF >/dev/null 2>&1 \
        && cmake --build "${build_dir}" --target wheelio_steam -j "$(sysctl -n hw.ncpu)" >/dev/null 2>&1
fi

dest_dir="${TARGET_BUILD_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}"

if [ -f "${src}" ]; then
    mkdir -p "${dest_dir}"
    cp "${src}" "${dest_dir}/libwheelio_steam.dylib"
    echo "Bundled Steam patch from ${src}"
elif [ -n "${WHEELIO_RELEASE:-}" ]; then
    echo "error: libwheelio_steam.dylib is required for a release build" >&2
    exit 1
else
    echo "warning: the Steam patch couldn't be built; Windows games in Steam will be unavailable in this build. It needs cmake (brew install cmake)."
fi

exit 0
