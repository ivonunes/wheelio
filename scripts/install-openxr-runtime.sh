#!/usr/bin/env bash
# Development loop for the OpenXR runtime: build wheelio_openxr.dll, copy it and
# its manifest next to the game exe inside a CrossOver bottle, and point the
# bottle's OpenXR loader at it through the Khronos registry key.
#
# Usage: scripts/install-openxr-runtime.sh <bottle name> <path to game .exe>
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/build-windows"
toolchain="${repo_root}/bridge/windows/toolchains/x86_64-w64-mingw32.cmake"
cx_root="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver"

bottle="${1:?bottle name}"
exe="${2:?path to game exe}"
game_dir="$(dirname "${exe}")"
bottle_dir="${HOME}/Library/Application Support/CrossOver/Bottles/${bottle}"

if [[ ! -d "${bottle_dir}/drive_c" ]]; then
    echo "Bottle not found: ${bottle_dir}" >&2
    exit 1
fi
if [[ "${game_dir}" != "${bottle_dir}/drive_c/"* ]]; then
    echo "The game exe must live inside the bottle's drive_c" >&2
    exit 1
fi

cmake -S "${repo_root}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_WINDOWS_PROXY=ON \
    -DCMAKE_TOOLCHAIN_FILE="${toolchain}" >/dev/null
cmake --build "${build_dir}" --target wheelio_openxr_runtime

# A DLL that imports a mingw runtime library (libwinpthread, libgcc, libstdc++)
# loads fine on the build machine and silently fails inside the bottle, where
# those DLLs do not exist. Catch that here rather than in the game.
if x86_64-w64-mingw32-objdump -p "${build_dir}/wheelio_openxr.dll" \
        | grep "DLL Name" | grep -q -i -E "libwinpthread|libgcc|libstdc"; then
    echo "wheelio_openxr.dll imports a mingw runtime DLL that is not available in the bottle:" >&2
    x86_64-w64-mingw32-objdump -p "${build_dir}/wheelio_openxr.dll" | grep "DLL Name" >&2
    exit 1
fi

cp "${build_dir}/wheelio_openxr.dll" "${game_dir}/wheelio_openxr.dll"
cp "${repo_root}/bridge/windows/openxr/wheelio_openxr.json" "${game_dir}/wheelio_openxr.json"

# drive_c/... -> C:\...
windows_manifest="C:\\$(printf '%s' "${game_dir#"${bottle_dir}/drive_c/"}/wheelio_openxr.json" | sed 's#/#\\#g')"

CX_ROOT="${cx_root}" CX_BOTTLE="${bottle}" WINEDEBUG=-all \
    "${cx_root}/bin/wine" reg add 'HKLM\SOFTWARE\Khronos\OpenXR\1' \
        /v ActiveRuntime /t REG_SZ /d "${windows_manifest}" /f >/dev/null

echo "Installed runtime to ${game_dir}"
echo "ActiveRuntime = ${windows_manifest}"
CX_ROOT="${cx_root}" CX_BOTTLE="${bottle}" WINEDEBUG=-all \
    "${cx_root}/bin/wine" reg query 'HKLM\SOFTWARE\Khronos\OpenXR\1' /v ActiveRuntime | tr -d '\r' | grep -i ActiveRuntime
