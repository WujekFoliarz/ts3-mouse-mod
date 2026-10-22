#!/usr/bin/env python3
"""Execute the verified game's actual axis filter before/after the v2 patch.
Linux x86-64 only. Reads the local EXE; does not launch or modify the game.
"""
import ctypes
import mmap
import platform
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from verify_game import verify

if sys.platform != 'linux' or platform.machine() != 'x86_64':
    raise SystemExit('This fixture requires Linux x86-64.')
verify(sys.argv[1])
# This build's .text file offset is RVA - 0xC00 (verified SHA-256 above).
data = Path(sys.argv[1]).read_bytes()
block = data[0x27B932 - 0xC00:0x27B963 - 0xC00]
# SysV fixture: (manager=RDI, x=ESI, y=EDX). Match the game's live registers,
# preserving the nonvolatile RBX/R14 used by the original instructions.
prologue = bytes.fromhex('53 41 56 48 89 fb 89 f1 89 d0 45 31 f6')
epilogue = bytes.fromhex('41 5e 5b c3')
code = mmap.mmap(-1, mmap.PAGESIZE, prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
code.write(prologue + block + epilogue)
entry = ctypes.addressof(ctypes.c_char.from_buffer(code))
run = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int, ctypes.c_int)(entry)
manager = ctypes.create_string_buffer(0x100)
xout = ctypes.c_int.from_buffer(manager, 0xDC)
yout = ctypes.c_int.from_buffer(manager, 0xE0)
def sample(x, y):
    xout.value, yout.value = x, y
    run(manager, x, y)
    return xout.value, yout.value
assert sample(4, 1) == (4, 0), 'fixture did not reproduce horizontal snapping'
assert sample(1, 4) == (0, 4), 'fixture did not reproduce vertical snapping'
# Same six bytes installed by InstallPatches: jump to original block end.
code.seek(len(prologue) + 0x27B943 - 0x27B932)
code.write(bytes.fromhex('e9 1b 00 00 00 90'))
for x, y in [(4,1),(1,4),(-4,1),(4,-1),(-1,-4),(1000,1),(1,1000),(1,1),(0,0)]:
    assert sample(x,y) == (x,y), f'patch lost mouse displacement {(x,y)}'
print('PASS: original game instructions reproduce axis snapping; v2 preserves every tested diagonal.')
