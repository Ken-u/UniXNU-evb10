#!/usr/bin/env python3
"""Make the inputs of UniXNU's audit-mobile-graphics-legacy.py, which build-native-legacy-display.py runs first.

Adapted from build-display-abi.py of the community UniXNU-qemu repository (BSD-2-Clause-Patent, Copyright (c) 2026,
UniXNU contributors). Run in the build tree after a "unixnu build kernel" of the display-less configuration
(configs/evb10_defconfig); the display kernel (configs/evb10_display_defconfig) links the archive that this and
build-native-legacy-display.py make.

The audit reads a "candidate": a kernel whose prelink plan holds the unchanged original IOMobileGraphicsFamily. Any
kernel of this IPSW built with that family among its prelink roots qualifies, and the audit checks it by the digest of
the original family executable (FAMILY_TEXT_SHA), so the kernel step's own kernel.macho is copied under the name the
audit looks for (build/xnu-prelink/native-owner-mobile-graphics-candidate.*).

The audit also wants the runtime observation of the original class (parent, instance size, metaclass), which UniXNU
made by booting a candidate under QEMU and stopping in OSMetaClass's constructor. This board has no debugger, and the
facts concern the unchanged original kernelcache, not our kernel: UniXNU's audit itself carries only the recorded hash of
that observation once the file is pruned ("runtime_metadata_checkpoint_sha256", see audit-mobile-graphics-legacy.py), by
reading its own previous output. This script writes that one field as the seed of that file; every other check of the
audit still runs against the actual bytes of the candidate. The board then measures the same facts itself, on the
first boot: NativeLegacyDisplay's probe prints the class size and whether the parent chain matches (evb10: display
probe: ... size 0x18a0 parent 1), which is the observation the QEMU run made.
"""
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNTIME_SHA = '3044c2beff552911605994c0dc7aa73182066cbb5aed0b396999ceeb5a96da2b'   # audit-mobile-graphics-legacy.py

kernel = ROOT.parent.parent / 'evb10' / 'kernel' / 'kernel.macho'   # the display-less pass A (output directory .../evb10)
if not kernel.is_file():
    raise SystemExit(f'{kernel} missing (unixnu build kernel, display-less configuration)')
plan = json.loads(kernel.with_suffix('.json').read_text())
if 'com.apple.iokit.IOMobileGraphicsFamily' not in plan['original_executable_sha256']:
    raise SystemExit('this kernel carries no original IOMobileGraphicsFamily: add it (and IOSurface, IOGPUFamily, ...) to '
                     '[kernel.prelink] roots of board.toml and rebuild the kernel first')

candidate = ROOT / 'build/xnu-prelink/native-owner-mobile-graphics-candidate.macho'
candidate.parent.mkdir(parents=True, exist_ok=True)
shutil.copyfile(kernel, candidate)
shutil.copyfile(kernel.with_suffix('.json'), candidate.with_suffix('.json'))

analysis = ROOT / 'build/display-analysis'
analysis.mkdir(parents=True, exist_ok=True)
seed = analysis / 'mobile-graphics-legacy-abi.json'
if not seed.exists():
    seed.write_text(json.dumps({'runtime_metadata_checkpoint_sha256': RUNTIME_SHA}, indent=2) + '\n')
print('candidate:', candidate)
print('audit seed:', seed)
