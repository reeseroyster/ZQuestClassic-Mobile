#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANDROID_DIR="$ROOT/android"
BUILD_DIR="$ROOT/build-android"

ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-}"
if [[ -z "$ANDROID_NDK_HOME" ]]; then
  # Newest installed version; r27+ is what links the C++ runtime for 16 KB pages.
  ANDROID_NDK_HOME="$(find "$ANDROID_HOME/ndk" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort -V | tail -n 1)"
fi

JDK_HOME="$(find "$HOME/.local/opt" -maxdepth 1 -type d -name 'jdk-17*' -print -quit 2>/dev/null || true)"
GRADLE_HOME="$(find "$HOME/.local/opt" -maxdepth 1 -type d -name 'gradle-8.9' -print -quit 2>/dev/null || true)"
if [[ -n "$JDK_HOME" ]]; then
  export JAVA_HOME="$JDK_HOME"
  export PATH="$JAVA_HOME/bin:$PATH"
fi
if [[ -n "$GRADLE_HOME" ]]; then
  export PATH="$GRADLE_HOME/bin:$PATH"
fi
if [[ -x "$ANDROID_HOME/cmake/3.30.5/bin/cmake" ]]; then
  export PATH="$ANDROID_HOME/cmake/3.30.5/bin:$PATH"
fi

if [[ ! -f "$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" ]]; then
  echo "Android NDK not found. Install it with Android Studio or set ANDROID_NDK_HOME." >&2
  exit 1
fi
if ! command -v cmake >/dev/null || ! command -v gradle >/dev/null; then
  echo "Install CMake 3.27+ and Gradle 8.9+ (or build from Android Studio)." >&2
  exit 1
fi

export ANDROID_HOME ANDROID_NDK_HOME

cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-23 \
  -DCMAKE_BUILD_TYPE=Release \
  -DWANT_NFD=OFF \
  -DWANT_ZUPDATER=OFF \
  -DWANT_WEBSOCKETS=OFF \
  -DWANT_ZC_TESTS=OFF

# Allegro's Android support ships its Activity and JNI helpers as an AAR.
cmake --build "$BUILD_DIR" --target aar
mkdir -p "$ANDROID_DIR/app/libs"
ALLEGRO_AAR="$(find "$BUILD_DIR" -path '*/lib/allegro-release.aar' -print -quit)"
if [[ -z "$ALLEGRO_AAR" ]]; then
  echo "Allegro's Android AAR was not produced by the CMake build." >&2
  exit 1
fi
cp "$ALLEGRO_AAR" "$ANDROID_DIR/app/libs/"

cd "$ANDROID_DIR"
gradle --no-daemon :app:assembleDebug

echo "APK: $ANDROID_DIR/app/build/outputs/apk/debug/app-debug.apk"
