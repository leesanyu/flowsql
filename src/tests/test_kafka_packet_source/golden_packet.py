#!/usr/bin/env python3
# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Independent probe implementation of the frozen wire format; no C++ codec use."""
import pathlib
import struct
import sys


def sample():
    # Deliberately non-IP/truncated bytes. Wire validity is independent of protocol validity.
    raw = bytes.fromhex('00112233445566778899aabb0800ff')
    header = struct.pack('>4sHH16sQQQII', b'FSPK', 1, 0, bytes(range(1, 17)),
                         2**64 - 1, 2**64 - 2, 2**64 - 3, 1, 1)
    packet = struct.pack('>IqIIIQ', 2**32 - 1, 2**63 - 1, len(raw), 65535, 1, 2**64 - 4)
    fact = struct.pack('>IIQqBBH', 2**32 - 1, 2**32 - 2, 2**64 - 5, 2**63 - 1, 1, 1, 0)
    assert len(header) == 56 and len(packet) == 32 and len(fact) == 28
    return header + packet + raw + fact


if __name__ == '__main__':
    path = pathlib.Path(__file__).with_name('golden_packet.hex')
    if '--write' in sys.argv:
        path.write_text(sample().hex() + '\n')
    else:
        assert bytes.fromhex(path.read_text()) == sample()
        print('PASS independent big-endian probe golden sample')
