#!/usr/bin/env python3
"""Qualify the owned HID input source against the unchanged original IOHIDDevice and archive it.

build-sheng-hid-input.py of UniXNU-sheng (BSD-2-Clause-Patent, Copyright (c) 2026, UniXNU contributors), ported to this board in
the way scripts/build-native-legacy-display.py ports sheng's display script: no Homebrew or Xcode SDK (the recorded kernel compiler
assembles the bindings and compiles the probe). The archive is build/evb10-hid/libEvb10Hid.a, which the display kernel
(configs/evb10_display_defconfig) force-loads next to the display archive (that one is refused by the kernel link if its bytes
change, so this does not extend it). Every instruction and ABI check of the sheng script is kept: the original IOHIDFamily
executable must be the audited one, the 207-slot abstract IOHIDDevice, its constructor/destructor and start/handleReport call
boundaries must match byte for byte, and NativeHidInput may override only the ten qualified slots.

Run through scripts/build-evb10-display.py, after build-native-legacy-display.py. Its inputs: the candidate that
build-display-abi.py copied (the display-less kernel, whose prelink plan carries the original IOHIDFamily).
"""
import hashlib,json,os,shutil,struct,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/evb10-hid';OUT.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho,backed_offset,exports

def run(cmd,**kw):return subprocess.check_output([str(x) for x in cmd],timeout=120,**kw)
def require(value,why):
    if not value:raise SystemExit(why)
# The reference candidate is the display-less kernel copied by build-display-abi.py. It must be its own plan's candidate and carry
# the same original IOHIDFamily instruction hash as the kernelcache sheng audited; every check below still runs against its bytes.
HID_TEXT_SHA='7e4c5104814f2e773d5be13d092dec53ee44ba5a61a81264b229780406e86b72'
p=ROOT/'build/xnu-prelink/native-owner-mobile-graphics-candidate.macho'
require(p.exists(),'No reference candidate available (build-display-abi.py makes it)')
b=p.read_bytes();plan=json.loads(p.with_suffix('.json').read_text());m=macho(b)
require(hashlib.sha256(b).hexdigest()==plan['candidate_sha256'],'Reference candidate matches its plan')
require(plan['original_executable_sha256'].get('com.apple.iokit.IOHIDFamily')==HID_TEXT_SHA,'Exact original HID reference candidate: add com.apple.iokit.IOHIDFamily to the prelink roots')
def read(a,n):
    at=backed_offset(m['segments'],a,n,len(b));return b[at:at+n]
names={n:a for a,ns in exports(b,m).items() for n in ns}
e=next(e for e in plan['extents'] if e['module']=='com.apple.iokit.IOHIDFamily' and e['segment']=='__TEXT_EXEC')
require(hashlib.sha256(read(e['va'],e['size'])).hexdigest()==plan['original_executable_sha256']['com.apple.iokit.IOHIDFamily'],'Complete original HID instructions unchanged')
vptr=0xfffffff005d045c0;methods=struct.unpack('<207Q',read(vptr,207*8))
base=struct.unpack('<168Q',read(names['__ZTV9IOService']+16,168*8))
changed=[i for i in range(168) if methods[i]!=base[i]]
require(changed==[0,1,7,12,14,21,23,76,81,82,86,87,91,92,93,101,132,140],'Exact original HID native-base overrides')
require([i for i,a in enumerate(methods) if a==names['___cxa_pure_virtual']]==[180] and all(methods) and read(vptr+207*8,24)==bytes(24),'Exact original 207-slot abstract device')
# Original C2 calls the actual native IOService C2 through its existing thunk;
# its own vptr store and D2 tail-call remain unchanged.
require(read(0xfffffff004ad94f8,0x20)==bytes.fromhex('fd7bbfa9fd0300912bd90094489100f008011791080000f9fd7bc1a8c0035fd6'),'Original protected constructor and exact vptr store')
require(struct.unpack('<Q',read(0xfffffff005d00f10,8))[0]==names['__ZN9IOServiceC2EPK11OSMetaClass'],'Actual native protected constructor binding')
require(struct.unpack('<Q',read(0xfffffff005d00f18,8))[0]==names['__ZN9IOServiceD2Ev'],'Actual native protected destructor binding')
# Original start passes (self, provider) to slot 168, then (self, &descriptor)
# to pure slot 180 and checks the real returned status/descriptor.
require(read(0xfffffff004ad9aec,0x34)==bytes.fromhex('680240f908a142f9e00313aae10314aa00013fd6680240f9603d003608d142f9e1630091e00313aa00013fd6a02d0035e00f40f9'),'Original handleStart and report-descriptor ABI')
require(methods[181]==0xfffffff004add0c4 and read(0xfffffff004add110,28)==bytes.fromhex('680240f9081543f9e00313aae20316aae30315aae40314aa00013fd6'),'Original handleReport forwards timestamp, report, type and options')
header='''// Generated from the qualified original image; opaque private storage only.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
class IOHIDDevice : public IOService {
 OSDeclareAbstractStructors(IOHIDDevice)
 unsigned char originalPrivate[0xd0-sizeof(IOService)];
public:
 using IOService::setProperty;
 IOReturn Dispatch(IORPC) override;
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 bool setProperty(const OSSymbol *,OSObject *) override;
 bool willTerminate(IOService *,IOOptionBits) override;
 IOReturn configureReport(IOReportChannelList *,IOReportConfigureAction,void *,void *) override;
 IOReturn updateReport(IOReportChannelList *,IOReportUpdateAction,void *,void *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 bool handleOpen(IOService *,IOOptionBits,void *) override;
 void handleClose(IOService *,IOOptionBits) override;
 bool handleIsOpen(const IOService *) const override;
 bool matchPropertyTable(OSDictionary *,SInt32 *) override;
 IOReturn message(UInt32,IOService *,void *) override;
 IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
'''
for i in range(168,207):
    if i==171:header+=' virtual OSString *newTransportString() const;\n'
    elif i==173:header+=' virtual OSString *newProductString() const;\n'
    elif i==180:header+=' virtual IOReturn newReportDescriptor(IOMemoryDescriptor **) const=0;\n'
    elif i==181:header+=' virtual IOReturn handleReport(IOMemoryDescriptor *,unsigned,IOOptionBits);\n'
    else:header+=f' virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque{i}();\n'
header+='};\nstatic_assert(sizeof(IOHIDDevice)==0xd0,"Original device instance size");\n'
(OUT/'OriginalHidDeviceABI.h').write_text(header)

print(run([sys.executable,ROOT/'scripts/build-evb10-drivers.py','NativeHidInput'],text=True),end='')
obj=OUT/'NativeHidInput.o';raw=obj.read_bytes();om=macho(raw)
st=next(p for c,p,_ in om['commands'] if c==2);sy,count,strings,_=struct.unpack_from('<4I',raw,st+8);symbols=[]
for i in range(count):
    ix,kind,section,_,va=struct.unpack_from('<IBBHQ',raw,sy+i*16)
    symbols.append((raw[strings+ix:raw.index(b'\0',strings+ix)].decode(),va,kind))
vt=next(va for name,va,_ in symbols if name=='__ZTV14NativeHidInput');relocations={}
for segment in om['segments']:
    for sec in segment['sections']:
        if sec['va']<=vt<sec['va']+sec['size']:
            ro,rc=struct.unpack_from('<2I',raw,sec['command_offset']+56)
            for i in range(rc):
                offset,bits=struct.unpack_from('<2I',raw,ro+i*8)
                if bits>>27&1 and bits>>28==0 and bits>>25&3==3:relocations[sec['va']+offset]=symbols[bits&0xffffff][0]
compiled=[relocations.get(vt+16+i*8) for i in range(207)]
require(all(compiled) and vt+16+207*8 not in relocations,'Compiled exact 207-slot vtable')
bindings={'__ZN11IOHIDDeviceC2EPK11OSMetaClass':0xfffffff004ad94f8,'__ZN11IOHIDDeviceD2Ev':0xfffffff004ad9518,'__ZN11IOHIDDevice10gMetaClassE':0xfffffff006ae6fa0,'__ZTV11IOHIDDevice':vptr-16}
owned=[]
for i,name in enumerate(compiled):
    if name.startswith(('__ZN14NativeHidInput','__ZNK14NativeHidInput')):owned.append(i)
    elif '11IOHIDDevice' in name:bindings[name]=methods[i]
    else:require(i<168 and names.get(name)==methods[i],'Actual inherited native method '+str(i))
require(owned==[0,1,7,14,86,87,111,171,173,180],'Only qualified HID methods overridden')
undefined={n for n,_,k in symbols if k==1 and '11IOHIDDevice' in n}
require(undefined==set(bindings),'All and only actual original HID imports bound')
asm=OUT/'original-bindings.S';asm.write_text(''.join(f'.globl {n}\n.set {n}, {a:#x}\n' for n,a in sorted(bindings.items())))
record0=json.loads((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
run([record0['arguments'][0],'--target=arm64-apple-ios17.0','-Werror','-Wno-incompatible-sysroot','-c',asm,'-o',OUT/'hid-original-bindings.o'],cwd=record0['directory'])
def tool(name):
    found=shutil.which(name)
    if found:return Path(found)
    for d in ('/opt/homebrew/opt/llvm/bin','/usr/local/opt/llvm/bin','/usr/bin'):
        if (Path(d)/name).is_file():return Path(d)/name
    raise SystemExit('no '+name+' on PATH')
def section_remover():
    """An llvm-objcopy that can remove a Mach-O section (LLVM 10, the Ubuntu 20.04 one, cannot): $UNIXNU_LLVM_OBJCOPY, then PATH."""
    import os,tempfile
    for candidate in [os.environ.get('UNIXNU_LLVM_OBJCOPY'),shutil.which('llvm-objcopy'),'/opt/homebrew/opt/llvm/bin/llvm-objcopy']:
        if not candidate or not Path(candidate).is_file():continue
        probe=subprocess.run([candidate,'--remove-section=__DATA,__mod_init_func',str(OUT/'NativeHidInput.o'),
                              str(Path(tempfile.gettempdir())/'objcopy-probe.o')],capture_output=True)
        if probe.returncode==0:return candidate
    raise SystemExit('no llvm-objcopy that removes Mach-O sections: set UNIXNU_LLVM_OBJCOPY to one of LLVM 13 or newer')
def globalize(path,name):
    """Make the local symbol NAME of a Mach-O object external, which llvm-objcopy --globalize-symbol does for ELF only.

    The symbol moves to the end of the local symbols, so that LC_DYSYMTAB's local and external ranges stay contiguous and
    sorted as clang writes them; the symbol table indexes in relocations and the indirect symbol table follow the swap.
    Nothing else of the object changes (the caller compares the code section before and after)."""
    data=bytearray(Path(path).read_bytes());info=macho(bytes(data))
    sym=next(p for c,p,_ in info['commands'] if c==0x2);dys=next(p for c,p,_ in info['commands'] if c==0xb)
    symoff,nsyms,stroff,_=struct.unpack_from('<4I',data,sym+8)
    ilocal,nlocal,iext,next_,_,_=struct.unpack_from('<6I',data,dys+8)
    def entry(i):return symoff+16*i
    def called(i):
        ix=struct.unpack_from('<I',data,entry(i))[0];return bytes(data[stroff+ix:data.index(b'\0',stroff+ix)]).decode()
    found=[i for i in range(nsyms) if called(i)==name]
    require(len(found)==1,'symbol '+name+' not found exactly once in '+str(path))
    i=found[0]
    require(data[entry(i)+4]&0xef==0x0e,name+' is not a defined local symbol (N_SECT, not N_EXT)')
    require(ilocal<=i<ilocal+nlocal and iext==ilocal+nlocal,'unexpected LC_DYSYMTAB layout')
    last=ilocal+nlocal-1
    if i!=last:
        a_,b_=bytes(data[entry(i):entry(i)+16]),bytes(data[entry(last):entry(last)+16])
        data[entry(i):entry(i)+16]=b_;data[entry(last):entry(last)+16]=a_
        remap={i:last,last:i}
        for segment in info['segments']:
            for section in segment['sections']:
                reloff,count=struct.unpack_from('<2I',data,section['command_offset']+56)
                for k in range(count):
                    at=reloff+8*k;address,bits=struct.unpack_from('<2I',data,at)
                    require(not address>>31,'scattered relocation')
                    if bits>>27&1 and bits&0xffffff in remap:
                        struct.pack_into('<I',data,at+4,(bits&~0xffffff)|remap[bits&0xffffff])
        indirect,count=struct.unpack_from('<2I',data,dys+56)
        for k in range(count):
            value=struct.unpack_from('<I',data,indirect+4*k)[0]
            if value in remap:struct.pack_into('<I',data,indirect+4*k,remap[value])
    data[entry(last)+4]|=1
    struct.pack_into('<4I',data,dys+8,ilocal,nlocal-1,last,next_+1)
    Path(path).write_bytes(bytes(data))
def late_object(source,target,initializer):
    """The object without its __mod_init_func (the probe runs the initializer between preModLoad and postModLoad) and with the
    initializer external: what UniXNU's llvm-objcopy --remove-section --globalize-symbol pair makes."""
    run([remover,'--remove-section=__DATA,__mod_init_func',source,target]);globalize(target,initializer)
remover=section_remover()
late=OUT/'NativeHidInput.late.o'
late_object(obj,late,'__GLOBAL__sub_I_NativeHidInput.cpp')
def code(data,image):
    s=next(s for g in image['segments'] for s in g['sections'] if s['name']=='__text');return data[s['fileoff']:s['fileoff']+s['size']]
require(code(raw,om)==code(late.read_bytes(),macho(late.read_bytes())),'Owned code unchanged by deferred metaclass registration')
# The probe (starts the device on the IOHIDResource) is compiled like KeyStoreProbe.cpp of the display archive.
source=ROOT/'kits/evb10/Evb10HidProbe.cpp';probe_obj=OUT/'Evb10HidProbe.o';r=record0;cmd=[];args=r['arguments'];i=0
while i<len(args):
    value=args[i]
    if value==r['file']:cmd.append(str(source))
    elif value=='-o':cmd+=['-o',str(probe_obj)];i+=1
    elif value=='-Xclang' and args[i+1].startswith('-W'):i+=1
    elif not value.startswith('-W'):cmd.append(value)
    i+=1
cmd+=['-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unnecessary-virtual-specifier','-Wno-c99-designator','-Wno-reorder-init-list',
      '-Wno-deprecated-enum-enum-conversion','-I',str(ROOT/'.cache/xnu-work/osfmk')]
run(cmd,cwd=r['directory'])
# Its own archive: the kernel link refuses the display archive when its bytes differ from build-native-legacy-display.py's report.
archive=OUT/'libEvb10Hid.a';archive.unlink(missing_ok=True)
run([str(tool('llvm-ar')),'rcs',archive,late,OUT/'hid-original-bindings.o',probe_obj])
(OUT/'abi-audit.json').write_text(json.dumps({'reference_candidate_sha256':plan['candidate_sha256'],'original_text_sha256':plan['original_executable_sha256']['com.apple.iokit.IOHIDFamily'],'original_slots':207,'original_instance_size':208,'owned_slots':owned,'bindings':bindings,'runtime_qualified':False,'source_sha256':hashlib.sha256((ROOT/'kits/evb10/Evb10Hid/NativeHidInput.cpp').read_bytes()).hexdigest(),'archive_sha256':hashlib.sha256(archive.read_bytes()).hexdigest()},indent=2)+'\n')
print('PASS: original HID instruction identity, base imports, call boundaries and exact owned vtable; archive: '+str(archive))
