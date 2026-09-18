#!/usr/bin/env bash
# Build a `fastboot boot`-able Android boot image for GS201/lynx.
#
#   mkimage-lynx.sh <m1n1.bin> <out.img> [xnu-macho]
#
# An XNU payload (if given) is appended to the kernel image at m1n1's
# _payload_start, which is where the gs201 platform looks for it.  ABL
# requires an AVB footer even for `fastboot boot`, so an empty vbmeta/footer
# pair is appended (same convention as the u-boot port).
set -euo pipefail

BIN="$1"
OUT="$2"
PAYLOAD="${3:-}"
ELF="${BIN%.bin}-raw.elf"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

KERNEL="$tmp/kernel.bin"

if [[ -n "$PAYLOAD" ]]; then
	[[ -f "$PAYLOAD" ]] || { echo "mkimage-lynx: no such payload: $PAYLOAD" >&2; exit 1; }

	offset="$(nm "$ELF" | awk '$3 == "_payload_start" { print $1 }')"
	[[ -n "$offset" ]] || { echo "mkimage-lynx: _payload_start not found in $ELF" >&2; exit 1; }
	offset=$((16#$offset))

	cp "$BIN" "$KERNEL"
	cur=$(stat -c%s "$KERNEL")
	if ((cur > offset)); then
		echo "mkimage-lynx: $BIN ($cur bytes) overruns _payload_start ($offset)" >&2
		exit 1
	fi
	truncate -s "$offset" "$KERNEL"
	cat "$PAYLOAD" >>"$KERNEL"
else
	cp "$BIN" "$KERNEL"
fi

empty="$tmp/empty"
: >"$empty"

mkbootimg \
	--kernel "$KERNEL" \
	--ramdisk "$empty" \
	--header_version 4 \
	--pagesize 4096 \
	--cmdline "console=ttySAC0,115200n8 androidboot.hardware=gs201 androidboot.serialconsole=1" \
	--output "$OUT"

python3 - "$OUT" <<'PY'
import sys
from pathlib import Path

p = Path(sys.argv[1])
img = p.read_bytes()
avbf = img.rfind(b"AVBf")
if avbf != -1 and avbf >= len(img) - 64:
    original = int.from_bytes(img[avbf + 12:avbf + 20], "big")
    if 0 < original <= avbf:
        img = img[:original]

vbmeta = bytearray(256)
vbmeta[:4] = b"AVB0"
vbmeta[4:8] = (1).to_bytes(4, "big")
footer = bytearray(64)
footer[:4] = b"AVBf"
footer[4:8] = (1).to_bytes(4, "big")
footer[12:20] = len(img).to_bytes(8, "big")
footer[20:28] = len(img).to_bytes(8, "big")
footer[28:36] = (256).to_bytes(8, "big")
p.write_bytes(img + vbmeta + footer)
print(f"wrote {p} ({p.stat().st_size} bytes)")
PY
