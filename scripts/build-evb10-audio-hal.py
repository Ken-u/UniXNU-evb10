#!/usr/bin/env python3
"""Build the audio server plug-in (hal/Evb10AudioPlugIn.c) as an iOS driver bundle, signed ad hoc, and pack it as a payload addon.

  build-evb10-audio-hal.py [OUTDIR]          default OUTDIR is out/audio

Writes
  OUTDIR/Evb10Audio.driver/{Evb10Audio, Info.plist}   the bundle the audio server loads from /System/Library/Audio/Plug-Ins/HAL
  OUTDIR/audio.addon                                  the same two files as an NGPUADD2 addon (audio/Evb10Audio.driver/...), which
                                                      uefi-evb10's tools/board/make-payload-boot.sh merges into the GPU addon

The executable's CDHash has to be in the owner trust cache: MaliMetal's usertest/build.sh picks OUTDIR/Evb10Audio.driver/Evb10Audio up when it
exists (build this first, then that), and pins the new bundle in board.toml.

Toolchain: LLVM_BIN (a directory with clang-20 and lld) and IOS_SDK (an iPhoneOS SDK), from the environment or from ../MaliMetal/.env.
The iOS SDK has no audio server plug-in headers: AudioServerPlugIn.h, AudioHardwareBase.h and AudioHardware.h come from the macOS 11.3 SDK
(Apple's headers, not redistributed here): they are fetched once into .cache/coreaudio-headers and checked against the sha256 below."""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import urllib.request
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADERS_URL = ('https://raw.githubusercontent.com/phracker/MacOSX-SDKs/master/MacOSX11.3.sdk/System/Library/Frameworks/'
               'CoreAudio.framework/Versions/A/Headers/')
HEADERS = {
    'AudioServerPlugIn.h': 'd17c4c50a5a68e2deb3947ece9980a26bee7e5417ebbf763bac526625786fa7c',
    'AudioHardwareBase.h': '103252d09fd8b87672e8049d6cc700eca725ddf6ca667c3c2270ddd6d41a3af3',
    'AudioHardware.h': 'f40e70e5417d4ff48dea2b1947a251938fa3151f05c8b26f449ec6f5f348020f',
}
BUNDLE = 'Evb10Audio.driver'
EXECUTABLE = 'Evb10Audio'


def toolchain():
    values = {key: os.environ.get(key) for key in ('LLVM_BIN', 'IOS_SDK')}
    env_file = ROOT.parent / 'MaliMetal' / '.env'
    if (not all(values.values())) and env_file.is_file():
        for line in env_file.read_text().splitlines():
            line = line.strip()
            if line.startswith('export '):
                line = line[7:]
            key, _, value = line.partition('=')
            if key in values and not values[key]:
                values[key] = value.strip().strip('"\'')
    missing = [key for key, value in values.items() if not value]
    if missing:
        sys.exit('set %s (environment or ../MaliMetal/.env)' % ' and '.join(missing))
    return values['LLVM_BIN'], values['IOS_SDK']


def fetch_headers():
    directory = ROOT / '.cache' / 'coreaudio-headers' / 'CoreAudio'
    directory.mkdir(parents=True, exist_ok=True)
    for name, digest in HEADERS.items():
        path = directory / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            data = urllib.request.urlopen(HEADERS_URL + name, timeout=60).read()
            if hashlib.sha256(data).hexdigest() != digest:
                sys.exit('%s: the download does not match the pinned sha256 %s' % (name, digest))
            path.write_bytes(data)
    return directory.parent


def pad(data):
    return data + bytes(-len(data) % 8)


def addon(files):
    """files: (path inside the addon, mode, bytes). The NGPUADD2 layout of UniXNU's scripts/make-gpu-addon.py (little endian): "NGPUADD2",
    u32 count, u32 0, u64 total bytes; per file u32 path bytes (with NUL, at most 160), u32 mode, u64 size, u64 compressed size, the path
    padded to 8 bytes, the zlib stream padded to 8 bytes."""
    body = b''
    for name, mode, data in files:
        encoded = name.encode() + b'\0'
        if len(encoded) > 160 or '..' in name.split('/') or name.startswith('/'):
            sys.exit('bad addon path ' + name)
        packed = zlib.compress(data, 9)
        body += struct.pack('<IIQQ', len(encoded), mode, len(data), len(packed)) + pad(encoded) + pad(packed)
    return b'NGPUADD2' + struct.pack('<IIQ', len(files), 0, 24 + len(body)) + body


def main():
    if len(sys.argv) > 2 or (len(sys.argv) == 2 and sys.argv[1] in ('-h', '--help')):
        sys.exit(__doc__)
    out = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else ROOT / 'out' / 'audio'
    llvm, sdk = toolchain()
    headers = fetch_headers()
    bundle = out / BUNDLE
    shutil.rmtree(bundle, ignore_errors=True)
    bundle.mkdir(parents=True)
    # the kernel build puts CCC_OVERRIDE_OPTIONS and friends into a persistent shell's environment; they change every clang call
    env = {key: value for key, value in os.environ.items() if key not in ('CCC_OVERRIDE_OPTIONS', 'LLVM_CONFIG', 'CLANG_BIN', 'CC', 'CXX')}
    command = [str(Path(llvm) / 'clang-20'), '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-I', str(headers),
               '-I', str(ROOT / 'package' / 'Evb10Audio'), '-O2', '-g0', '-Wall', '-Wextra', '-Werror', '-bundle', '-fvisibility=hidden',
               '-fuse-ld=lld', '-B' + llvm, '-Wl,-adhoc_codesign', '-framework', 'CoreFoundation', '-framework', 'IOKit',
               str(ROOT / 'hal' / 'Evb10AudioPlugIn.c'), '-o', str(bundle / EXECUTABLE)]
    result = subprocess.run(command, env=env, stderr=subprocess.PIPE, universal_newlines=True)
    sys.stderr.write(''.join(line + '\n' for line in result.stderr.splitlines() if 'directory not found' not in line))
    if result.returncode:
        sys.exit('the plug-in did not build')
    shutil.copy(ROOT / 'hal' / 'Info.plist', bundle / 'Info.plist')
    files = [('audio/%s/%s' % (BUNDLE, EXECUTABLE), 0o755, (bundle / EXECUTABLE).read_bytes()),
             ('audio/%s/Info.plist' % BUNDLE, 0o644, (bundle / 'Info.plist').read_bytes())]
    blob = addon(files)
    (out / 'audio.addon').write_bytes(blob)
    for name, mode, data in files:
        print('%o %8d %s sha256 %s' % (mode, len(data), name, hashlib.sha256(data).hexdigest()[:16]))
    print('%s: %d bytes' % (out / 'audio.addon', len(blob)))


if __name__ == '__main__':
    main()
