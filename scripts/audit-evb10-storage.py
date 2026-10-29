#!/usr/bin/env python3
"""Record the original IOBlockStorageDevice ABI of the pinned kernelcache, from the kernel image alone.

UniXNU's scripts/audit-storage-interface.py qualifies the same ABI, but two of its checks need
evidence that only a guest with a debugger attached can produce: a memory snapshot of the live
metaclass objects and the live translation registers. This board has no such guest yet, so this
script keeps every check that can be made on the files and replaces the other two:

  * the class sizes (IOBlockStorageDevice 0x90, IOService 0x88) and the metaclass link are checked
    by the driver itself: a static_assert on the compiled public classes, and a runtime guard that
    reads the original IOBlockStorageDevice metaclass before the first block device is created;
  * the 4 KiB translation granule is a property of every UniXNU kernel, not of a run.

What it checks, all of it on kernel.macho / kernel.json from `unixnu build kernel`:

  1. the public IOStorageFamily source is the pinned Darwin 23 revision, and its
     IOBlockStorageDevice compiles with the kernel's own recorded flags into an object whose
     vtable has the 190 slots the driver will be bound against (14 pure virtual);
  2. the kernel image is the pinned original kernelcache, and the original IOStorageFamily
     executable code in it is unchanged;
  3. exactly one block of the original __DATA_CONST has that vtable (176 slots bind to kernel
     exports, the other 14 point into the original IOStorageFamily text);
  4. the original protected constructor, base destructor and metaclass getter are what the
     public source compiles to (the abstract D1/D0 are traps, so a subclass may only use D2).

Output: scripts/data/evb10-storage-interface-audit.json, which scripts/build-evb10-drivers.py reads
when it binds NativeEmmcBlock. Run it again only when the kernelcache pin changes:

  python3 scripts/audit-evb10-storage.py [--kernel out/evb10/kernel/kernel.macho] [--output FILE]

The audit logic follows UniXNU's audit-storage-interface.py, BSD-2-Clause-Patent,
Copyright (c) 2026, UniXNU contributors.
"""
import argparse
import hashlib
import json
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
import native_prelink as np  # noqa: E402

REVISION = 'e45fbe30a7879e0b16e26dddb78538f1b00e0bff'
ORIGINAL_SHA256 = 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'
RECORD = 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
MODULE = 'com.apple.iokit.IOStorageFamily'
# The sizes the live guest reported for the original classes; the driver asserts the same numbers.
CLASS_SIZES = {'IOBlockStorageDevice': 0x90, 'IOService': 0x88}


def require(value, message):
    if not value:
        raise SystemExit('audit: ' + message)


def reference_object(out):
    """Compile the public IOBlockStorageDevice.cpp with the kernel's recorded command."""
    source = ROOT / 'refer/IOStorageFamily'
    require((source / '.git').is_dir(), '%s is missing; run "unixnu fetch IOStorageFamily"' % source)
    revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    require(revision == REVISION, 'unexpected public IOStorageFamily revision ' + revision)
    require((ROOT / 'build/storage-abi/include/IOKit/storage/IOBlockStorageDevice.h').is_file(),
            'public storage headers are not prepared; run scripts/prepare-storage-headers.py')
    record = json.loads((ROOT / RECORD).read_text().strip().rstrip(','))
    destination = out / 'IOBlockStorageDevice-reference.o'
    command, index, arguments = [], 0, record['arguments']
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source / 'IOBlockStorageDevice.cpp'))
        elif value == '-o':
            command += ['-o', str(destination)]
            index += 1
        elif value in ('-MF', '-MJ'):
            index += 1
        elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W') and value not in ('-MD', '-MP'):
            command.append(value)
        index += 1
    command += ['-I', str(ROOT / 'build/storage-abi/include'), '-I', str(ROOT / '.cache/xnu-work/osfmk'),
                '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-deprecated-declarations',
                '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-unnecessary-virtual-specifier',
                '-Wno-deprecated-enum-enum-conversion']
    (out / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
    result = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
    (out / 'compile.log').write_text(result.stdout + result.stderr)
    require(not result.returncode, 'the public source does not compile:\n' + (result.stdout + result.stderr)[-3000:])
    return source, revision, destination


def reference_methods(path):
    """The 190 virtual slots of the public class, by mangled name."""
    obj = path.read_bytes()
    om = np.macho(obj)
    require(om['kind'] == 1, 'expected an ordinary ARM64 reference object')
    version = next(at for command, at, size in om['commands'] if command == 0x32)
    require(struct.unpack_from('<II', obj, version + 8) == (2, 0x110000), 'expected the iOS 17 deployment target')
    sycmd = next(at for command, at, size in om['commands'] if command == 2)
    symoff, count, stroff, _ = struct.unpack_from('<4I', obj, sycmd + 8)
    symbols = []
    for i in range(count):
        ix, kind, _, _, value = struct.unpack_from('<IBBHQ', obj, symoff + i * 16)
        symbols.append(dict(name=obj[stroff + ix:obj.index(b'\0', stroff + ix)].decode(), value=value, kind=kind))
    vtable = next(s['value'] for s in symbols if s['name'] == '__ZTV20IOBlockStorageDevice')
    end = min(s['value'] for s in symbols if not s['kind'] & 0xe0 and s['kind'] & 0xe == 0xe and s['value'] > vtable)
    relocations = {}
    for segment in om['segments']:
        for section in segment['sections']:
            if section['va'] <= vtable < section['va'] + section['size']:
                ro, rc = struct.unpack_from('<II', obj, section['command_offset'] + 56)
                for i in range(rc):
                    address, word = struct.unpack_from('<II', obj, ro + i * 8)
                    if word >> 27 & 1 and word >> 28 == 0 and word >> 25 & 3 == 3:
                        relocations[section['va'] + address] = symbols[word & 0xffffff]['name']
    methods = []
    for at in range(vtable + 16, end, 8):
        if at not in relocations:
            break
        methods.append(relocations[at])
    require(len(methods) == 190 and methods.count('___cxa_pure_virtual') == 14, 'public iOS class dispatch shape changed')
    return obj, methods


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--kernel', type=Path, default=ROOT.parent / 'kernel/kernel.macho',
                        help='candidate kernel image; kernel.json must sit next to it')
    parser.add_argument('--output', type=Path, default=ROOT / 'scripts/data/evb10-storage-interface-audit.json')
    args = parser.parse_args()

    out = ROOT / 'build/storage-abi'
    out.mkdir(parents=True, exist_ok=True)
    source, revision, reference = reference_object(out)
    obj, methods = reference_methods(reference)

    kernel_path = args.kernel.resolve()
    kernel = kernel_path.read_bytes()
    plan = json.loads(kernel_path.with_suffix('.json').read_text())
    require(hashlib.sha256(kernel).hexdigest() == plan['candidate_sha256'], 'kernel.json does not describe this kernel image')
    require(plan['original_sha256'] == ORIGINAL_SHA256, 'the kernel is not built from the pinned original kernelcache')
    native = np.macho(kernel)
    exports = np.exports(kernel, native)
    by_name = {name: va for va, names in exports.items() for name in names}
    extents = [e for e in plan['extents'] if e['module'] == MODULE]
    const = next(e for e in extents if e['segment'] == '__DATA_CONST')
    text = next(e for e in extents if e['segment'] == '__TEXT_EXEC')

    def read(va, length):
        at = np.backed_offset(native['segments'], va, length, len(kernel))
        return kernel[at:at + length]

    text_sha256 = hashlib.sha256(read(text['va'], text['size'])).hexdigest()
    require(text_sha256 == plan['original_executable_sha256'][MODULE], 'original storage executable code changed')
    blob = read(const['va'], const['size'])
    found = []
    for at in range(0, len(blob) - (len(methods) + 2) * 8 + 1, 8):
        if blob[at:at + 16] != bytes(16):
            continue
        pointers = struct.unpack_from('<' + 'Q' * len(methods), blob, at + 16)
        if all(pointers[i] == by_name[name] for i, name in enumerate(methods) if name in by_name):
            found.append((const['va'] + at, pointers))
    require(len(found) == 1, 'cannot uniquely identify the original block-storage class dispatch')
    table, pointers = found[0]
    matched = sum(name in by_name for name in methods)
    require(matched == 176, 'unexpected original/native kernel binding count')
    for i, name in enumerate(methods):
        if name not in by_name:
            require(text['va'] <= pointers[i] < text['va'] + text['size'], 'owned storage method outside original text')

    def relative_branch(address, instruction):
        immediate = instruction & 0x3ffffff
        if immediate & 0x2000000:
            immediate -= 0x4000000
        return address + immediate * 4

    def thunk_names(address):
        a, load, branch = struct.unpack('<3I', read(address, 12))
        require(a & 0x9f00001f == 0x90000010 and load & 0xffc003ff == 0xf9400210 and branch == 0xd61f0200,
                'unqualified original native import thunk')
        immediate = ((a >> 5 & 0x7ffff) << 2) | (a >> 29 & 3)
        if immediate & 0x100000:
            immediate -= 0x200000
        slot = (address & ~4095) + immediate * 4096 + (load >> 10 & 4095) * 8
        target = struct.unpack('<Q', read(slot, 8))[0]
        return exports.get(target, [])

    constructors = []
    for address in range(text['va'], text['va'] + text['size'] - 32, 4):
        words = struct.unpack('<8I', read(address, 32))
        if (words[:2] != (0xa9bf7bfd, 0x910003fd) or words[2] & 0xfc000000 != 0x94000000 or
                words[3] & 0x9f00001f != 0x90000008 or words[4] & 0xffc003ff != 0x91000108 or
                words[5:] != (0xf9000008, 0xa8c17bfd, 0xd65f03c0)):
            continue
        immediate = ((words[3] >> 5 & 0x7ffff) << 2) | (words[3] >> 29 & 3)
        if immediate & 0x100000:
            immediate -= 0x200000
        stored = ((address + 12) & ~4095) + immediate * 4096 + (words[4] >> 10 & 4095)
        if stored == table + 16:
            constructors.append((address, relative_branch(address + 8, words[2])))
    require(len(constructors) == 1 and '__ZN9IOServiceC2EPK11OSMetaClass' in thunk_names(constructors[0][1]),
            'original protected block constructor does not forward to the actual IOService')
    destructor = pointers[0] - 4
    word = struct.unpack('<I', read(destructor, 4))[0]
    require(word & 0xfc000000 == 0x14000000 and '__ZN9IOServiceD2Ev' in thunk_names(relative_branch(destructor, word)),
            'unqualified actual base destructor')
    require(all(read(pointers[i], 4) == struct.pack('<I', 0xd4200020) for i in (0, 1)),
            'unexpected original abstract complete-destructor behavior')
    function = pointers[7]
    a, add, ret = struct.unpack('<3I', read(function, 12))
    require(a & 0x9f00001f == 0x90000000 and add & 0xffc003ff == 0x91000000 and ret == 0xd65f03c0,
            'unqualified original storage metaclass getter')
    immediate = ((a >> 5 & 0x7ffff) << 2) | (a >> 29 & 3)
    if immediate & 0x100000:
        immediate -= 0x200000
    meta = (function & ~4095) + immediate * 4096 + (add >> 10 & 4095)
    service_meta = by_name['__ZN9IOService10gMetaClassE']

    result = dict(
        schema='evb10-storage-interface-audit/1',
        evidence='static: kernel image and public source only; class sizes are asserted by the driver at compile time '
                 'and by its runtime guard before the first device is created',
        public_source_revision=revision,
        source_sha256=hashlib.sha256((source / 'IOBlockStorageDevice.cpp').read_bytes()).hexdigest(),
        reference_object_sha256=hashlib.sha256(obj).hexdigest(), reference_object_executed_or_linked=False,
        original_sha256=plan['original_sha256'],
        audited_candidate_sha256=plan['candidate_sha256'],
        storage_text_sha256=text_sha256,
        storage_extents={e['segment']: dict(va=hex(e['va']), size=hex(e['size'])) for e in extents},
        expected_class_sizes={name: hex(size) for name, size in CLASS_SIZES.items()},
        method_slots=len(methods), kernel_bindings=matched, pure_virtual_slots=methods.count('___cxa_pure_virtual'),
        inherited_kernel_methods=matched - methods.count('___cxa_pure_virtual'),
        vtable=hex(table), original_metaclass=hex(meta), source_ioservice_metaclass=hex(service_meta),
        original_protected_constructor=hex(constructors[0][0]), original_base_destructor=hex(destructor),
        abstract_complete_destructors_trap=True,
        methods=[dict(slot=i, reference_name=name, candidate_target=hex(pointers[i]), kernel_names=exports.get(pointers[i], []))
                 for i, name in enumerate(methods)],
        disk_read_tested=False, transport_implemented=False, hardware_tested=False)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print('PASS: %d ordinary iOS storage slots; %d kernel bindings (%d inherited + %d pure virtual)' % (
        len(methods), matched, result['inherited_kernel_methods'], result['pure_virtual_slots']))
    print('original vtable %s, metaclass %s, constructor %s -> %s' % (
        hex(table), hex(meta), hex(constructors[0][0]), args.output))


if __name__ == '__main__':
    main()
