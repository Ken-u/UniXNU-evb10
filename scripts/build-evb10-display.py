#!/usr/bin/env python3
"""Archive builder of board.toml's [kernel] archives: the display archive, when this kernel links it.

configs/evb10_display_defconfig sets CONFIG_XNU_NATIVE_LEGACY_DISPLAY=y, which makes the kernel link force-load
build/native-legacy-display/libNativeLegacyDisplay.late.a (and refuse a stale one). That archive binds NativeLegacyDisplay to
the ORIGINAL IOMobileFramebufferLegacy, and the binding step needs a kernel whose prelink plan carries the original
IOMobileGraphicsFamily to read the original code from. Such a kernel is the product of an earlier pass, so the order is:

  1. a kernel of configs/evb10_defconfig (no display; its prelink roots already carry the graphics family),
  2. this script, which makes the stubs, the audit inputs and the archive from that kernel's kernel.macho,
  3. a kernel of configs/evb10_display_defconfig, which links the archive.

scripts/build-evb10-kernel.sh runs those passes. Under evb10_defconfig this script does nothing, so the display-less kernel
never depends on it.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
FAMILY = 'com.apple.iokit.IOMobileGraphicsFamily'

if os.environ.get('XNU_NATIVE_LEGACY_DISPLAY', '0') != '1':
    print('display archive: skipped (CONFIG_XNU_NATIVE_LEGACY_DISPLAY is not set in this configuration)')
    sys.exit(0)

# The configuration name names the output directory: the display-less pass A lives next to this one as .../evb10.
base = ROOT.parent.parent / 'evb10' / 'kernel'
plan_path = base / 'kernel.json'
if not plan_path.is_file():
    raise SystemExit('%s missing: build the display-less kernel first (scripts/build-evb10-kernel.sh does)' % plan_path)
if FAMILY not in json.loads(plan_path.read_text())['original_executable_sha256']:
    raise SystemExit('the previous kernel carries no original %s: build the display-less kernel first '
                     '(scripts/build-evb10-kernel.sh does)' % FAMILY)

for script in ('build-link-stubs.py', 'build-display-abi.py', 'build-native-legacy-display.py', 'build-evb10-hid.py'):
    print('display archive: %s' % script)
    subprocess.run([sys.executable, str(ROOT / 'scripts' / script)], check=True, cwd=ROOT)
