#!/usr/bin/env bash
# regen_bios.sh — canonical, hygiene-safe regeneration of the BIOS generated C.
#
# WHY THIS EXISTS: generated/<stem>_*.c is gitignored build output. The
# recompiler (psxrecomp-bios) and the runtime are built by SEPARATE CMake trees,
# so editing the BIOS emitter (full_function_emitter.cpp, strict_translator.cpp,
# the cycle headers, …) does NOT automatically refresh generated/. If you forget
# to regenerate, the runtime links a STALE BIOS that no longer matches the emitter
# (this bit us: a stale 4439-entry BIOS vs the current 4406-entry output). This
# script makes regeneration one command AND records an emitter fingerprint so the
# CMake build can warn when generated/ has drifted (see runtime/runtime.cmake).
#
# Usage:
#   tools/regen_bios.sh                               # regen from bios/SCPH1001.toml
#   tools/regen_bios.sh --config bios/OpenBIOS.toml   # regen another BIOS profile
#   PSXRECOMP_BIOS_BUILD=recompiler/build tools/regen_bios.sh   # custom build dir
#
# Legacy env-override form (bypasses the profile; used for scratch regens into a
# different out dir — fingerprint stem falls back to PSXRECOMP_BIOS_STEM/SCPH1001):
#   PSXRECOMP_BIOS_ROM=... PSXRECOMP_BIOS_SEEDS=... PSXRECOMP_BIOS_OUT=... tools/regen_bios.sh
#
# Run from the framework root (the dir containing recompiler/ and generated/).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PROFILE="bios/SCPH1001.toml"
if [ $# -gt 0 ]; then
    if [ $# -eq 2 ] && [ "$1" = "--config" ] && [ -n "$2" ]; then
        PROFILE="$2"
    else
        # A typo'd flag silently regenerating the DEFAULT profile would be a
        # nasty surprise — reject anything but the exact supported form.
        echo "regen_bios: unrecognized arguments: $*" >&2
        echo "usage: tools/regen_bios.sh [--config <bios profile.toml>]" >&2
        exit 2
    fi
fi

# Locate the recompiler build dir (where psxrecomp-bios[.exe] lives or will be built).
BUILD="${PSXRECOMP_BIOS_BUILD:-}"
if [ -z "$BUILD" ]; then
    for d in recompiler/build-t2 recompiler/build recompiler/cmake-build*; do
        if [ -f "$d/CMakeCache.txt" ]; then BUILD="$d"; break; fi
    done
fi
[ -n "$BUILD" ] || { echo "regen_bios: no recompiler build dir found (set PSXRECOMP_BIOS_BUILD)"; exit 1; }

# 1. Build the emitter FRESH so the regen always reflects current source.
echo "regen_bios: building psxrecomp-bios in $BUILD"
cmake --build "$BUILD" --target psxrecomp-bios >/dev/null

EXE="$BUILD/psxrecomp-bios.exe"; [ -f "$EXE" ] || EXE="$BUILD/psxrecomp-bios"
[ -f "$EXE" ] || { echo "regen_bios: psxrecomp-bios not found in $BUILD after build"; exit 1; }

toml_value() {
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$PROFILE" | head -1
}

if [ -n "${PSXRECOMP_BIOS_ROM:-}${PSXRECOMP_BIOS_SEEDS:-}${PSXRECOMP_BIOS_OUT:-}" ]; then
    # 2a. Legacy env-override regen (positional invocation, profile bypassed).
    BIOS="${PSXRECOMP_BIOS_ROM:-bios/SCPH1001.BIN}"
    SEEDS="${PSXRECOMP_BIOS_SEEDS:-recompiler/seeds/phase2_ghidra_seeds.json}"
    OUT="${PSXRECOMP_BIOS_OUT:-generated}"
    STEM="${PSXRECOMP_BIOS_STEM:-SCPH1001}"
    echo "regen_bios: emit-full $BIOS -> $OUT (seeds: $SEEDS) [env-override mode]"
    mkdir -p "$OUT"
    "$EXE" "$BIOS" "$OUT" --emit-full "$SEEDS"
else
    # 2b. Profile-driven regen (the canonical path).
    [ -f "$PROFILE" ] || { echo "regen_bios: profile not found: $PROFILE"; exit 1; }
    OUT="$(toml_value out_dir)";  OUT="${OUT:-generated}"
    STEM="$(toml_value out_stem)"
    [ -n "$STEM" ] || { echo "regen_bios: no out_stem in $PROFILE"; exit 1; }
    echo "regen_bios: emit-full --config $PROFILE (stem: $STEM)"
    mkdir -p "$OUT"
    "$EXE" --config "$PROFILE"
fi

# 3. Record the emitter fingerprint so the build can detect future drift. MUST
#    match runtime/runtime.cmake's staleness check invocation (same profile arg).
"$ROOT/tools/bios_emitter_fingerprint.sh" "$PROFILE" > "$OUT/$STEM.emitter.sha"
echo "regen_bios: wrote fingerprint $OUT/$STEM.emitter.sha"
echo "regen_bios: done ($(grep -m1 -o 'Dispatch entries: [0-9]*' "$OUT/${STEM}_dispatch.c" || echo '?'))"
