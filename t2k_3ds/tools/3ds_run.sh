#!/usr/bin/env bash
# Ralph-wiggum loop helper: kill mandarine, rebuild the 3DS target, relaunch,
# and screenshot the emulator window. Usage: t2k_3ds/tools/3ds_run.sh [seconds]
# (runs from t2k_3ds/, where the Makefile and the built t2k.3dsx live)
set -u
cd "$(dirname "$0")/.."
# NEVER :1 (the user's desktop); validation runs on a private Xvfb, see DOCTRINE.md.
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM DISPLAY="${T2K_DISPLAY:-:91}"
WAIT="${1:-7}"
SHOT=/tmp/mand_shot.png
# port-docs 50-tooling/mandarine-emulator-location-and-version-pinning: the
# AppImage is a versioned symlink; harnesses take it from an env var with that
# path as the default and never hardcode an extract directory.
APPIMAGE="${MANDARINE:-/mnt/nas/Development/mandarine.AppImage}"

echo "== kill mandarine (AppImage launcher + FUSE-mounted child) =="
kill_mandarine() {
  # 1) the AppImage launcher (argv contains the .AppImage path)
  pkill -9 -f 'mandarine.*\.AppImage' 2>/dev/null
  # 2) the re-exec'd binary running from the FUSE mount (/tmp/.mount_*mandar*)
  pkill -9 -f '\.mount_[Mm]andar' 2>/dev/null
  # 3) the window's owning pid (works even when the cmdline/exe don't say
  #    "mandarine" — the FUSE-mounted binary is /tmp/.mount_mandarXXXX/.../mandarine).
  for w in $(xdotool search --name -i mandarine 2>/dev/null); do
    wp=$(xdotool getwindowpid "$w" 2>/dev/null)
    [ -n "$wp" ] && kill -9 "$wp" 2>/dev/null
  done
  # 4) belt-and-suspenders: scan EVERY pid's resolved exe (readlink /proc/pid/exe)
  #    — do NOT rely on pgrep matching the cmdline, since the live process's argv
  #    is the .AppImage path while only exe resolves to the .mount_mandar* path.
  #    Skip our own pids so we never kill the loop.
  for p in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
    [ "$p" = "$$" ] && continue
    [ "$p" = "$PPID" ] && continue
    exe=$(readlink -f "/proc/$p/exe" 2>/dev/null)
    case "$exe" in *[Mm]andar*|*.mount_[Mm]andar*) kill -9 "$p" 2>/dev/null;; esac
  done
}
kill_mandarine; sleep 1; kill_mandarine  # twice: FUSE child can respawn briefly
leftover=$(for p in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
  [ "$p" = "$$" ] && continue; [ "$p" = "$PPID" ] && continue
  e=$(readlink -f /proc/$p/exe 2>/dev/null); case "$e" in *[Mm]andar*) echo "$p";; esac
done)
[ -n "$leftover" ] && echo "WARN leftover mandarine pids: $leftover" || echo "mandarine clear"
sleep 1

echo "== build =="
# make's exit code is captured directly (not via pipeline status) so a
# filtered-to-empty clean build can never look like a failure, and a real
# failure can never be masked by tail/grep's own (unrelated) exit status.
BUILD_LOG=/tmp/3ds_build.log
make -j"$(nproc)" > "$BUILD_LOG" 2>&1
rc=$?
grep -vE 'backtrace support|set but not used|warning:|\^|Vec3 |lane_pos|player_pos' "$BUILD_LOG" | tail -15
if [ "$rc" -ne 0 ]; then
  echo "BUILD FAILED (see $BUILD_LOG)"; exit 1
fi
[ -f t2k.3dsx ] || { echo "no .3dsx"; exit 1; }

echo "== launch =="
nohup "$APPIMAGE" "$PWD/t2k.3dsx" >/tmp/mandarine.log 2>&1 &
sleep "$WAIT"

# Find the largest Mandarine window and capture it.
WID=""
for w in $(xdotool search --name "Mandarine" 2>/dev/null); do
  h=$(xdotool getwindowgeometry "$w" 2>/dev/null | grep -oE '[0-9]+x[0-9]+' | cut -dx -f2)
  [ -n "${h:-}" ] && [ "$h" -gt 1000 ] && WID="$w" && break
done
[ -z "$WID" ] && WID=$(xdotool search --name "Mandarine" 2>/dev/null | head -1)
import -window "$WID" "$SHOT" 2>/dev/null && echo "SHOT $SHOT ($(stat -c%s "$SHOT") bytes, win $WID)" || echo "capture failed"
