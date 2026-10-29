#!/usr/bin/env python3
# dis.py <rva> : disassemble a GameAssembly function; name direct calls (types dump) and show C strings referenced by
# rip-relative lea (icall names passed to il2cpp_resolve_icall).
import sys, re, os, pefile, capstone
GA = os.environ.get('GA', '/mnt/user-data/uploads/Edge Of Eternity/GameAssembly.dll')
TY = os.environ.get('TYPES', '/home/claude/types.txt')
pe = pefile.PE(GA, fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
names = {}; cls = None
for l in open(TY, encoding='utf-8', errors='ignore'):
    if l.startswith(('class ', 'struct ', 'enum ')): cls = l.split(' ')[1].strip()
    m = re.match(r'  M (?:static )?(.*?) rva=0x([0-9a-f]+)', l)
    if m and m.group(2) != '0':
        nm = re.search(r'(\S+)\(', m.group(1)); names.setdefault(int(m.group(2), 16), f'{cls}::{nm.group(1) if nm else m.group(1)}')
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
rva = int(sys.argv[1], 16); b, e = rva, rva + 0x400
for x in pe.DIRECTORY_ENTRY_EXCEPTION:
    if x.struct.BeginAddress <= rva < x.struct.EndAddress: b, e = x.struct.BeginAddress, x.struct.EndAddress
def cstr(r):
    try:
        d = pe.get_data(r, 200); s = d.split(b'\0')[0]
        if len(s) > 6 and all(32 <= c < 127 for c in s): return s.decode()
    except Exception: pass
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = False
print(f'{names.get(b,"?")} [{b:#x}..{e:#x}]')
for ins in md.disasm(pe.get_data(b, e - b), base + b):
    a = ins.address - base; extra = ''
    m = re.search(r'\[rip ([+-]) 0x([0-9a-f]+)\]', ins.op_str)
    if m:
        t = ins.address + ins.size + (int(m.group(2), 16) * (1 if m.group(1) == '+' else -1)) - base
        s = cstr(t)
        if s: extra = f'  "{s}"'
    if ins.mnemonic == 'call' and ins.op_str.startswith('0x'):
        t = int(ins.op_str, 16) - base; extra = f'  {names.get(t, hex(t))}'
    if extra or ins.mnemonic == 'call' or len(sys.argv) > 2:
        print(f'  {a:#x}: {ins.mnemonic} {ins.op_str}{extra}')
