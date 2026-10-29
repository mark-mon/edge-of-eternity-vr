#!/usr/bin/env python3
# xref.py <rva> : every direct call/jmp to rva in GameAssembly .text, with the calling method's name
import sys, re, pefile, bisect, struct, os
GA = os.environ.get('GA', '/mnt/user-data/uploads/Edge Of Eternity/GameAssembly.dll')
TY = os.environ.get('TYPES', '/home/claude/types.txt')
pe = pefile.PE(GA, fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
funcs = sorted((e.struct.BeginAddress, e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION)
starts = [f[0] for f in funcs]
names = {}; cls = None
for l in open(TY, encoding='utf-8', errors='ignore'):
    if l.startswith(('class ', 'struct ', 'enum ')): cls = l.split(' ')[1].strip()
    m = re.match(r'  M (?:static )?(.*?) rva=0x([0-9a-f]+)', l)
    if m and m.group(2) != '0':
        nm = re.search(r'(\S+)\(', m.group(1)); names.setdefault(int(m.group(2), 16), f'{cls}::{nm.group(1) if nm else "?"}')
import numpy as np
nkeys = sorted(names)
targets = [int(a, 16) for a in sys.argv[1:]]
for sec in pe.sections:
    if not (sec.Characteristics & 0x20000000): continue   # executable
    data = np.frombuffer(sec.get_data(), dtype=np.uint8); va = sec.VirtualAddress
    n = len(data) - 5
    idx = np.nonzero((data[:n] == 0xE8) | (data[:n] == 0xE9))[0]
    rel = (data[idx+1].astype(np.int64) | (data[idx+2].astype(np.int64) << 8) | (data[idx+3].astype(np.int64) << 16) | (data[idx+4].astype(np.int64) << 24))
    rel = np.where(rel >= 2**31, rel - 2**32, rel)
    tgt = va + idx + 5 + rel
    for t in targets:
        for i in idx[tgt == t]:
            src = va + int(i)
            k = bisect.bisect_right(starts, src) - 1
            fb = funcs[k][0] if k >= 0 else 0
            # nearest named method at or before the call
            j = bisect.bisect_right(nkeys, src) - 1
            print(f'{src:#x} {"call" if data[i]==0xE8 else "jmp "} -> {t:#x}  in {names.get(nkeys[j], "?")} (+{src-nkeys[j]:#x})')
