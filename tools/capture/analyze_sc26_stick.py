"""Per-report stick statistics of the real SC26's 0x42 reports in the Steam capture."""
import math
import struct
import sys
from collections import Counter

path = sys.argv[1]
reports = []  # (time, seq, buttons, lx, ly, rx, ry, ts)
with open(path, encoding="utf-8") as f:
    next(f)
    for line in f:
        parts = line.rstrip("\n").split("\t")
        if len(parts) < 5 or parts[2] != "0x81":
            continue
        b = bytes.fromhex(parts[4])
        if len(b) < 54 or b[0] != 0x42:
            continue
        seq = b[1]
        buttons = struct.unpack_from("<I", b, 2)[0]
        lx, ly, rx, ry = struct.unpack_from("<hhhh", b, 10)
        ts = struct.unpack_from("<I", b, 30)[0]
        reports.append((float(parts[1]), seq, buttons, lx, ly, rx, ry, ts))

print("0x42 reports:", len(reports))
# cadence
dt = [reports[i][0] - reports[i - 1][0] for i in range(1, len(reports))]
dts = [(reports[i][7] - reports[i - 1][7]) & 0xFFFFFFFF for i in range(1, len(reports))]
print("usb interval ms: min %.2f median %.2f max %.2f" % (min(dt) * 1e3, sorted(dt)[len(dt) // 2] * 1e3, max(dt) * 1e3))
print("imu_timestamp step us: min %d median %d max %d" % (min(dts), sorted(dts)[len(dts) // 2], max(dts)))
seqgaps = sum(1 for i in range(1, len(reports)) if ((reports[i][1] - reports[i - 1][1]) & 0xFF) != 1)
print("sequence gaps:", seqgaps)

# stick behaviour
same = sum(1 for i in range(1, len(reports)) if reports[i][3:5] == reports[i - 1][3:5])
print("consecutive reports with identical left stick: %d of %d (%.1f%%)" % (same, len(reports) - 1, 100.0 * same / (len(reports) - 1)))
centre = [r for r in reports if math.hypot(r[3], r[4]) < 3276]
print("left stick near centre in %d reports; distinct centre values: %d" % (len(centre), len(set((r[3], r[4]) for r in centre))))
if centre:
    cnt = Counter((r[3], r[4]) for r in centre)
    print("  most common centre values:", cnt.most_common(5))

# rim segments: consecutive reports with magnitude > 0.8
mags = [math.hypot(r[3], r[4]) / 32767.0 for r in reports]
print("left magnitude max %.3f; reports > 0.95: %d; > 1.0: %d" % (max(mags), sum(1 for m in mags if m > 0.95), sum(1 for m in mags if m > 1.0)))
segs = []
start = None
for i, m in enumerate(mags):
    if m > 0.8 and start is None:
        start = i
    elif m <= 0.8 and start is not None:
        if i - start > 50:
            segs.append((start, i))
        start = None
print("rim segments longer than 50 reports:", len(segs))
for (a, b) in segs[:12]:
    angles = [math.atan2(reports[i][4], reports[i][3]) for i in range(a, b)]
    steps = []
    for i in range(1, len(angles)):
        d = abs(angles[i] - angles[i - 1]) * 180 / math.pi
        if d > 180:
            d = 360 - d
        steps.append(d)
    sectors = set(int((ang + math.pi) / (2 * math.pi) * 16) % 16 for ang in angles)
    ident = sum(1 for i in range(a + 1, b) if reports[i][3:5] == reports[i - 1][3:5])
    total = sum(steps)
    print("  reports %d..%d (%.1f s): %d reports, sectors %d/16, total sweep %.0f deg, step deg median %.2f max %.1f, identical consecutive %d, mag %.2f..%.2f" % (
        a, b, reports[b - 1][0] - reports[a][0], b - a, len(sectors), total, sorted(steps)[len(steps) // 2], max(steps), ident, min(mags[a:b]), max(mags[a:b])))
