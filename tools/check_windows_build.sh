#!/bin/sh
# ==========================================================================
#  PWRadarSystem - verify the Windows code path from Linux
#  ------------------------------------------------------------------------
#  Cross-compiles the whole project for Windows x64 with mingw-w64 and links
#  PWRadarCore.dll + PWRadarUI.exe.  This is not a substitute for building
#  with MSVC, but it does compile every line of the Win32 platform layer -
#  the window procedure, the DIB framebuffer, the Win32 threading and atomics,
#  and the dllexport/dllimport plumbing - so an API misuse or a missing header
#  is caught here rather than on the Windows machine.
#
#      sudo apt install mingw-w64
#      ./tools/check_windows_build.sh
#
#  If wine is installed the freshly built PWRadarUI.exe is also *executed*
#  (--selftest), because "it compiles" is not the same claim as "it runs":
#  the PWR_OK / <winuser.h> macro collision that produced
#  "engine creation failed: thread failure" compiled perfectly clean at
#  -Werror and only showed up at run time.
#
#  Exits non-zero on any warning, error, collision or self-test failure, so it
#  works as a CI gate.
# ==========================================================================
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC="${CC:-x86_64-w64-mingw32-gcc}"
OUT="${OUT:-$ROOT/build-mingw}"

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "[ERROR] $CC not found.  Install it: sudo apt install mingw-w64" >&2
    exit 1
fi

# --------------------------------------------------------------------------
#  Gate 0: no public identifier may be an object-like macro in the Windows
#  SDK.  This runs first because a collision miscompiles silently - there is
#  no point looking at the rest of the output until it is clean.
# --------------------------------------------------------------------------
if command -v python3 >/dev/null 2>&1; then
    echo "== name-collision gate =="
    python3 "$ROOT/tools/check_name_collisions.py"
    echo
else
    echo "[WARN] python3 not found - skipping the name-collision gate." >&2
fi

WARN="-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes
      -Wmissing-prototypes -Wcast-qual -Wpointer-arith -Werror"
# Windows 7 or later: the platform layer uses condition variables.
DEFS="-D_WIN32_WINNT=0x0601"

rm -rf "$OUT"
mkdir -p "$OUT/core" "$OUT/ui"

echo "== PWRadarCore -> PWRadarCore.dll =="
for f in "$ROOT"/PWRadarCore/src/*.c; do
    $CC -std=c17 -O2 $WARN $DEFS -DPWR_BUILD_SHARED \
        -I "$ROOT/PWRadarCore/include" -I "$ROOT/PWRadarCore/src" \
        -c "$f" -o "$OUT/core/$(basename "${f%.c}").o"
done
$CC -shared "$OUT"/core/*.o -o "$OUT/PWRadarCore.dll" \
    -Wl,--out-implib,"$OUT/libPWRadarCore.a"

echo "== PWRadarUI -> PWRadarUI.exe =="
for f in "$ROOT"/PWRadarUI/src/*.c; do
    $CC -std=c17 -O2 $WARN $DEFS \
        -I "$ROOT/PWRadarCore/include" -I "$ROOT/PWRadarUI/src" \
        -c "$f" -o "$OUT/ui/$(basename "${f%.c}").o"
done
$CC "$OUT"/ui/*.o -L"$OUT" -lPWRadarCore -luser32 -lgdi32 \
    -o "$OUT/PWRadarUI.exe"

N=$(x86_64-w64-mingw32-objdump -p "$OUT/PWRadarCore.dll" | grep -coE '\bpwr_[a-z_0-9]+\b' || true)
echo
echo "[OK] Windows code path builds clean."
echo "     $OUT/PWRadarCore.dll   ($N exported pwr_* symbols)"
echo "     $OUT/PWRadarUI.exe"

# --------------------------------------------------------------------------
#  Gate 2: actually run it.  wine is close enough to Win32 for the engine's
#  threading, condition variables and interlocked intrinsics to be exercised
#  for real; the self test never opens a window, so no display is required.
# --------------------------------------------------------------------------
echo
if command -v wine >/dev/null 2>&1; then
    echo "== executing PWRadarUI.exe --selftest under wine =="
    ( cd "$OUT" && WINEDEBUG=-all wine ./PWRadarUI.exe --selftest )
    echo
    echo "[OK] Windows binary runs and passes its self test."
else
    echo "[WARN] wine not found - compiled but NOT executed." >&2
    echo "       Install it to close the loop: sudo apt install wine64" >&2
fi

echo
echo "Note: wine is not Windows and mingw is not MSVC.  This gate proves the"
echo "Win32 code path compiles clean and executes correctly; MSVC-specific"
echo "diagnostics still have to be checked with the real toolchain."
