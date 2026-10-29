#!/usr/bin/env python3
"""Compile the board drivers of UniXNU-evb10 against the actual source-kernel ABI.

Components of the UniXNU catalog (ArmDTPlatform, ArmCpu) are built by `component:<ID>` entries of
board.toml. This script builds what the catalog does not carry:

    build/rk3588-emmc/libRk3588Emmc.a      package/Rk3588Emmc  (submodule; bind-emmc-storage.py makes the archive)
    build/mali/libMali.a                   package/Mali        (submodule)
    build/evb10-touch/libEvb10Touch.a      package/Evb10Touch
    build/evb10-hid/                       package/Evb10Hid    (object; build-evb10-hid.py)
    build/native-legacy-display/           package/Evb10Display (object; build-native-legacy-display.py)

An XNU driver has to be compiled with the same flags, the same SDK and the same deployment target as
the kernel object it lives next to, or the Mach-O it produces will not link or will not run. The kernel
build records the exact command for every object it compiles in a `.cpo.json` file, so this script reads
that record, points it at the driver source, and runs the compiler. The board's package/ appears as kits/evb10 in the build
tree; catalog drivers are under drivers/.

  python3 scripts/build-evb10-drivers.py Rk3588Emmc | Mali | Evb10Touch
  python3 scripts/build-evb10-drivers.py NativeLegacyDisplay | NativeGOPPresenter | NativeKeyStoreEvents | NativeHidInput

Derived from UniXNU-sheng's scripts/build-arm-driver.py, BSD-2-Clause-Patent,
Copyright (c) 2026, UniXNU contributors.
"""
import argparse
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# component -> (sources relative to the build tree, output directory under build/, extra include directories under the
# tree root, whether scripts/bind-emmc-storage.py makes the archive). Headers next to a source are found
# through the quote include of that source; they are not listed here.
COMPONENTS = {
    # The RK3588 eMMC host. It subclasses the original IOBlockStorageDevice, so it compiles against the public IOStorageFamily
    # headers the kernel step prepares in build/storage-abi/include, and its archive is made by scripts/bind-emmc-storage.py,
    # which adds the absolute symbols for the original code. This script only leaves the object (and removes a stale archive),
    # so the kernel link cannot pick up a driver that was never bound.
    'Rk3588Emmc': (('kits/evb10/Rk3588Emmc/NativeEmmc.cpp',), 'rk3588-emmc', ('build/storage-abi/include',), True),
    # The touch panel of the DSI display (I2C5, polled). A plain archive; the display probe (package/LegacyDisplayProbe.cpp)
    # starts it with evb10-touch=1. It publishes the finger set NativeHidInput consumes.
    'Evb10Touch': (('kits/evb10/Evb10Touch/Evb10Touch.cpp',), 'evb10-touch', (), False),
    # The Mali-G610: power, firmware, MMU, command streams and the Metal-facing service; started by the display probe
    # with mali=<milestone>.
    'Mali': (('kits/evb10/Mali/Mali.cpp',), 'mali', (), False),
    # The display archive (scripts/build-native-legacy-display.py, run through scripts/build-evb10-display.py). These
    # leave an object only ('object'): that script generates the original-class header NativeLegacyDisplay includes (hence
    # the include of its own output directory), binds the objects to the original IOMobileFramebufferLegacy and makes the
    # archive. drivers/GopFramebuffer and drivers/AppleKeyStoreState are the UniXNU components of those names.
    # NativeLegacyDisplay is this repository's fork (package/Evb10Display): the original's direct-scanout path drove the
    # SM8550 MDP; the fork drives VOP2 through the VOP's IOMMU (boot argument evb10-vop-direct=1).
    'NativeLegacyDisplay': (('kits/evb10/Evb10Display/NativeLegacyDisplay.cpp',), 'native-legacy-display',
                            ('build/native-legacy-display', 'drivers/GopFramebuffer'), 'object'),
    'NativeGOPPresenter': (('drivers/GopFramebuffer/NativeGOPPresenter.cpp',), 'native-gop-presenter', (), 'object'),
    # The owned HID input device (package/Evb10Hid, from ShengHidInput): an object only; scripts/build-evb10-hid.py audits it
    # against the original IOHIDDevice (it generates the header NativeHidInput includes), binds it and adds it to the display
    # archive.
    'NativeHidInput': (('kits/evb10/Evb10Hid/NativeHidInput.cpp',), 'evb10-hid', ('build/evb10-hid',), 'object'),
    'NativeKeyStoreEvents': (('drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp',), 'apple-key-store-state', (), 'object'),
}

# The record of a kernel object compiled for the same machine configuration and
# SDK. Only its arguments matter: the source and output paths are replaced.
RECORD = 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'

# MH_MAGIC_64, CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64_ALL, MH_OBJECT.
MACHO = (0xfeedfacf, 0x100000c, 0, 1)
# LC_BUILD_VERSION with PLATFORM_IOS and the iOS 17.0 deployment target, which
# is what build-xnu-4k-baseline.sh passes (-miphoneos-version-min=17.0).
LC_BUILD_VERSION = 0x32
DEPLOYMENT = (2, 0x110000)


# The display drivers compare the console with their device memory; they get the console with the kernel's own frame buffer
# (package/Evb10Console.h, which kits/evb10/ carries in the build tree) force-included.
FORCE_INCLUDE = {
    'NativeLegacyDisplay': 'kits/evb10/Evb10Console.h',
    'NativeGOPPresenter': 'kits/evb10/Evb10Console.h',
}


def compiler_command(record, source, destination, includes=(), force_include=None):
    """The recorded command with this source and output, minus warnings XNU sets
    that would make the driver a hard error under -Werror."""
    command, index, arguments = [], 0, record['arguments']
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
        elif value == '-o':
            command.extend(['-o', str(destination)])
            index += 1
        elif value == '-Xclang' and index + 1 < len(arguments) and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    command.extend([
        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
        '-Wno-c99-designator', '-Wno-reorder-init-list',
        '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier',
        # The driver includes XNU's private headers from the patched work clone.
        '-I', str(ROOT / '.cache/xnu-work/osfmk'),
        # warnings in the XNU headers themselves are not this driver's problem
        '-Wno-error=unknown-warning-option',
    ])
    for include in includes:
        command.extend(['-I', str(ROOT / include)])
    if force_include:
        command.extend(['-include', str(ROOT / force_include)])
    return command


def check_object(path):
    """Refuse an object the kernel would not accept: wrong architecture, wrong
    file type, or built for a different platform than the kernel."""
    data = path.read_bytes()
    magic, cpu, subtype, kind, count, _ = struct.unpack_from('<6I', data, 0)
    if (magic, cpu, subtype, kind) != MACHO:
        raise SystemExit('%s is not an ARM64 Mach-O object (magic %#x, cputype %#x, filetype %d)'
                         % (path, magic, cpu, kind))
    offset, deployment = 32, None
    for _ in range(count):
        command, length = struct.unpack_from('<II', data, offset)
        if command == LC_BUILD_VERSION:
            deployment = struct.unpack_from('<II', data, offset + 8)
        offset += length
    if deployment != DEPLOYMENT:
        raise SystemExit('%s is built for platform/deployment %s; the kernel is iOS 17.0 (%s)'
                         % (path, deployment, DEPLOYMENT))


def ar_tool():
    """`ar` that understands Mach-O: llvm-ar, or the Xcode one on a Mac."""
    for name in ('llvm-ar', 'ar'):
        found = shutil.which(name)
        if found:
            return found
    for candidate in ('/opt/homebrew/opt/llvm/bin/llvm-ar', '/usr/local/opt/llvm/bin/llvm-ar',
                      '/usr/bin/llvm-ar'):
        if Path(candidate).is_file():
            return candidate
    raise SystemExit('no llvm-ar on PATH; a Mach-O archive needs one')


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('component', choices=sorted(COMPONENTS))
    args = parser.parse_args()

    relatives, directory, includes, bound = COMPONENTS[args.component]
    record_path = ROOT / RECORD
    if not record_path.is_file():
        raise SystemExit('%s is missing: the kernel step writes it on its first (bootstrap) pass, '
                         'so run "unixnu build kernel" before this builder' % record_path)
    # The kernel build writes the record with a trailing comma, as a fragment.
    record = json.loads(record_path.read_text().strip().rstrip(','))

    out = ROOT / 'build' / directory
    out.mkdir(parents=True, exist_ok=True)
    objects, commands, logs = [], [], []
    for relative in relatives:
        source = ROOT / relative
        if not source.is_file():
            raise SystemExit('%s is missing; board sources are package/ of the device repository (kits/evb10 in the '
                             'tree), the others are UniXNU components under drivers/' % source)
        destination = out / (Path(relative).stem + '.o')
        destination.unlink(missing_ok=True)
        command = compiler_command(record, source, destination, includes, FORCE_INCLUDE.get(args.component))
        commands.append(command)
        result = subprocess.run(command, cwd=record['directory'],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        logs.append(result.stdout)
        if result.returncode:
            sys.stderr.write(result.stdout[-4000:] + '\n')
            raise SystemExit('%s: the compiler rejected %s' % (args.component, relative))
        if not destination.is_file():
            raise SystemExit('%s: the compiler produced no object for %s' % (args.component, relative))
        check_object(destination)
        objects.append(destination)
    (out / 'compile-command.json').write_text(json.dumps(commands[0] if len(commands) == 1 else commands, indent=2) + '\n')
    (out / 'compile.log').write_text(''.join(logs))

    archive = out / ('lib' + args.component + '.a')
    archive.unlink(missing_ok=True)
    if bound == 'object':
        print('%s: -Werror build and ARM64/iOS 17 object audit passed -> %s (object only; the archive comes from '
              'scripts/build-native-legacy-display.py)' % (args.component, objects[0]))
        return
    if bound:
        print('%s: -Werror build and ARM64/iOS 17 object audit passed; the archive comes from '
              'scripts/bind-emmc-storage.py' % args.component)
        return
    subprocess.run([ar_tool(), 'rcs', str(archive)] + [str(path) for path in objects], check=True)
    print('%s: -Werror build and ARM64/iOS 17 object audit passed -> %s'
          % (args.component, archive))


if __name__ == '__main__':
    main()
