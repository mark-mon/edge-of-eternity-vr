#!/usr/bin/env python3
# calls.py <rva> [maxbytes] : disassemble a GameAssembly.dll function and name every call target using eoevr_types.txt
import sys, re, pefile, capstone, bisect, os
GA = os.environ.get('GA', '/mnt/user-data/uploads/Edge Of Eternity/GameAssembly.dll')
TY = os.environ.get('TYPES', '/home/claude/types.txt')
pe = pefile.PE(GA, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
names = {}
cls = None
for l in open(TY, encoding='utf-8', errors='ignore'):
    if l.startswith(('class ', 'struct ', 'enum ')): cls = l.split(' ')[1].strip()
    m = re.match(r'  M (?:static )?(.*?) rva=0x([0-9a-f]+)', l)
    if m and m.group(2) != '0':
        r = int(m.group(2), 16)
        sig = m.group(1)
        nm = re.search(r'(\S+)\(', sig)
        names.setdefault(r, f'{cls}::{nm.group(1) if nm else sig}')
def fn_bounds(rva):
    # .pdata lookup
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
    for e in pe.DIRECTORY_ENTRY_EXCEPTION:
        if e.struct.BeginAddress <= rva < e.struct.EndAddress: return e.struct.BeginAddress, e.struct.EndAddress
    return rva, rva + 0x400
rva = int(sys.argv[1], 16)
b, e = fn_bounds(rva)
mx = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x3000
e = b + mx if len(sys.argv) > 2 else e
data = pe.get_data(b, e - b)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
print(f'{names.get(b, "?")}  [{b:#x}..{e:#x}]')
for ins in md.disasm(data, base + b):
    if ins.mnemonic in ('call', 'jmp') and ins.op_str.startswith('0x'):
        t = int(ins.op_str, 16) - base
        if ins.mnemonic == 'jmp' and b <= t < e: continue
        print(f'  {ins.address-base:#x}: {ins.mnemonic} {t:#x} {names.get(t, "")}')
    elif ins.mnemonic == 'call':
        print(f'  {ins.address-base:#x}: call {ins.op_str}')
