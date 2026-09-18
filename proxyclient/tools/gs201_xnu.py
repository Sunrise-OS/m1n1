#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Upload a VMAPPLE XNU Mach-O over the GS201 USB proxy and boot it.

Fast iteration path: no fastboot re-flash per kernel change. The device
keeps its EL2 HVC/PAC shim live and enters the kernel at EL1 with the
MMU off, same handoff as the appended-payload boot.

Usage:
    ./proxyclient/tools/gs201_xnu.py /path/to/kernel.development.vmapple
    ./proxyclient/tools/gs201_xnu.py kernel --bootargs "-v keepsyms=1" --patch-hvc0
"""
import sys, pathlib, subprocess, struct
sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

import argparse

parser = argparse.ArgumentParser(description="GS201 XNU upload-and-boot via proxy")
parser.add_argument("kernel", type=pathlib.Path,
                    help="Mach-O kernel (e.g. kernel.development.vmapple)")
parser.add_argument("-b", "--bootargs", default="",
                    help="XNU cmdline (default: firmware FDT bootargs)")
parser.add_argument("--patch-hvc0", action="store_true",
                    help="NOP the initial VMAPPLE PAC HVC (fallback for no-EL2 hosts)")
parser.add_argument("--no-compress", action="store_true",
                    help="Upload raw instead of gzip+gzdec (slow, but isolates gunzip issues)")
parser.add_argument("--func-addr", type=lambda x: int(x, 0), default=None,
                    help="gs201_xnu_boot runtime address (default: u.base + nm offset)")
parser.add_argument("--elf", type=pathlib.Path,
                    default=pathlib.Path(__file__).resolve().parents[2] / "build" / "m1n1-raw.elf",
                    help="ELF used to resolve gs201_xnu_boot")
args = parser.parse_args()

from m1n1.setup import p, u, iface

# --- optional narrow HVC0 patch (mirrors xnu-work/patchvmapple) ---
INITIAL_HVC = bytes.fromhex("00 20 b8 d2 02 00 00 d4 00 00 00 b5")
RETURN_SUCCESS = bytes.fromhex("e0 03 1f aa")  # mov x0, xzr

def text_exec_range(data: bytes):
    if len(data) < 32 or data[:4] != struct.pack("<I", 0xfeedfacf):
        raise ValueError("not a 64-bit Mach-O")
    ncmds, sizeofcmds = struct.unpack_from("<II", data, 16)
    off = 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == 0x19:  # LC_SEGMENT_64
            segname = data[off+8:off+24].split(b"\0")[0].decode()
            if segname == "__TEXT_EXEC":
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", data, off+24)
                return fileoff, fileoff + filesize
        off += cmdsize
    raise ValueError("no __TEXT_EXEC segment")

data = bytearray(args.kernel.read_bytes())
print(f"Read {len(data)} bytes from {args.kernel}")
if args.patch_hvc0:
    start, end = text_exec_range(data)
    first = data.find(INITIAL_HVC, start, end)
    if first < 0:
        raise SystemExit("initial VMAPPLE PAC HVC signature not found; refusing to patch")
    if data.find(INITIAL_HVC, first + 1, end) >= 0:
        raise SystemExit("initial VMAPPLE PAC HVC signature is ambiguous; refusing to patch")
    data[first+4:first+8] = RETURN_SUCCESS
    print(f"Patched initial PAC HVC at file offset 0x{first+4:x} (hvc #0 -> mov x0, xzr)")

# --- resolve gs201_xnu_boot ---
if args.func_addr is not None:
    func_addr = args.func_addr
else:
    out = subprocess.run(["nm", str(args.elf)], capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit(f"nm {args.elf} failed; pass --func-addr explicitly")
    func_off = None
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == "gs201_xnu_boot":
            func_off = int(parts[0], 16)
            break
    if func_off is None:
        raise SystemExit("gs201_xnu_boot not found in ELF; rebuild with PLATFORM=gs201?")
    func_addr = (u.base + func_off) & ((1 << 64) - 1)
    print(f"gs201_xnu_boot: file offset 0x{func_off:x} + base 0x{u.base:x} = 0x{func_addr:x}")

if args.no_compress:
    print("--no-compress: raw upload (no gzip, no device gunzip)")
    import time
    payload_addr = u.malloc(len(data))
    t0 = time.time()
    print(f"Uploading {len(data)} bytes to 0x{payload_addr:x}...")
    iface.writemem(payload_addr, bytes(data), True)
    t1 = time.time()
    dt = max(t1 - t0, 0.01)
    print(f"Upload complete in {t1-t0:.1f}s ({len(data)/dt/1024:.0f} KiB/s)")
else:
    # --- upload (gzip through the proxy; the kernel is ~2.5x compressible) ---
    import gzip, time
    print("Compressing...")
    t0 = time.time()
    payload = gzip.compress(bytes(data), compresslevel=6)
    t1 = time.time()
    print(f"Compressed {len(data)} -> {len(payload)} bytes "
          f"({len(payload)/len(data):.2f}) in {t1-t0:.1f}s")

    payload_addr = u.malloc(len(data))
    compressed_addr = u.malloc(len(payload))
    t0 = time.time()
    print(f"Uploading {len(payload)} bytes to 0x{compressed_addr:x}...")
    iface.writemem(compressed_addr, payload, True)
    t1 = time.time()
    dt = max(t1 - t0, 0.01)
    print(f"Upload complete in {t1-t0:.1f}s ({len(payload)/dt/1024:.0f} KiB/s)")

    # Alive + integrity check before gunzip: ping, then spot-read magic.
    p.nop()
    print("Device alive after upload.")
    head = iface.readmem(compressed_addr, 16)
    want = payload[:16]
    assert bytes(head) == bytes(want), f"readback mismatch: {bytes(head).hex()} != {bytes(want).hex()}"
    print(f"Readback OK ({bytes(head).hex()}).")

    print(f"Decompressing on device to 0x{payload_addr:x}...")
    t0 = time.time()
    old_timeout = iface.dev.timeout
    iface.dev.timeout = 180
    try:
        got = p.gzdec(compressed_addr, len(payload), payload_addr, len(data))
    finally:
        iface.dev.timeout = old_timeout
    t1 = time.time()
    assert got == len(data), f"gzdec size mismatch: {got} != {len(data)}"
    print(f"Decompressed {got} bytes in {t1-t0:.1f}s")
    u.free(compressed_addr)
    # Verify the Mach-O magic survived the round trip.
    magic = iface.readmem(payload_addr, 4)
    assert bytes(magic) == bytes(data[:4]), f"payload magic mismatch: {bytes(magic).hex()}"
    print("Payload magic OK.")

cmdline = args.bootargs or None
if cmdline:
    print(f"Cmdline override: {cmdline!r}")
else:
    print("No cmdline override; device falls back to firmware FDT bootargs.")
    cmdline = 0

print(f"Calling gs201_xnu_boot(0x{payload_addr:x}, ...) — proxy will go away, watch the framebuffer.")
# Destructive, never returns: send without waiting for a reply.
p.request(p.P_CALL, func_addr, payload_addr, cmdline, no_reply=True)
print("Boot request sent (no reply expected).")
