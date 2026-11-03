#!/usr/bin/env python3
"""Correct an inherited x86-64 ISA requirement when compiled code uses baseline ISA.

CachyOS optimized crt objects can mark an ELF as needing AVX-512 regardless of
the actual instruction set used by the project. Only relax a known GNU property
after checking the compiler-reported ISA used is baseline.
"""
import os
import struct
import sys
from pathlib import Path

ISA_NEEDED = 0xC0008002
ISA_USED = 0xC0010002

def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment

def normalize(path: Path):
    data = bytearray(path.read_bytes())
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('Expected little-endian ELF64')
    shoff = struct.unpack_from('<Q', data, 40)[0]
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 58)
    if not shoff or shnum == 0 or shstrndx >= shnum:
        raise ValueError('Missing ELF section headers')
    sections = []
    for i in range(shnum):
        pos = shoff + shentsize*i
        if pos+64 > len(data): raise ValueError('Truncated section header')
        name, typ = struct.unpack_from('<II', data, pos)
        off, size = struct.unpack_from('<QQ', data, pos+24)
        if off+size > len(data): raise ValueError('Truncated section')
        sections.append((name,typ,off,size))
    names = sections[shstrndx]
    strtab=data[names[2]:names[2]+names[3]]
    def section_name(index):
        end=strtab.find(b'\0',index)
        return strtab[index:end if end>=0 else len(strtab)]
    candidates=[s for s in sections if section_name(s[0])==b'.note.gnu.property']
    if not candidates:
        print('ELF ISA: no GNU property note, nothing to normalize')
        return
    changed=False
    for _,stype,off,size in candidates:
        if stype != 7: raise ValueError('GNU property section is not SHT_NOTE')
        end=off+size; pos=off
        while pos+12<=end:
            namesz,descsz,ntype=struct.unpack_from('<III',data,pos);pos+=12
            name=bytes(data[pos:pos+namesz]);pos=align(pos+namesz,4)
            desc=pos;descend=pos+descsz;pos=align(descend,4)
            if pos>end:raise ValueError('Bad GNU note length')
            if name.rstrip(b'\0')!=b'GNU' or ntype!=5:continue
            props=[]; pp=desc
            while pp+8<=descend:
                prop,sz=struct.unpack_from('<II',data,pp);pp+=8
                if pp+sz>descend:raise ValueError('Bad GNU property length')
                props.append((prop,pp,sz))
                pp=align(pp+sz,8)
            used=[struct.unpack_from('<I',data,p)[0] for t,p,n in props if t==ISA_USED and n==4]
            if used and any(u & ~1 for u in used):
                raise ValueError('Refusing to rewrite executable using higher ISA')
            for typ,p,n in props:
                if typ!=ISA_NEEDED:continue
                if n!=4:raise ValueError('Unexpected x86 ISA needed length')
                old=struct.unpack_from('<I',data,p)[0]
                if old & ~0xF: raise ValueError('Unknown ISA needed flags')
                if old != 1:
                    struct.pack_into('<I',data,p,1)
                    print(f'ELF ISA: corrected needed mask {old:#x} -> 0x1 (baseline)')
                    changed=True
    if changed:
        path.write_bytes(data)
        print('ELF ISA: updated GNU property; other notes left intact')
    else: print('ELF ISA: already compatible')

if __name__=='__main__':
    if len(sys.argv)!=2: raise SystemExit('usage: normalize-elf-isa.py EXECUTABLE')
    normalize(Path(sys.argv[1]))
