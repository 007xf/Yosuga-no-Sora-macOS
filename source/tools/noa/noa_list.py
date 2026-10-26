#!/usr/bin/env python3
"""List / extract entries of an ERISA-Archive (.noa). Input is only read.

usage: python3 -I noa_list.py <archive.noa> [--extract OUTDIR] [--match SUBSTR]

Layout (verified on title.noa / option.noa / frame.noa):
  0x40 "DirEntry" u64 len, u32 count, entries:
       u64 size, u32 attr, u32 encoding (0 stored, 0x80000010 ERISA-N),
       u64 offset, 8-byte FILETIME, u32 extra-len + extra, u32 name-len + name(cp932)
  The entry's "filedata" record header (tag + u64 stored length) sits at
  0x40 + offset; payload starts 16 bytes later.  ERISA-N payloads decode to `size`.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import erisan  # noqa: E402


def read_dir(f, base=0x40):
    f.seek(base)
    tag = f.read(8)
    struct.unpack('<Q', f.read(8))
    if tag != b'DirEntry':
        raise SystemExit('no DirEntry at %#x: %r' % (base, tag))
    cnt, = struct.unpack('<I', f.read(4))
    ents = []
    for _ in range(cnt):
        size, attr, enc, off = struct.unpack('<QIIQ', f.read(24))
        f.read(8)
        xl, = struct.unpack('<I', f.read(4))
        extra = f.read(xl)
        nl, = struct.unpack('<I', f.read(4))
        name = f.read(nl).rstrip(b'\0').decode('cp932', 'replace')
        ents.append(dict(name=name, size=size, attr=attr, enc=enc, rec=base + off, extra=extra))
    return ents


def read_entry(f, e):
    f.seek(e['rec'])
    tag = f.read(8)
    ln, = struct.unpack('<Q', f.read(8))
    if tag != b'filedata':
        raise ValueError('%s: record tag %r' % (e['name'], tag))
    raw = f.read(ln)
    if e['enc'] == 0:
        return raw[:e['size']]
    if e['enc'] == 0x80000010:
        return erisan.decode(raw, e['size'])
    raise ValueError('%s: unsupported encoding %#x' % (e['name'], e['enc']))


def main():
    a = sys.argv[1:]
    path = a[0]
    out = a[a.index('--extract') + 1] if '--extract' in a else None
    match = a[a.index('--match') + 1] if '--match' in a else None
    with open(path, 'rb') as f:
        for e in read_dir(f):
            print('%-28s size=%-9d attr=%#x enc=%#x' % (e['name'], e['size'], e['attr'], e['enc']))
            if out and (match is None or match.lower() in e['name'].lower()):
                data = read_entry(f, e)
                os.makedirs(out, exist_ok=True)
                with open(os.path.join(out, e['name']), 'wb') as g:
                    g.write(data)
                print('   -> %d bytes' % len(data))


if __name__ == '__main__':
    main()
