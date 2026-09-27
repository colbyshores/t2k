#!/usr/bin/env bash
# Push a build to the console over ftpd (ftpd-pro, port 5000).
#
#   t2k_3ds/tools/3ds_push.sh [ip]          the development t2k.3dsx
#   t2k_3ds/tools/3ds_push.sh --cia [ip]    the installable t2k.cia
#   t2k_3ds/tools/3ds_push.sh --fast [ip]   skip the gate suite (see below)
#
# THE GATE SUITE RUNS BEFORE EVERY PUSH unless --fast / T2K_SKIP_CHECK=1. A push
# puts a binary in front of a person, and a console session is the most expensive
# verifier available here -- minutes of hands-on time against seconds of CI. Until
# now the push script would ship anything, including a tree whose own gates were
# already red, so the human was the first to find out. Run the cheap checks first
# and let the device test be for the things only a device can tell you.
#
# --fast exists because the suite is not free (it rebuilds pc + 3ds). Use it when
# you have just run tools/check.sh and only the artifact moved.
#
# (default 192.168.4.39; env T2K_3DS_IP). The lease MOVES -- .30, .24, .26, now
# .39 -- so the default is only ever a best guess. If it fails, scan the subnet
# for port 5000, but note the Synology boxes (.251/.253) answer on it too, so a
# hit there is not the 3DS.
#
# The console must be on with ftpd running. The .3dsx lands in sdmc:/3ds/; the
# .cia lands in sdmc:/cia/, which is where FBI looks -- pushing it does NOT
# install it, you still install from FBI on the console.
set -u
cd "$(dirname "$0")/.."
MODE=3dsx
FAST="${T2K_SKIP_CHECK:-0}"
while [ "${1:-}" = "--fast" ] || [ "${1:-}" = "--cia" ]; do
    if [ "${1:-}" = "--fast" ]; then FAST=1; else MODE=cia; fi
    shift
done
IP="${1:-${T2K_3DS_IP:-192.168.4.39}}"

if [ "$FAST" = 1 ]; then
    echo "== gate suite SKIPPED (--fast / T2K_SKIP_CHECK) =="
elif ! bash ../tools/check.sh; then
    echo "PUSH ABORTED: the gate suite is red." >&2
    echo "Fix it, or push with --fast if you have already run tools/check.sh" >&2
    echo "yourself and only the artifact moved." >&2
    exit 1
fi

if [ "$MODE" = cia ]; then
    [ -f t2k.cia ] || { echo "no t2k.cia -- run 'make production' first"; exit 1; }
    SZ=$(stat -c%s t2k.cia)
    echo "== push t2k.cia ($((SZ/1024/1024)) MB) -> ftp://$IP:5000/cia/t2k.cia =="
    echo "   this is a few hundred MB over a 3DS's FTP; expect minutes, not seconds"
    # --max-time scaled to the payload: the soundtrack makes this ~300 MB and a
    # 3DS writes it at a few MB/s at best. A 60 s cap (the .3dsx default below)
    # would abort it a fifth of the way in and leave a truncated file that FBI
    # would happily try to install.
    if curl --connect-timeout 5 --max-time 5400 -# -T t2k.cia "ftp://$IP:5000/cia/t2k.cia"; then
        echo "pushed $SZ bytes"
    else
        echo "PUSH FAILED: is the 3DS on and ftpd running at $IP:5000?"; exit 1
    fi
    echo "== on the SD now =="
    REMOTE=$(curl -s --max-time 20 "ftp://$IP:5000/cia/" | awk '$NF=="t2k.cia"{print $(NF-4)}')
    curl -s --max-time 20 "ftp://$IP:5000/cia/" | grep -E ' t2k\.cia' || true
    if [ "${REMOTE:-}" = "$SZ" ]; then
        echo "size matches the local file ($SZ bytes) -- complete"
    else
        echo "SIZE MISMATCH: local $SZ, remote ${REMOTE:-none} -- the transfer is INCOMPLETE"
        echo "do not install this; re-run the push"
        exit 1
    fi
    echo "now install it from FBI on the console: SD -> cia -> t2k.cia"
    exit 0
fi

[ -f t2k.3dsx ] || { echo "no t2k.3dsx -- run 'make 3ds' first"; exit 1; }
# TWO copies, deliberately: the Homebrew Launcher entry the user launches is
# /3ds/t2k.3dsx (folder root), and /3ds/t2k/ is the game's own folder. On
# 2026-08-25 a push to the folder alone left a stale Aug-21 build at the root
# and a whole "the 3DS is missing the fades" investigation followed.
ok=1
for dst in /3ds/t2k.3dsx /3ds/t2k/t2k.3dsx; do
  echo "== push t2k.3dsx -> ftp://$IP:5000$dst =="
  if curl --max-time 60 --connect-timeout 5 -sS -T t2k.3dsx "ftp://$IP:5000$dst"; then
    echo "pushed $(stat -c%s t2k.3dsx) bytes"
  else
    echo "PUSH FAILED for $dst: is the 3DS on and ftpd running at $IP:5000?"; ok=0
  fi
done
echo "== on the SD now =="
curl -s --max-time 10 "ftp://$IP:5000/3ds/" | grep -E ' t2k\.3dsx' || true
curl -s --max-time 10 "ftp://$IP:5000/3ds/t2k/" | grep -E ' t2k\.3dsx' || true
[ "$ok" = 1 ] || exit 1
