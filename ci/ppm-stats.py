#!/usr/bin/env python3
# Count pixels in a binary PPM (P6) file, optionally restricted to the top
# rows. Used by ci/render-smoke.sh to check that a window is actually visible
# (colored decoration pixels present) and that title text is rasterized.
#
# Usage: ppm-stats.py <file.ppm> <mode> [y_max]
#   mode: red      -> saturated red pixels (border / titlebar background)
#         white    -> near-white pixels (title text on a red titlebar)
#         nonblack -> any pixel that is not essentially black
#   y_max: only count pixels with y < y_max (physical pixels)
import sys

path, mode = sys.argv[1], sys.argv[2]
ymax = int(sys.argv[3]) if len(sys.argv) > 3 else None

d = open(path, "rb").read()
i = 0


def token():
    global i
    while d[i] in b" \t\r\n":
        i += 1
    s = i
    while d[i] not in b" \t\r\n":
        i += 1
    return d[s:i]


# P6 header: magic, width, height, maxval (no comments expected from grim).
assert token() == b"P6", "not a binary PPM"
w = int(token())
h = int(token())
mx = int(token())
i += 1  # single whitespace after maxval
px = d[i:]

count = 0
for y in range(h):
    if ymax is not None and y >= ymax:
        break
    row = y * w * 3
    for x in range(w):
        k = row + x * 3
        r, g, b = px[k], px[k + 1], px[k + 2]
        if mode == "red":
            if r > 150 and g < 90 and b < 90:
                count += 1
        elif mode == "white":
            if r > 200 and g > 200 and b > 200:
                count += 1
        elif mode == "nonblack":
            if r + g + b > 40:
                count += 1
        else:
            sys.stderr.write("unknown mode %s\n" % mode)
            sys.exit(2)
print(count)
