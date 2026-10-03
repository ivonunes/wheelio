#!/usr/bin/env bash
# Build a Wheelio release into dist/wheelio-<version>.zip.
#
#   scripts/release.sh          build, then sign and package (local use)
#   scripts/release.sh build    build and stage the unsigned app and DLLs
#   scripts/release.sh sign     sign and notarize the staged app, then zip it
#
# CI runs the two halves in separate jobs, so the signing secrets never share a
# machine with the build's third-party code. Signing happens only when
# CODESIGN_IDENTITY is set, notarizing only when the NOTARY_* variables are.
set -euo pipefail

stage="${1:-all}"
case "${stage}" in
  all|build|sign) ;;
  *) echo "usage: $0 [build|sign]" >&2; exit 1 ;;
esac

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

mac_build_dir="${repo_root}/build-release"
windows_build_dir="${repo_root}/build-windows-release"
windows_toolchain="${repo_root}/bridge/windows/toolchains/x86_64-w64-mingw32.cmake"
dist_dir="${repo_root}/dist"

# Single source of truth: the top-level VERSION file (overridable via $VERSION).
version="${VERSION:-}"
if [[ -z "${version}" ]]; then
  version="$(tr -d '[:space:]' < "${repo_root}/VERSION" 2>/dev/null || true)"
fi
if [[ -z "${version}" ]]; then
  version="dev"
fi

package_name="wheelio-${version}"
stage_dir="${dist_dir}/${package_name}"
zip_path="${dist_dir}/${package_name}.zip"
staged_app="${stage_dir}/Wheelio.app"

build() {
  local mac_app="${mac_build_dir}/Build/Products/Release/Wheelio.app"

  # 1. Build the Windows DLLs first, so the app's build phase bundles these
  #    exact copies and they're also available as standalone files.
  cmake -S "${repo_root}" -B "${windows_build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_WINDOWS_PROXY=ON \
    -DCMAKE_TOOLCHAIN_FILE="${windows_toolchain}"
  cmake --build "${windows_build_dir}" --target wheelio_dinput8 wheelio_openxr_runtime

  cmake -S "${repo_root}" -B "${windows_build_dir}-x86" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_WINDOWS_PROXY=ON \
    -DCMAKE_TOOLCHAIN_FILE="${repo_root}/bridge/windows/toolchains/i686-w64-mingw32.cmake"
  cmake --build "${windows_build_dir}-x86" --target wheelio_dinput8

  # 2. Build the macOS app. Apple silicon only: the VR streamer (Rust +
  #    VideoToolbox) is built for arm64 alone. Ad-hoc signed by the project
  #    settings; re-signed by the sign stage with a Developer ID.
  # Bundle phases must not fall back to a build without VR pieces.
  export WHEELIO_RELEASE=1

  "${repo_root}/scripts/gen-xcode.sh"
  xcodebuild \
    -project "${repo_root}/Wheelio.xcodeproj" \
    -scheme Wheelio \
    -configuration Release \
    -derivedDataPath "${mac_build_dir}" \
    ARCHS=arm64 \
    ONLY_ACTIVE_ARCH=NO \
    MARKETING_VERSION="${version}" \
    build

  # 3. Stage the app and the standalone DLLs. The app already bundles the DLLs
  #    via its build phase; nothing is copied into it here, which would
  #    invalidate its signature.
  rm -rf "${stage_dir}" "${zip_path}"
  mkdir -p "${stage_dir}"
  ditto "${mac_app}" "${staged_app}"
  cp "${windows_build_dir}/dinput8.dll" "${stage_dir}/dinput8.dll"
  cp "${windows_build_dir}-x86/dinput8.dll" "${stage_dir}/dinput8_x86.dll"
  cp "${windows_build_dir}/wheelio_openxr.dll" "${stage_dir}/wheelio_openxr.dll"
  cp "${repo_root}/bridge/windows/openxr/wheelio_openxr.json" "${stage_dir}/wheelio_openxr.json"
  echo "Staged ${stage_dir}"
}

sign() {
  if [[ ! -d "${staged_app}" ]]; then
    echo "error: nothing staged at ${stage_dir}; run '$0 build' first" >&2
    exit 1
  fi

  # 4. Developer ID signature with hardened runtime and secure timestamp.
  if [[ -n "${CODESIGN_IDENTITY:-}" ]]; then
    echo "Signing ${staged_app} with ${CODESIGN_IDENTITY}"
    local entitlements="${repo_root}/app/Wheelio.entitlements"
    # Nested executables first (notarization checks every one), then the bundle.
    codesign --force --options runtime --timestamp \
      --entitlements "${entitlements}" \
      --sign "${CODESIGN_IDENTITY}" "${staged_app}/Contents/Resources/wheelio-streamer"
    codesign --force --options runtime --timestamp \
      --entitlements "${entitlements}" \
      --sign "${CODESIGN_IDENTITY}" "${staged_app}"
    codesign --verify --strict --verbose=2 "${staged_app}"
  fi

  # 5. Notarize with notarytool (App Store Connect API key) and staple.
  if [[ -n "${NOTARY_KEY_PATH:-}" && -n "${NOTARY_KEY_ID:-}" && -n "${NOTARY_ISSUER_ID:-}" ]]; then
    echo "Notarizing ${staged_app}"
    local notarize_zip="${dist_dir}/wheelio-notarize.zip"
    rm -f "${notarize_zip}"
    ditto -c -k --keepParent "${staged_app}" "${notarize_zip}"
    xcrun notarytool submit "${notarize_zip}" \
      --key "${NOTARY_KEY_PATH}" \
      --key-id "${NOTARY_KEY_ID}" \
      --issuer "${NOTARY_ISSUER_ID}" \
      --wait
    xcrun stapler staple "${staged_app}"
    rm -f "${notarize_zip}"
  fi

  # 6. Zip the staged app and DLLs.
  rm -f "${zip_path}"
  (
    cd "${dist_dir}"
    ditto -c -k --norsrc --keepParent "${package_name}" "${zip_path}"
  )
  echo "Created ${zip_path}"
}

if [[ "${stage}" == "all" || "${stage}" == "build" ]]; then
  build
fi
if [[ "${stage}" == "all" || "${stage}" == "sign" ]]; then
  sign
fi
