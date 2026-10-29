#!/usr/bin/env python3
"""Kernel-step archive entry: turn .cache/firmware/mali_csffw.bin into kits/evb10/Mali/mali_csffw.inc.

The firmware is a vendor binary and is neither committed nor embedded in any repository: put it in
.cache/firmware with scripts/extract-files.py first. The generated include is ignored by the Mali
repository's .gitignore.
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
firmware = ROOT / '.cache/firmware/mali_csffw.bin'
generator = ROOT / 'kits/evb10/Mali/tools/gen-fw-inc.py'
if not firmware.is_file():
    sys.exit(f'{firmware} is missing: run scripts/extract-files.py SOURCE (see proprietary-files.txt)')
if not generator.is_file():
    sys.exit(f'{generator} is missing: initialise the kits/evb10/Mali submodule')
subprocess.run([sys.executable, str(generator), str(firmware)], check=True, env={**os.environ, 'MALI_CSFFW': str(firmware)})
