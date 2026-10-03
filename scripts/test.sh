#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

mac_build_dir="${repo_root}/build"
windows_build_dir="${repo_root}/build-windows"
windows_toolchain="${repo_root}/bridge/windows/toolchains/x86_64-w64-mingw32.cmake"

ats_dir="${HOME}/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/American Truck Simulator/bin/win_x64"
mac_app="${mac_build_dir}/Build/Products/Release/Wheelio.app"
windows_dll="${windows_build_dir}/dinput8.dll"

# Build the macOS app with Xcode (the app moved from CMake to project.yml).
"${repo_root}/scripts/gen-xcode.sh"
xcodebuild \
  -project "${repo_root}/Wheelio.xcodeproj" \
  -scheme Wheelio \
  -configuration Release \
  -derivedDataPath "${mac_build_dir}" \
  build

cmake -S "${repo_root}" -B "${windows_build_dir}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_WINDOWS_PROXY=ON \
  -DCMAKE_TOOLCHAIN_FILE="${windows_toolchain}"
cmake --build "${windows_build_dir}" --target wheelio_dinput8

if [[ ! -d "${ats_dir}" ]]; then
  echo "American Truck Simulator win_x64 directory not found:" >&2
  echo "  ${ats_dir}" >&2
  exit 1
fi

cp "${windows_dll}" "${ats_dir}/dinput8.dll"
open "${mac_app}"

echo "Copied ${windows_dll} to ${ats_dir}/dinput8.dll"
echo "Opened ${mac_app}"
