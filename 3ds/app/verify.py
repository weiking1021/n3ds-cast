from pathlib import Path
import struct
import subprocess
root=Path(__file__).resolve().parent
elf=(root/'build/resident.elf').read_bytes()
entry,offset=struct.unpack_from('<II',elf,24)
count=struct.unpack_from('<H',elf,44)[0]
segments=[]
for i in range(count):
    kind,source,address,_,size,memory,flags,align=struct.unpack_from('<8I',elf,offset+i*32)
    if kind==1:
        assert source+size<=len(elf) and address%4096==0
        segments.append((flags,elf[source:source+size].ljust((size+4095)//4096*4096,b'\0'),memory))
assert entry==0x100000 and sorted(x[0] for x in segments)==[4,5,6]
blob=b''.join(data for flags,data,memory in sorted(segments,key=lambda s:{5:0,4:1,6:2}[s[0]]))
package=(root/'n3ds-cast.3dsx').read_bytes()
assert package[:4]==b'3DSX' and len(package)>len(blob)
assert blob in package, 'Resident must be embedded inside the single delivered 3dsx'
assert (root/'assets/ui-font.bcfnt').read_bytes() in package, 'UI font must be embedded, not a separate upload'
committed=sum((memory+4095)//4096*4096 for _,_,memory in segments)+6*1024*1024+0x40000
assert committed<8*1024*1024
symbols=subprocess.check_output(['/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm',str(root/'build/resident.elf')]).decode()
for forbidden in ('hidInit','aptInit','gfxInitDefault','gspInit','PluginMenu','OSDImpl','dspInit','ndspInit','DSP_UnloadComponent','DSP_ReadPipeIfPossible','DSP_SetSemaphore','audio_stream_next','capture_encode_jpeg_rgb24'):
    assert not any(line.split()[-1]==forbidden for line in symbols.splitlines()),forbidden
assert any(line.split()[-1]=='capture_encode_3ds_bgr8_jpeg_420' for line in symbols.splitlines())
# APT:A IPC from the resident crashed NS (svcBreak) on hardware; keep it out.
assert b'APT:' not in blob, 'Resident must not open APT services'
for name in ('__appInit','__appExit','__libctru_exit','__system_allocateHeaps'):
    assert any(line.split()[-2:] == ['T',name] for line in symbols.splitlines()),name
print(f'PASS: single 3dsx ({len(package)} bytes), own embedded resident ({len(blob)} bytes), RAM budget, independent exit/heap, no resident HID/APT/GSP initialization')
