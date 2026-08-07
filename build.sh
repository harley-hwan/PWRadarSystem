#!/bin/sh
# ==========================================================================
#  PWRadarSystem - Linux build
#  ------------------------------------------------------------------------
#      ./build.sh                Release
#      ./build.sh Debug          Debug
#      ./build.sh Release run    build, self test, then launch the console
#      ./build.sh clean          delete the build directory
#
#  Requires: a C17 compiler, CMake 3.21+, and the libX11 development files.
#      Debian/Ubuntu : sudo apt install build-essential cmake libx11-dev
#      Fedora/RHEL   : sudo dnf install gcc cmake libX11-devel
# ==========================================================================
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILDDIR="$ROOT/build"
CFG="${1:-Release}"

if [ "$CFG" = "clean" ]; then
    rm -rf "$BUILDDIR"
    echo "[OK] removed $BUILDDIR"
    exit 0
fi

case "$CFG" in
    Debug|Release) ;;
    *) echo "[ERROR] unknown configuration \"$CFG\". Use Debug, Release or clean." >&2
       exit 2 ;;
esac

if ! command -v cmake >/dev/null 2>&1; then
    echo "[ERROR] cmake not found. Install it (apt install cmake / dnf install cmake)." >&2
    exit 1
fi

JOBS="$( (nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4) )"

echo "======================================================================"
echo " PWRadarSystem  -  $CFG   ($JOBS jobs)"
echo "======================================================================"
echo

cmake -S "$ROOT" -B "$BUILDDIR" -DCMAKE_BUILD_TYPE="$CFG"
cmake --build "$BUILDDIR" -j "$JOBS"

EXE="$BUILDDIR/PWRadarUI"
if [ ! -x "$EXE" ]; then
    echo "[FAILED] the build reported success but $EXE is missing." >&2
    exit 1
fi

echo
echo "---------------------------------------------------------------------"
echo " Numerical acceptance suite"
echo "---------------------------------------------------------------------"
"$EXE" --selftest

echo
echo "======================================================================"
echo " BUILD OK"
echo "   executable : $EXE"
echo "   library    : $BUILDDIR/libPWRadarCore.so"
echo "======================================================================"

if [ "${2:-}" = "run" ]; then
    if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
        echo "[WARN] no DISPLAY set; the console needs an X server." >&2
    fi
    exec "$EXE"
fi
