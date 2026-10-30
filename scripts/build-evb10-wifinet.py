#!/usr/bin/env python3
"""Kernel-step archive entry: the network side of the Wi-Fi module (package/Evb10Wifi/WifiNet.cpp) as a late-registered archive.

WifiNet.cpp makes the Ethernet interface `en0` and the IOKit classes by which the system's network configuration and its wireless
daemon find a wireless interface (IO80211Controller, IO80211Interface, IONetworkStack and their user client). They are registered late,
as the display's and the HID device's classes are: the object's __mod_init_func section is removed and its static initializer made
external, and package/Evb10Wifi/Evb10Wifi.cpp runs it between OSMetaClass::preModLoad and postModLoad once the module has a station
interface and a hardware address. build/evb10-wifinet/libEvb10WifiNet.a is force-loaded by configs/evb10_display_defconfig.

The globalize step is that of scripts/build-evb10-hid.py (llvm-objcopy's --globalize-symbol does ELF only).
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/evb10-wifinet'
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import macho  # noqa: E402

INITIALIZER = '__GLOBAL__sub_I_WifiNet.cpp'


def require(value, why):
    if not value:
        raise SystemExit(why)


def run(cmd, **kw):
    return subprocess.check_output([str(x) for x in cmd], timeout=300, **kw)


def tool(name):
    found = shutil.which(name)
    if found:
        return Path(found)
    for d in ('/opt/homebrew/opt/llvm/bin', '/usr/local/opt/llvm/bin', '/usr/bin'):
        if (Path(d) / name).is_file():
            return Path(d) / name
    raise SystemExit('no ' + name + ' on PATH')


def section_remover(probe_object):
    """An llvm-objcopy that can remove a Mach-O section (LLVM 10, the Ubuntu 20.04 one, cannot): $UNIXNU_LLVM_OBJCOPY, then PATH."""
    for candidate in [os.environ.get('UNIXNU_LLVM_OBJCOPY'), shutil.which('llvm-objcopy'), '/opt/homebrew/opt/llvm/bin/llvm-objcopy']:
        if not candidate or not Path(candidate).is_file():
            continue
        probe = subprocess.run([candidate, '--remove-section=__DATA,__mod_init_func', str(probe_object),
                                str(Path(tempfile.gettempdir()) / 'objcopy-probe-wifinet.o')], capture_output=True)
        if probe.returncode == 0:
            return candidate
    raise SystemExit('no llvm-objcopy that removes Mach-O sections: set UNIXNU_LLVM_OBJCOPY to one of LLVM 13 or newer')


def globalize(path, name):
    """Make the local symbol NAME of a Mach-O object external (see scripts/build-evb10-hid.py for the details of the symbol table swap)."""
    data = bytearray(Path(path).read_bytes())
    info = macho(bytes(data))
    sym = next(p for c, p, _ in info['commands'] if c == 0x2)
    dys = next(p for c, p, _ in info['commands'] if c == 0xb)
    symoff, nsyms, stroff, _ = struct.unpack_from('<4I', data, sym + 8)
    ilocal, nlocal, iext, next_, _, _ = struct.unpack_from('<6I', data, dys + 8)

    def entry(i):
        return symoff + 16 * i

    def called(i):
        ix = struct.unpack_from('<I', data, entry(i))[0]
        return bytes(data[stroff + ix:data.index(b'\0', stroff + ix)]).decode()

    found = [i for i in range(nsyms) if called(i) == name]
    require(len(found) == 1, 'symbol ' + name + ' not found exactly once in ' + str(path))
    i = found[0]
    require(data[entry(i) + 4] & 0xef == 0x0e, name + ' is not a defined local symbol (N_SECT, not N_EXT)')
    require(ilocal <= i < ilocal + nlocal and iext == ilocal + nlocal, 'unexpected LC_DYSYMTAB layout')
    last = ilocal + nlocal - 1
    if i != last:
        a_, b_ = bytes(data[entry(i):entry(i) + 16]), bytes(data[entry(last):entry(last) + 16])
        data[entry(i):entry(i) + 16] = b_
        data[entry(last):entry(last) + 16] = a_
        remap = {i: last, last: i}
        for segment in info['segments']:
            for section in segment['sections']:
                reloff, count = struct.unpack_from('<2I', data, section['command_offset'] + 56)
                for k in range(count):
                    at = reloff + 8 * k
                    address, bits = struct.unpack_from('<2I', data, at)
                    require(not address >> 31, 'scattered relocation')
                    if bits >> 27 & 1 and bits & 0xffffff in remap:
                        struct.pack_into('<I', data, at + 4, (bits & ~0xffffff) | remap[bits & 0xffffff])
        indirect, count = struct.unpack_from('<2I', data, dys + 56)
        for k in range(count):
            value = struct.unpack_from('<I', data, indirect + 4 * k)[0]
            if value in remap:
                struct.pack_into('<I', data, indirect + 4 * k, remap[value])
    data[entry(last) + 4] |= 1
    struct.pack_into('<4I', data, dys + 8, ilocal, nlocal - 1, last, next_ + 1)
    Path(path).write_bytes(bytes(data))


def code(data):
    image = macho(data)
    section = next(s for g in image['segments'] for s in g['sections'] if s['name'] == '__text')
    return data[section['fileoff']:section['fileoff'] + section['size']]


def main():
    print(run([sys.executable, ROOT / 'scripts/build-evb10-drivers.py', 'Evb10WifiNet'], text=True), end='')
    obj = OUT / 'WifiNet.o'
    late = OUT / 'WifiNet.late.o'
    run([section_remover(obj), '--remove-section=__DATA,__mod_init_func', obj, late])
    globalize(late, INITIALIZER)
    require(code(obj.read_bytes()) == code(late.read_bytes()), 'Owned code unchanged by deferred metaclass registration')
    archive = OUT / 'libEvb10WifiNet.a'
    archive.unlink(missing_ok=True)
    run([tool('llvm-ar'), 'rcs', archive, late])
    print('Evb10WifiNet: late-registered archive -> %s' % archive)


main()
