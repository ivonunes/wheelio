#!/usr/bin/env bash
# Regenerate Wheelio.xcodeproj from project.yml.
#
# The .xcodeproj is generated (gitignored) — edit project.yml, not the project
# in Xcode, then re-run this. Requires XcodeGen: brew install xcodegen
set -euo pipefail

cd "$(dirname "$0")/.."

if ! command -v xcodegen >/dev/null 2>&1; then
    echo "xcodegen not found. Install it with: brew install xcodegen" >&2
    exit 1
fi

xcodegen generate
echo "Generated Wheelio.xcodeproj — open it with: open Wheelio.xcodeproj"
