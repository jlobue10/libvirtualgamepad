#!/usr/bin/env python3
"""Decodes captures/sc26-steam-control.tsv (written by Capture-SteamController.ps1 -Phase Steam)
into the ordered feature-report conversation between Steam and the controller: every SET_REPORT
command ([type][length][payload]) and the GET_REPORT reply that follows it, then the distinct
commands, the union of SET_SETTINGS_VALUES triples and every reply. Output is plain text; the
checked-in copy is captures/sc26-steam-handshake.txt.

    python tools/capture/Decode-Sc26Handshake.py [captures/sc26-steam-control.tsv]
"""
import csv
import sys
from collections import Counter, defaultdict

path = sys.argv[1] if len(sys.argv) > 1 else 'captures/sc26-steam-control.tsv'
rows = list(csv.reader(open(path, encoding='utf-8-sig'), delimiter='\t'))[1:]
seq = []
pending = None
for r in rows:
    direction, request_type, payload, t = r[2], r[3], r[11], float(r[1])
    if direction == '0x00' and request_type == '0x21':      # SET_REPORT (class, host -> interface)
        seq.append(('SET', t, payload))
    elif direction == '0x00' and request_type == '0xa1':    # GET_REPORT request
        pending = t
    elif direction == '0x01' and pending is not None and len(payload) == 128:
        seq.append(('GET', t, payload))                    # the 64-byte completion that answers it
        pending = None

NUL = b'\x00'


def fmt(payload):
    b = bytes.fromhex(payload)
    typ, length, body = b[1], b[2], b[3:3 + b[2]]
    s = f"type=0x{typ:02x} len={length:2d} payload={body.hex()}"
    if typ == 0xae and length > 1:
        s += f" tag=0x{body[0]:02x} text={body[1:].split(NUL)[0].decode('ascii', 'replace')!r}"
    return s


print(f"Steam <-> Steam Controller (2026) feature-report conversation, from {path}")
print(f"{len(seq)} transfers (SET = Steam writes a command, GET = the controller's reply Steam reads)\n")
for k, (kind, t, payload) in enumerate(seq):
    print(f"{k:3d} {t:8.3f}s {kind} {fmt(payload)}")

count = Counter()
distinct = defaultdict(set)
for kind, t, payload in seq:
    if kind == 'SET':
        b = bytes.fromhex(payload)
        count[b[1]] += 1
        distinct[b[1]].add(payload[2:6 + 2 * b[2]])
print("\nSET commands by type (count, distinct payloads):")
for typ, n in sorted(count.items()):
    print(f"  0x{typ:02x}: {n} sets, {len(distinct[typ])} distinct")
print("\nDistinct payloads of the non-0x87 commands ([type][len][payload]):")
for typ in sorted(distinct):
    if typ != 0x87:
        for p in sorted(distinct[typ]):
            print(f"  {p}")
triples = Counter()
for kind, t, payload in seq:
    if kind == 'SET':
        b = bytes.fromhex(payload)
        if b[1] == 0x87:
            body = b[3:3 + b[2]]
            for i in range(0, len(body) - 2, 3):
                triples[(body[i], int.from_bytes(body[i + 1:i + 3], 'little'))] += 1
print("\n0x87 SET_SETTINGS_VALUES triples (setting id = value) seen, with counts:")
for (i, v), n in sorted(triples.items()):
    print(f"  setting {i:3d} = 0x{v:04x} ({v})  x{n}")
print("\nReplies:")
for kind, t, payload in seq:
    if kind == 'GET':
        print(f"  {t:8.3f}s {fmt(payload)}")
