#!/usr/bin/env bash
# Xcode build phase: copy app/engine-release.json, the engine this build of
# Wheelio pins (written by release-engine.yml), into the app's Resources.
# Before the first engine is published there's none, and the app says games
# aren't available in this build; a release build needs one.
#
# Relies on Xcode-provided env: SRCROOT, TARGET_BUILD_DIR,
# UNLOCALIZED_RESOURCES_FOLDER_PATH.

src="${SRCROOT}/app/engine-release.json"
dest_dir="${TARGET_BUILD_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}"

if [ -f "${src}" ]; then
    mkdir -p "${dest_dir}"
    cp "${src}" "${dest_dir}/engine-release.json"
elif [ -n "${WHEELIO_RELEASE:-}" ]; then
    echo "error: app/engine-release.json is required for a release build; run the Release engine workflow" >&2
    exit 1
else
    rm -f "${dest_dir}/engine-release.json"
fi

exit 0
