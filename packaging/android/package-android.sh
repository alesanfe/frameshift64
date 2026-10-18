#!/usr/bin/env bash
# package-android.sh — Produce the Android APK/AAB.
#
# Delegates to the Gradle project under platforms/android/ (the platform
# wrapper owns the manifest, keystore config and CMake-in-Gradle wiring —
# see docs/platforms/android.md). Signing needs keystore.properties;
# copy keystore.properties.example and fill it in for release builds.
#
#   ./packaging/android/package-android.sh            # release APK
#   ./packaging/android/package-android.sh --debug    # debug APK

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ANDROID_DIR="$PROJECT_DIR/platforms/android"
TASK="assembleRelease"
if [[ "${1:-}" == "--debug" ]]; then
    TASK="assembleDebug"
fi

cd "$ANDROID_DIR"
./gradlew "$TASK"
echo "APK under $ANDROID_DIR/app/build/outputs/"
