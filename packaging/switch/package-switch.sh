#!/usr/bin/env bash
# package-switch.sh — Produce the Switch .nro + SD-card bundle.
#
# Delegates to scripts/build/build-switch.sh, which drives the full
# pipeline: devkitPro cross-compile + asset extraction + switch_sd/
# staging. Requires the dkp toolchain (see that script's header).

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
exec "$PROJECT_DIR/scripts/build/build-switch.sh" "$@"
