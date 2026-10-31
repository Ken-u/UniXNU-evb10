#!/usr/bin/env python3
"""Writes package/Evb10Touch/touch_script.inc (git-ignored, compiled in, played by evb10-touch-script=SECS): unlock, Settings, Wi-Fi, the
network of .cache/firmware/join.txt, its passphrase typed on the on-screen keyboard, Join. The passphrase is only read here and turned into
key positions; nothing is printed.

  gen-wifi-touch-script.py            join
  gen-wifi-touch-script.py info       ... and open the network's page 30 s after the join
  gen-wifi-touch-script.py forget     ... and Forget This Network (page, button, confirmation)
  gen-wifi-touch-script.py rejoin     ... and join the same network again from the list

Positions are pixels of the 536x952 screenshot of this device (evb10-shot-img=1 evb10-shot-scale=2, tools/xnu-probe/shot2png.py in
uefi-evb10), the times are for a run whose first step plays at about 127 s (evb10-touch-script=99). The network is the first row of the list,
which is the only one with evb10-wifi=5 evb10-wifi-join=2 (the scans list just the join file's network). Rebuild the kernel after running it."""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JOIN = os.path.join(ROOT, '.cache/firmware/join.txt')
OUT = os.path.join(ROOT, 'package/Evb10Touch/touch_script.inc')
W, H = 536.0, 952.0   # the screenshot's pixels = the points of the screen

# key centres (pixels) of the iPad keyboard in the 536x952 screenshot: [unshifted, shifted] for the symbol keys
rows = {
    722: [(21, '`', '~'), (55, '1', '!'), (91, '2', '@'), (127, '3', '#'), (164, '4', '$'), (201, '5', '%'), (238, '6', '^'), (274, '7', '&'),
          (311, '8', '*'), (347, '9', '('), (384, '0', ')'), (421, '-', '_'), (458, '=', '+')],
    766: [(78, 'q', 'Q'), (114, 'w', 'W'), (150, 'e', 'E'), (187, 'r', 'R'), (223, 't', 'T'), (260, 'y', 'Y'), (297, 'u', 'U'), (333, 'i', 'I'),
          (370, 'o', 'O'), (407, 'p', 'P'), (443, '[', '{'), (480, ']', '}'), (516, '\\', '|')],
    813: [(84, 'a', 'A'), (120, 's', 'S'), (157, 'd', 'D'), (193, 'f', 'F'), (230, 'g', 'G'), (267, 'h', 'H'), (303, 'j', 'J'), (340, 'k', 'K'),
          (376, 'l', 'L'), (413, ';', ':'), (450, "'", '"')],
    860: [(103, 'z', 'Z'), (139, 'x', 'X'), (176, 'c', 'C'), (212, 'v', 'V'), (249, 'b', 'B'), (285, 'n', 'N'), (322, 'm', 'M'), (358, ',', '<'),
          (395, '.', '>'), (431, '/', '?')],
}
SHIFT = (42, 866)
SPACE = (240, 907)
keys = {}
for y, row in rows.items():
    for x, plain, shifted in row:
        keys[plain] = ((x, y), False)
        keys[shifted] = ((x, y), True)
keys[' '] = (SPACE, False)


def pm(p):
    return int(round(p[0] / W * 1000)), int(round(p[1] / H * 1000))


with open(JOIN) as f:
    lines = f.read().split('\n')
passphrase = lines[1] if len(lines) > 1 else ''

raw = []   # (ms, x_pm, y_pm, hold, dx, dy, comment)
raw.append((0, 500, 970, 800, 0, -620, 'swipe up from the bottom edge: unlock'))
raw.append((8000, 500, 970, 800, 0, -620, 'second swipe'))
raw.append((20000, 670, 924, 100, 0, 0, 'tap Settings in the Dock'))
raw.append((60000, 500, 331, 100, 0, 0, 'Wi-Fi row'))
raw.append((95000, 300, 254, 100, 0, 0, 'the first (only) network: the password sheet'))


def type_passphrase(t):
    """key taps for the passphrase from t on; returns the time of the Join tap"""
    step_ms = 1400
    for ch in passphrase:
        if ch not in keys:
            sys.exit('no key for a character of the passphrase (position %d)' % passphrase.index(ch))
        pos, shifted = keys[ch]
        if shifted:
            x, y = pm(SHIFT)
            raw.append((t, x, y, 100, 0, 0, 'shift'))
            t += step_ms
        x, y = pm(pos)
        raw.append((t, x, y, 100, 0, 0, 'key'))
        t += step_ms
    t += 2000
    raw.append((t, 929, 111, 100, 0, 0, 'Join'))
    return t


join_at = type_passphrase(114000)
args = sys.argv[1:]
if 'info' in args or 'forget' in args or 'rejoin' in args:
    raw.append((join_at + 30000, 908, 187, 100, 0, 0, 'the network info button'))
if 'forget' in args or 'rejoin' in args:
    x, y = pm((113, 131))
    raw.append((join_at + 36000, x, y, 100, 0, 0, 'Forget This Network'))
    x, y = pm((335, 537))
    raw.append((join_at + 42000, x, y, 100, 0, 0, 'Forget in the confirmation'))
if 'rejoin' in args:
    raw.append((join_at + 57000, 300, 254, 100, 0, 0, 'the network again: the password sheet'))
    rejoin_at = type_passphrase(join_at + 76000)
with open(OUT, 'w') as f:
    f.write('// local, not committed: see evb10-touch-script in Evb10Touch.cpp (written by scripts/gen-wifi-touch-script.py)\n')
    f.write('// (the first swipe after the boot does not unlock; the second does)\n')
    f.write('static const ScriptStep kScript[] = {\n')
    for ms, x, y, hold, dx, dy, comment in raw:
        f.write('  {%d, %d, %d, %d, %d, %d},   // %s\n' % (ms, x, y, hold, dx, dy, comment if comment != 'key' else 'key'))
    f.write('};\n')
print('steps:', len(raw), 'last at', raw[-1][0], 'ms; join at', join_at, 'ms')
