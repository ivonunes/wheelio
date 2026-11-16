#!/usr/bin/env bash
# Xcode build phase: copy the VR streamer (Purr's, purr/modules/vr_streamer/
# streamer) into the app's Resources so the app can supervise it. Best effort
# like bundle-proxy-dll.sh: uses a prebuilt release binary, builds one if cargo
# is available, otherwise warns and continues (VR streaming is then unavailable
# in that build).
#
# Relies on Xcode-provided env: SRCROOT, TARGET_BUILD_DIR,
# UNLOCALIZED_RESOURCES_FOLDER_PATH.

export PATH="${HOME}/.cargo/bin:/opt/homebrew/bin:/usr/local/bin:${PATH}"

src="${SRCROOT}/purr/modules/vr_streamer/streamer/target/release/vr_streamer"

if [ ! -f "${src}" ] && command -v cargo >/dev/null 2>&1; then
    "${SRCROOT}/scripts/streamer-cargo.sh" build --release >/dev/null 2>&1 || true
fi

dest_dir="${TARGET_BUILD_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}"

if [ -f "${src}" ]; then
    mkdir -p "${dest_dir}"
    cp "${src}" "${dest_dir}/wheelio-streamer"
    # A proper ad-hoc signature (cargo's output is only linker-signed); the
    # release script re-signs the whole bundle when a Developer ID is set.
    codesign --force -s - "${dest_dir}/wheelio-streamer" 2>/dev/null || true
    echo "Bundled streamer from ${src}"
elif [ -n "${WHEELIO_RELEASE:-}" ]; then
    echo "error: the VR streamer is required for a release build" >&2
    exit 1
else
    echo "warning: the VR streamer wasn't found; VR streaming will be unavailable in this build. Build it with: scripts/streamer-cargo.sh build --release"
fi

exit 0
