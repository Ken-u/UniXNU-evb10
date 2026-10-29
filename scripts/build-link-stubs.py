#!/usr/bin/env python3
"""Archive the no-op hooks of kits/evb10/LinkStubs.c for the kernel link (CONFIG_XNU_EXTRA_LD_ARCHIVES).

Derived from build-link-stubs.py of the community UniXNU-qemu repository, BSD-2-Clause-Patent,
Copyright (c) 2026, UniXNU contributors.
"""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
source = ROOT / 'kits/evb10/LinkStubs.c'
out = ROOT / 'build/link-stubs'
out.mkdir(parents=True, exist_ok=True)
obj, archive = out / 'LinkStubs.o', out / 'libLinkStubs.a'
subprocess.run(['xcrun', '--sdk', 'iphoneos', 'clang', '-arch', 'arm64', '-mkernel', '-miphoneos-version-min=17.0',
                '-Os', '-Wall', '-Werror', '-c', str(source), '-o', str(obj)], check=True)
archive.unlink(missing_ok=True)
ar = shutil.which('llvm-ar') or shutil.which('ar')
subprocess.run([ar, 'rcs', str(archive), str(obj)], check=True)
print(archive)
