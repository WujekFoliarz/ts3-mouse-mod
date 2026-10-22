#!/usr/bin/env python3
"""Read-only PE/build/call-site verification. Python 3 standard library only."""
import argparse
import hashlib
import struct
from pathlib import Path

SHA256 = 'f87941b418a56b935a5e8372c11720304410d204a8cabd1d9970becfceff9722'
SITES = {
    0x27B8F7: bytes.fromhex('ba 14 00 00 00 ff 50 48'),
    0x21ABC1: bytes.fromhex('e8 ea 34 fe ff'),
    0x1FE356: bytes.fromhex('e8 a5 3a 05 00'),
    0x27BA1B: bytes.fromhex('ff 90 d8 00 00 00'),
    0x0AF528: bytes.fromhex('e8 a3 63 fd ff'),
    0x0859F6: bytes.fromhex('e8 e5 a8 fb ff'),
    0x27B943: bytes.fromhex('43 8d 04 40 3b c2'),
}

def verify(path):
    data = Path(path).read_bytes()
    assert hashlib.sha256(data).hexdigest() == SHA256, 'Unsupported executable SHA-256'
    assert data[:2] == b'MZ', 'Not a PE executable'
    nt, = struct.unpack_from('<I', data, 0x3C)
    assert data[nt:nt + 4] == b'PE\0\0'
    machine, sections, timestamp = struct.unpack_from('<HHI', data, nt + 4)
    assert machine == 0x8664 and timestamp == 0x6ABF29B6
    optional_size, = struct.unpack_from('<H', data, nt + 20)
    assert struct.unpack_from('<I', data, nt + 24 + 56)[0] == 0xBDC000
    table = nt + 24 + optional_size

    def offset(rva):
        for i in range(sections):
            header = table + i * 40
            _, va, rawsize, raw = struct.unpack_from('<IIII', data, header + 8)
            if va <= rva < va + rawsize:
                return raw + rva - va
        raise AssertionError(f'RVA {rva:X} outside file-backed sections')

    for rva, expected in SITES.items():
        actual = data[offset(rva):offset(rva) + len(expected)]
        assert actual == expected, f'Unexpected instructions at RVA {rva:X}'
        print(f'PASS RVA 0x{rva:06X}: {actual.hex(" ")}')
    for call, target in [(0x21ABC1, 0x1FE0B0), (0x1FE356, 0x251E00),
                         (0x0AF528, 0x0858D0), (0x0859F6, 0x0402E0)]:
        disp, = struct.unpack_from('<i', data, offset(call) + 1)
        assert call + 5 + disp == target, 'Wrong native call target'
    assert struct.unpack_from('<Q', data, offset(0x7BC938 + 0xD8))[0] == 0x14027AD90
    print('PASS SHA-256, AMD64 headers, call targets, keyboard vtable')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('exe', type=Path)
    args = parser.parse_args()
    try:
        verify(args.exe)
    except (OSError, AssertionError, struct.error) as error:
        parser.exit(1, f'FAIL: {error}\n')
