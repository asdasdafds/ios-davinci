#!/usr/bin/env python3
"""Wrap a SWEETXNU payload as SWEETLZ4 for the shim (davinci ABL rejects
`fastboot boot` ramdisks of ~16 MiB and more). Needs the `lz4` CLI.
Usage: lz4payload.py IN.bin OUT.lz4"""
import struct, subprocess, sys

src, dst = sys.argv[1], sys.argv[2]
raw = open(src, 'rb').read()
if raw[:8] != b'SWEETXNU':
    sys.exit('input is not a SWEETXNU payload')
# -l: legacy frame (magic 0x184c2102, [u32 size][block]...), what loader.c decodes.
z = subprocess.run(['lz4', '-l', '-9', '-c', src], stdout=subprocess.PIPE, check=True).stdout
with open(dst, 'wb') as f:
    f.write(b'SWEETLZ4' + struct.pack('<Q', len(raw)) + z)
print(f'{src}: {len(raw)} -> {dst}: {16 + len(z)} bytes')
