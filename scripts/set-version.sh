#!/usr/bin/env bash
# Set the app version everywhere it is recorded: the VERSION file (read by
# release.sh) and MARKETING_VERSION in project.yml (the app's Info.plist).
#   scripts/set-version.sh 2.1.0
set -euo pipefail

version="${1:-}"
if [[ ! "${version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "usage: $0 MAJOR.MINOR.PATCH" >&2
  exit 1
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
printf '%s\n' "${version}" > "${repo_root}/VERSION"
sed -i '' -E "s/^([[:space:]]*MARKETING_VERSION:[[:space:]]*)\"[^\"]*\"/\1\"${version}\"/" "${repo_root}/project.yml"

if ! grep -q "MARKETING_VERSION: \"${version}\"" "${repo_root}/project.yml"; then
  echo "error: could not set MARKETING_VERSION in project.yml" >&2
  exit 1
fi
