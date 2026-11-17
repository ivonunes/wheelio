#!/usr/bin/env bash
# Runs cargo on Purr's VR streamer (purr/modules/vr_streamer/streamer), e.g.
#   scripts/streamer-cargo.sh build --release
#   scripts/streamer-cargo.sh test --release
#
# Locked to Purr's Cargo.lock, so the build is the one Purr tested.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "${repo_root}/purr/modules/vr_streamer/streamer"
exec cargo --locked "$@"
