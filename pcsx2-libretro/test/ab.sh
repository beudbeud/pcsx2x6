#!/bin/sh
# Alternated A/B of cores with the headless harness, on the Pi (busybox sh).
#   ab.sh "<game>" <frames> <rounds> /tmp/coreA.so /tmp/coreB.so ...
# Game = basename shared by the .acgame and the RetroArch .state. Prints one line
# per run: STEADY fps, then the per-second fps from the state load onwards (the
# SC3 demo only fights ~30 s after the load: read those, not just the average).
# Stop ES first (/etc/init.d/S31emulationstation stop): it takes CPU and DRM.
G="$1"; F="$2"; N="$3"; shift 3
ROM="${ROMDIR:-/recalbox/share/externals/usb0/recalbox/roms/namco2x6}/$G.acgame"
ST="${STATEDIR:-/recalbox/share/saves/namco2x6}/$G.state"
# An arm is a core, or core@libdir to run it against another mesa (LD_LIBRARY_PATH).
# SHOT=<frame>[,<frame>] saves /tmp/shots-<game>-<arm>/frame-NNNNNN.png to check the scene.
i=1
while [ "$i" -le "$N" ]; do
	for A in "$@"; do
		C="${A%@*}"
		LIB=""
		[ "$C" != "$A" ] && LIB="${A#*@}"
		NAME="$(basename "$C" .so)${LIB:+@$(basename "$LIB")}"
		L="/tmp/ab-$NAME-$i.log"
		D="/tmp/shots-$(echo "$G" | tr ' ' '_')-$NAME" # per game: runs must not overwrite each other
		mkdir -p "$D"
		LD_LIBRARY_PATH="$LIB" PCSX2X6_TEST_STATE="$ST" /tmp/test_pcsx2x6 "$C" "$ROM" /recalbox/share/bios "$F" \
			"" "$D" "${SHOT:-}" > "$L" 2>&1
		S=$(sed -n 's/.*state load at frame \([0-9]*\).*/\1/p' "$L")
		printf '%-20s #%d STEADY %s | %s\n' "$NAME" "$i" \
			"$(grep STEADY "$L" | awk '{print $4}')" \
			"$(awk -v s="${S:-0}" '$2 == "frame" && $3 + 0 > s { sub("fps=", "", $5); printf "%d ", $5 }' "$L")"
	done
	i=$((i + 1))
done
