#!/usr/bin/env bash
# Build fixture, scanner and dumper for host (macOS, fixture only) and Android arm64.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
OUT="$ROOT/out"; mkdir -p "$OUT"

NDK="${ANDROID_NDK:-$HOME/Library/Android/sdk/ndk/27.1.12297006}"
CLANG="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android30-clang++"
FLAGS=(-std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter)
ANDROID=(-static-libstdc++ -fPIE -pie)

clang++ "${FLAGS[@]}" -o "$OUT/ue4_fake_host" "$ROOT/fixture/ue4_fake.cpp"
echo "built out/ue4_fake_host"

[ -x "$CLANG" ] || { echo "NDK clang not found at $CLANG" >&2; exit 1; }
"$CLANG" "${FLAGS[@]}" "${ANDROID[@]}" -o "$OUT/ue4_fake_arm64" "$ROOT/fixture/ue4_fake.cpp"
echo "built out/ue4_fake_arm64"
"$CLANG" "${FLAGS[@]}" "${ANDROID[@]}" -o "$OUT/ue4scan_arm64"  "$ROOT/scanner/ue4scan.cpp"
echo "built out/ue4scan_arm64"
"$CLANG" "${FLAGS[@]}" "${ANDROID[@]}" -o "$OUT/ue4dump_arm64"  "$ROOT/dumper/ue4dump.cpp"
echo "built out/ue4dump_arm64"
