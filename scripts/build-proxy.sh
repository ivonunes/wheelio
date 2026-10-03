#!/usr/bin/env bash
# Build the Windows proxy dinput8.dll (64-bit and 32-bit) and the OpenXR
# runtime into build-windows-release/ and build-windows-x86-release/.
# Run this once (needs mingw-w64) so Xcode builds — Debug included — can bundle
# the DLL into the app via scripts/bundle-proxy-dll.sh.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/build-windows-release"
build_dir_x86="${repo_root}/build-windows-x86-release"
toolchain="${repo_root}/bridge/windows/toolchains/x86_64-w64-mingw32.cmake"
toolchain_x86="${repo_root}/bridge/windows/toolchains/i686-w64-mingw32.cmake"

if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
    echo "mingw-w64 not found. Install it with: brew install mingw-w64" >&2
    exit 1
fi

cmake -S "${repo_root}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_WINDOWS_PROXY=ON \
    -DCMAKE_TOOLCHAIN_FILE="${toolchain}"
cmake --build "${build_dir}" --target wheelio_dinput8 wheelio_openxr_runtime wheelio_xr_smoke

cmake -S "${repo_root}" -B "${build_dir_x86}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_WINDOWS_PROXY=ON \
    -DCMAKE_TOOLCHAIN_FILE="${toolchain_x86}"
cmake --build "${build_dir_x86}" --target wheelio_dinput8

echo "Built ${build_dir}/dinput8.dll, ${build_dir_x86}/dinput8.dll (32-bit) and ${build_dir}/wheelio_openxr.dll"
