#!/usr/bin/env python3
"""Bind an owned concrete backend to the unchanged original Legacy lifecycle.

build-gop-legacy-display.py of UniXNU-sheng (BSD-2-Clause-Patent, Copyright (c) 2026, UniXNU contributors), reduced for
the RK3588 EVB10 in the way the community UniXNU-qemu repository reduced sheng's script, and ported to a Linux build host
(no Homebrew, no Xcode SDK): NativeLegacyDisplay, this board's LegacyDisplayProbe.cpp, NativeGOPPresenter, KeyStoreProbe.cpp
and NativeKeyStoreEvents. sheng's HID input, boot progress, touch, WiFi, audio, battery, backlight, CPU frequency and GPU
power objects are not part of this archive (they drive sheng hardware; this board's input and GPU are later steps). The
original-class checks and bindings are unchanged. Its inputs come from build-display-abi.py (the reference candidate and the
audit seed).

Run in the build tree, after a kernel step whose prelink roots carry the original IOMobileGraphicsFamily
(scripts/build-evb10-display.py is the entry point of board.toml's [kernel] archives and decides when).
"""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/native-legacy-display';OUT.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho,backed_offset,exports
def run(cmd,**kw):return subprocess.check_output([str(x) for x in cmd],timeout=90,**kw)
def require(v,why):
    if not v:raise SystemExit(why)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
print(run(['python3',ROOT/'scripts/audit-mobile-graphics-legacy.py'],text=True),end='')
audit=json.loads((ROOT/'build/display-analysis/mobile-graphics-legacy-abi.json').read_text())
pure=audit['abstract_slots'];methods=audit['methods']
header='''// SPDX-License-Identifier: BSD-2-Clause-Patent
// Opaque ABI storage for the exact original class, not source-level access
// to its private fields. The actual original metadata supplies its full
// Framebuffer/Service/IOService ancestry. Only qualified base prototypes may
// be called from source; opaque entries are original dispatch targets only.
#pragma once
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
struct OriginalLegacySwapRequest;
class IOMobileFramebufferLegacy : public IOService {
 OSDeclareAbstractStructors(IOMobileFramebufferLegacy)
 unsigned char originalPrivate[0x18a0-sizeof(IOService)];
public:
 // Both fields are qualified by the complete original slot-249 getter.
 // This modifies this object's own mode state, never a vtable or executable.
 void installBootDimensions(uint32_t width,uint32_t height) {
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5808)=width;
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5812)=height;
 }
 const OriginalLegacySwapRequest *pendingSwapRequest() const {
  const OriginalLegacySwapRequest *request=nullptr;
  memcpy(&request,reinterpret_cast<const unsigned char *>(this)+5480,sizeof(request));return request;
 }
 // The real normal scanout backend removes its pending head before calling
 // slot 295. Slot 312 is a discard path (terminal state 8), not presentation.
 // The queue is a tail queue (head at +5480, tail pointer at +5488, each request's next at +2960 and previous-link at +2968): taking the head
 // moves the next request, if a client already queued one behind it (int-132: swap 3 arrived during swap 2's GPU copy and the old
 // single-entry form refused to retire swap 2, so every wait on it stayed busy), up to the head. The original queue's reference
 // moves to the current-swap slot for the original completion body.
 bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {
  auto self=reinterpret_cast<unsigned char *>(this);
  auto bytes=reinterpret_cast<const unsigned char *>(request);
  const void *current=nullptr,*head=nullptr,*tail=nullptr,*next=nullptr,*previous=nullptr;
  memcpy(&current,self+5472,8);memcpy(&head,self+5480,8);memcpy(&tail,self+5488,8);
  if(!request || current || head!=request)return false;
  memcpy(&next,bytes+2960,8);memcpy(&previous,bytes+2968,8);
  if(previous!=self+5480)return false;
  if(next) {
   const void *link=self+5480;
   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next))+2968,&link,8);
  } else if(tail!=bytes+2960)return false;
  memcpy(self+5472,&request,8);
  if(next) {memcpy(self+5480,&next,8);}
  else {const void *empty=nullptr,*emptyTail=self+5480;memcpy(self+5480,&empty,8);memcpy(self+5488,&emptyTail,8);}
  return true;
 }
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 void systemWillShutdown(IOOptionBits) override;
 IOService *probe(IOService *,SInt32 *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
'''
for i in range(168,358):
    if i==184:header+=' virtual bool isMainDisplay();\n'
    elif i==188:header+=' virtual IOReturn getDotPitch(uint32_t *);\n'
    elif i==213:header+=' virtual IOReturn supportedFrameInfo(uint32_t,uint32_t *,char *,uint32_t *)=0;\n'
    elif i==249:header+=' virtual void getDisplayDimensions(uint32_t *,uint32_t *);\n'
    elif i==290:header+=' virtual void kickSwapQueue();\n'
    elif i==296:header+=' virtual bool validateSwap(const OriginalLegacySwapRequest *);\n'
    elif i==295:header+=' virtual void completeCurrentSwap();\n'
    elif i==337:header+=' virtual IOReturn collectCompletionReports(uint32_t,void *,uint32_t *,uint32_t)=0;\n'
    elif i==288:header+=' virtual bool startHardware();\n'
    elif i==301:header+=' virtual bool getRefreshAttributes(uint64_t *,bool);\n'
    elif i in pure:header+=f' virtual __attribute__((noreturn)) void opaque{i}()=0;\n'
    else:header+=f' virtual __attribute__((deprecated("Unclassified ABI: never call from source"))) void opaque{i}();\n'
header+='};\nstatic_assert(sizeof(IOMobileFramebufferLegacy)==0x18a0,"Actual original instance extent");\n'
(OUT/'OriginalLegacyFramebufferABI.h').write_text(header)
print(run(['python3',ROOT/'scripts/build-evb10-drivers.py','NativeLegacyDisplay'],text=True),end='')
obj=OUT/'NativeLegacyDisplay.o';raw=obj.read_bytes();image=macho(raw)
st=next(p for c,p,_ in image['commands'] if c==2);sy,count,strings,_=struct.unpack_from('<4I',raw,st+8);symbols=[]
for i in range(count):
    ix,kind,section,_,va=struct.unpack_from('<IBBHQ',raw,sy+i*16)
    symbols.append((raw[strings+ix:raw.index(b'\0',strings+ix)].decode(),va,kind))
vtable=next(va for name,va,_ in symbols if name=='__ZTV19NativeLegacyDisplay');relocations={}
for segment in image['segments']:
    for sec in segment['sections']:
        if sec['va']<=vtable<sec['va']+sec['size']:
            ro,rc=struct.unpack_from('<2I',raw,sec['command_offset']+56)
            for i in range(rc):
                offset,bits=struct.unpack_from('<2I',raw,ro+i*8)
                if bits>>27&1 and bits>>28==0 and bits>>25&3==3:relocations[sec['va']+offset]=symbols[bits&0xffffff][0]
compiled=[relocations.get(vtable+16+i*8) for i in range(358)]
require(all(compiled) and vtable+16+358*8 not in relocations,'Exact owned 358-entry vtable')
candidate=ROOT/'build/xnu-prelink/native-owner-mobile-graphics-candidate.macho'
if not candidate.exists():  # pruned: same fallback as scripts/audit-mobile-graphics-legacy.py (identical original family executable)
    later=sorted((ROOT/'build/xnu-prelink').glob('native-owner-ufs-cpu-desktop-*-candidate.macho'),key=lambda f:f.stat().st_mtime)
    require(later,'No reference candidate available');candidate=later[-1]
old=candidate.read_bytes();old_image=macho(old)
names={n:v for v,group in exports(old,old_image).items() for n in group}
def read(va,n):
    at=backed_offset(old_image['segments'],va,n,len(old));return old[at:at+n]
bindings={'__ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass':0xfffffff0057865cc,
          '__ZN25IOMobileFramebufferLegacyD2Ev':0xfffffff005786618,
          '__ZN25IOMobileFramebufferLegacy10gMetaClassE':0xfffffff006b35940,
          '__ZTV25IOMobileFramebufferLegacy':int(audit['legacy_vptr'],16)-16}
owned=[];native=[]
for i,name in enumerate(compiled):
    expected=int(methods[i]['address'],16)
    if name.startswith(('__ZN19NativeLegacyDisplay','__ZNK19NativeLegacyDisplay')):
        owned.append(i);require(i in (0,1,7,14,84,86,87,141,155,184,188,288,290,296,301) or i in pure,'Only qualified own methods and guarded leaves override')
    elif '25IOMobileFramebufferLegacy' in name:
        require(i not in pure,'No unresolved original abstract backend target');bindings[name]=expected
    else:
        require(i<168 and names.get(name)==expected,'Actual inherited native base symbol '+str(i));native.append(i)
require(owned==sorted([0,1,7,14,84,86,87,141,155,184,188,288,290,296,301]+pure),'Exact owned method set')
undefined={n for n,_,k in symbols if k==1 and 'IOMobileFramebufferLegacy' in n}
require(undefined==set(bindings),'All and only actual original-class imports qualified')
# Exact original IOSurface getters, independently observed through the real
# original swap request. These aliases call original code; no object layout
# is substituted and no original method/vtable is patched.
for name,address,field,wide in [
    ('descriptor',0xfffffff004b9ead0,48,True),('width',0xfffffff004b9ebbc,88,True),
    ('height',0xfffffff004b9ebc4,96,True),('stride',0xfffffff004b9ebd4,104,True),
    ('allocation',0xfffffff004b9ebfc,144,True),('format',0xfffffff004b9ebac,128,False),
    ('planes',0xfffffff004b9ebf4,152,False)]:
    expected=(0xf9400000 if wide else 0xb9400000)|((field//(8 if wide else 4))<<10)
    require(read(address,8)==struct.pack('<2I',expected,0xd65f03c0),'Exact original surface getter '+name)
    bindings['_native_legacy_surface_'+name]=address
# The original display-refresh notification sender (self, timestamp, continuous host time, frame interval): it sends the six-word
# vsync message (type 5) to every client entry queued at +5592, the tail queue that selector 72's registration (slot 208) fills at
# +5512 + type*16. The platform subclass that normally calls it on each panel vblank is not part of this port.
require(read(0xfffffff005796720,16)==bytes.fromhex('ff0302d1fd7b07a9fdc30191a0831ff8') and
        read(0xfffffff005796830,4)==bytes.fromhex('08ed4af9') and
        read(0xfffffff00579685c,16)==bytes.fromhex('e2230091c3008052240080521684ff97') and
        read(0xfffffff005795fbc,4)==bytes.fromhex('0ab182d2'),
        'Original vsync sender, its +5592 queue and six-word drop-allowed async send')
bindings['_native_legacy_vsync_notify']=0xfffffff005796720
# Slot 301 fills the six refresh words that setDisplayRefreshProperties publishes as IOMFBDisplayRefresh (minimum interval, its mach
# time, maximum interval, its mach time, step, its mach time; intervals in 32.32 seconds): the generic body fixes the minimum at
# 0x04444444 (1/60 s), and the render server paces its display timer by it.
require(int(methods[301]['address'],16)==0xfffffff00579ea5c and read(0xfffffff00579ea68,8)==bytes.fromhex('888888d28888a0f2') and
        read(0xfffffff00579ec54,4)==bytes.fromhex('08b544f9'),
        'Original slot-301 refresh words with the 1/60 s minimum, called by setDisplayRefreshProperties')
# setDisplayRefreshProperties itself (self): fills the six words through slot 301 and publishes them as IOMFBDisplayRefresh. Nothing in
# the family calls it (the platform subclass does), so without it the render server falls back to its own 1/60 s heartbeat.
require(read(0xfffffff00579ebb8,16)==bytes.fromhex('ff0302d1fd7b07a9fdc30191a0031ff8') and
        read(0xfffffff00579ed54,16)==bytes.fromhex('086540f90173ff90218c0c9100013fd6'),
        'Original setDisplayRefreshProperties publishing IOMFBDisplayRefresh through setProperty')
bindings['_native_legacy_set_refresh_properties']=0xfffffff00579ebb8
# Both protected destructors/constructors must follow the exact original chain.
def branch(pc):
    word=struct.unpack('<I',read(pc,4))[0];require(word&0xfc000000==0x94000000,'Actual protected base call')
    value=word&0x3ffffff;return pc+4*(value-(1<<26) if value&(1<<25) else value)
require(branch(0xfffffff0057865fc)==0xfffffff00577b510 and branch(0xfffffff005786630)==0xfffffff00577b55c,
        'Original protected Legacy C2/D2 really call their original Service bases')
# Actual caller passes only self at slot 288 and tests bit zero of w0.
require(read(0xfffffff005787ef8,16)==bytes.fromhex('080040f9088144f900013fd6e0000037'),
        'Original bool start_hardware() call and result check')
getter=int(methods[249]['address'],16)
for offset,field in [(0x30,5808),(0x58,5812)]:
    require(struct.unpack('<I',read(getter+offset,4))[0]==0xb9400000|((field//4)<<10)|(8<<5)|8,
            'Actual original width/height getter fields')
require(int(methods[295]['address'],16)==0xfffffff005795b88 and
        read(0xfffffff005795b98,16)==bytes.fromhex('e00740f9280080520101001272d3ff97'),
        'Actual original slot-295 passes true to the normal current completion body')
require(read(0xfffffff00578aa7c,24)==bytes.fromhex('080040f9084545f9030080d2e20303aa0400805200013fd6'),
        'Original completion calls optional report slot 337 with null outputs and zero limit')
require(read(0xfffffff005777e9c,16)==bytes.fromhex('080040f908e142f900013fd6c0000036'),
        'Original isMainDisplay dispatch uses only self and tests boolean bit zero')
asm=OUT/'original-bindings.S';asm.write_text(''.join(f'.globl {name}\n.set {name}, {va:#x}\n' for name,va in sorted(bindings.items())))
# The recorded compiler assembles the absolute symbols (the same way scripts/bind-emmc-storage.py does); no SDK needed.
record0=json.loads((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
run([record0['arguments'][0],'--target=arm64-apple-ios17.0','-Werror','-Wno-incompatible-sysroot','-c',asm,'-o',OUT/'original-bindings.o'],cwd=record0['directory'])
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
        probe=subprocess.run([candidate,'--remove-section=__DATA,__mod_init_func',str(OUT/'NativeLegacyDisplay.o'),
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
late=OUT/'NativeLegacyDisplay.late.o'
late_object(obj,late,'__GLOBAL__sub_I_NativeLegacyDisplay.cpp')
def code(data,info):
    sec=next(s for g in info['segments'] for s in g['sections'] if s['name']=='__text');return data[sec['fileoff']:sec['fileoff']+sec['size']]
require(code(raw,image)==code(late.read_bytes(),macho(late.read_bytes())),'Only owned initializer section registration changes')
r=json.loads((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
cmd=[];args=r['arguments'];i=0;source=ROOT/'kits/evb10/LegacyDisplayProbe.cpp'
while i<len(args):
    value=args[i]
    if value==r['file']:cmd.append(str(source))
    elif value=='-o':cmd+=['-o',str(OUT/'LegacyDisplayProbe.o')];i+=1
    elif value=='-Xclang' and args[i+1].startswith('-W'):i+=1
    elif not value.startswith('-W'):cmd.append(value)
    i+=1
cmd+=['-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unnecessary-virtual-specifier','-Wno-c99-designator','-Wno-reorder-init-list',
      '-Wno-deprecated-enum-enum-conversion','-I',str(ROOT/'.cache/xnu-work/osfmk')]
run(cmd,cwd=r['directory'])
keystore_source=ROOT/'kits/evb10/KeyStoreProbe.cpp'
keystore_obj=OUT/'KeyStoreProbe.o'
keystore_cmd=[str(keystore_source) if str(arg)==str(source) else str(keystore_obj) if str(arg)==str(OUT/'LegacyDisplayProbe.o') else arg for arg in cmd]
run(keystore_cmd,cwd=r['directory'])
print(run(['python3',ROOT/'scripts/build-evb10-drivers.py','NativeKeyStoreEvents'],text=True),end='')
keyevents=ROOT/'build/apple-key-store-state'
keyevents_late=keyevents/'NativeKeyStoreEvents.late.o'
late_object(keyevents/'NativeKeyStoreEvents.o',keyevents_late,'__GLOBAL__sub_I_NativeKeyStoreEvents.cpp')
require(code((keyevents/'NativeKeyStoreEvents.o').read_bytes(),macho((keyevents/'NativeKeyStoreEvents.o').read_bytes()))==
        code(keyevents_late.read_bytes(),macho(keyevents_late.read_bytes())),'Owned event-service instructions unchanged by late init staging')
# The display backend, its probe, the key store and the presenter. sheng's HID input, boot process, log, reset, touch, WiFi,
# audio, battery, backlight, CPU frequency and GPU power objects stay out.
archive=OUT/'libNativeLegacyDisplay.late.a';archive.unlink(missing_ok=True)
ar=str(tool('llvm-ar'))
run([ar,'rcs',archive,late,OUT/'original-bindings.o',OUT/'LegacyDisplayProbe.o',keystore_obj,keyevents_late])
sources=[ROOT/'kits/evb10/Evb10Display/NativeLegacyDisplay.cpp',ROOT/'drivers/GopFramebuffer/NativeLegacyRequest.h',source,keystore_source,
         ROOT/'drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp']
print(run(['python3',ROOT/'scripts/build-evb10-drivers.py','NativeGOPPresenter'],text=True),end='')
gop=ROOT/'build/native-gop-presenter';gop_late=gop/'NativeGOPPresenter.late.o'
late_object(gop/'NativeGOPPresenter.o',gop_late,'__GLOBAL__sub_I_NativeGOPPresenter.cpp')
require(code((gop/'NativeGOPPresenter.o').read_bytes(),macho((gop/'NativeGOPPresenter.o').read_bytes()))==
        code(gop_late.read_bytes(),macho(gop_late.read_bytes())),'Owned presenter instructions unchanged by late init staging')
run([ar,'rcs',archive,gop_late])
sources.extend([ROOT/'drivers/GopFramebuffer/NativeGOPPresenter.cpp',ROOT/'drivers/GopFramebuffer/NativeGOPWire.h'])
(OUT/'build-report.json').write_text(json.dumps(dict(
    source_sha256={str(p.relative_to(ROOT)):sha(p) for p in sources},generated_abi_sha256=sha(OUT/'OriginalLegacyFramebufferABI.h'),
    original_candidate_sha256=sha(candidate),original_audit_sha256=sha(ROOT/'build/display-analysis/mobile-graphics-legacy-abi.json'),
    compiled_object_sha256=sha(obj),late_object_sha256=sha(late),archive_sha256=sha(archive),
    bindings={n:hex(v) for n,v in bindings.items()},virtual_entries=358,owned_entries=owned,inherited_native_entries=native,
    guarded_backend_leaves=[i for i in pure if i not in (213,337)],original_instructions_modified=False,driver_initialized=False,display_presented=False,desktop=False),indent=2)+'\n')
print('Built genuine original Legacy inheritance/dispatch; seven backend leaves still guarded, runtime lifecycle unqualified')
