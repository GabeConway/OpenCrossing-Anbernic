#!/usr/bin/env bash
# Guard against the three hand-maintained launcher copies drifting apart
# (assemble.sh does not copy them; a stale copy once nearly shipped without
# the PipeWire audio fix — kb/build-test.md).
#
# Lines that legitimately differ between the muOS release launcher and the
# PortMaster-catalog submission are normalized away:
#   GAMEDIR=...      (ac-gc vs acgc port dir)
#   PORT_32BIT=...   (catalog builds set it for 64-bit CFWs)
# Blank-line differences are ignored (diff -B).
set -u
cd "$(dirname "$0")/.."

A="port_files/Animal Crossing.sh"
B="portmaster/Animal Crossing.sh"
C="portmaster/pm-submission/acgc/Animal Crossing.sh"

normalize() { grep -vE '^GAMEDIR=|PORT_32BIT|^# 32-bit ARM' "$1"; }

fail=0
for pair in "$A|$B" "$A|$C"; do
    x="${pair%|*}"; y="${pair#*|}"
    if ! diff -B <(normalize "$x") <(normalize "$y") > /tmp/launcher-sync-diff.$$; then
        echo "DRIFT between '$x' and '$y':"
        cat /tmp/launcher-sync-diff.$$
        fail=1
    fi
    rm -f /tmp/launcher-sync-diff.$$
done

if [ "$fail" -eq 0 ]; then
    echo "LAUNCHERS-IN-SYNC (3 copies, normalized)"
else
    echo "Launcher copies have drifted — sync port_files/ -> portmaster/ before packaging." >&2
fi
exit "$fail"
