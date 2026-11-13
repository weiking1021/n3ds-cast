"""Embed our ELF's three load segments; no external payload is installed/read."""
from pathlib import Path
import struct
import sys

data=Path(sys.argv[1]).read_bytes()
assert data[:7]==b'\x7fELF\x01\x01\x01'
entry,phoff=struct.unpack_from('<II',data,24)
phsize,phnum=struct.unpack_from('<HH',data,42)
assert phsize==32 and phnum<=16
segments={}
for i in range(phnum):
    kind,offset,address,_,filesize,memsize,flags,align=struct.unpack_from('<8I',data,phoff+i*phsize)
    if kind!=1:continue
    assert flags in (5,4,6) and flags not in segments
    assert address%4096==0 and filesize<=memsize and offset+filesize<=len(data)
    pages=(filesize+4095)//4096
    assert pages and pages<256 and memsize<1024*1024
    assert flags==6 or (memsize+4095)//4096==pages
    segments[flags]=(address,pages,(memsize+4095)//4096,data[offset:offset+filesize].ljust(pages*4096,b'\0'))
assert set(segments)=={4,5,6} and entry==segments[5][0]==0x100000
blob=b'';defines=[]
for flags,label in ((5,'TEXT'),(4,'RO'),(6,'RW')):
    address,pages,total,content=segments[flags]
    defines.extend([f'#define NATIVE_{label}_ADDRESS 0x{address:08X}u',
                    f'#define NATIVE_{label}_PAGES {pages}u',
                    f'#define NATIVE_{label}_OFFSET {len(blob)}u'])
    if flags==6:
        defines.extend([f'#define NATIVE_RW_INIT_PAGES {pages}u',f'#define NATIVE_RW_TOTAL_PAGES {total}u'])
    blob+=content
defines.append(f'#define NATIVE_IMAGE_SIZE {len(blob)}u')
body=',\n'.join(','.join(str(b) for b in blob[i:i+32]) for i in range(0,len(blob),32))
Path(sys.argv[2]).write_text('#pragma once\n'+'\n'.join(defines)+'\nstatic const unsigned char native_image[]={\n'+body+'\n};\n')
print(f'PASS: embedded own resident, {len(blob)} initialized bytes, 3 validated segments')
