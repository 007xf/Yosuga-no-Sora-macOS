#!/usr/bin/env python3
"""Print the IDR_COTOMI launch configuration embedded in an EntisGLS .exe
(RCDATA resource, ERISA-N compressed UTF-16).  usage: python3 -I exe_cotomi.py <exe>"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import erisan

def main(path):
    b = open(path, 'rb').read()
    pe = struct.unpack_from('<I', b, 60)[0]
    nsec = struct.unpack_from('<H', b, pe + 6)[0]
    optsz = struct.unpack_from('<H', b, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from('<H', b, opt)[0]
    ddir = opt + (96 if magic == 0x10b else 112)
    rva_res = struct.unpack_from('<I', b, ddir + 2 * 8)[0]
    secs = []
    for i in range(nsec):
        s = opt + optsz + 40 * i
        vsz, va, rsz, raw = struct.unpack_from('<IIII', b, s + 8)
        secs.append((va, max(vsz, rsz), raw))
    def off(rva):
        for va, sz, raw in secs:
            if va <= rva < va + sz:
                return rva - va + raw
        raise ValueError(hex(rva))
    base = off(rva_res)
    def name(o):
        n = struct.unpack_from('<H', b, base + o)[0]
        return b[base + o + 2: base + o + 2 + 2 * n].decode('utf-16le')
    def walk(o, path):
        nn, ni = struct.unpack_from('<HH', b, base + o + 12)
        for i in range(nn + ni):
            nid, tgt = struct.unpack_from('<II', b, base + o + 16 + 8 * i)
            key = name(nid & 0x7fffffff) if nid & 0x80000000 else nid
            if tgt & 0x80000000:
                yield from walk(tgt & 0x7fffffff, path + [key])
            else:
                rva, sz = struct.unpack_from('<II', b, base + tgt)
                yield path + [key], b[off(rva): off(rva) + sz]
    for p, data in walk(0, []):
        if 'IDR_COTOMI' in [str(x) for x in p]:
            if data[:2] == b'\xff\xfe':
                txt = data.decode('utf-16')
            else:
                raw = erisan.ERISANDecoder(data).decode(1 << 20)
                txt = raw.decode('utf-16le', 'replace')
            print(txt.rstrip('\0'))
main(sys.argv[1])
