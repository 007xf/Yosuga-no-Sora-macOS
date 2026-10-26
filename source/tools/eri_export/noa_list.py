#!/usr/bin/env python3
"""List entries of an ERISA-Archive (.noa). Usage: noa_list.py file.noa [--json]"""
import struct, sys, json

def read_dir(f, base, data_start, path=''):
    out = []
    f.seek(base)
    tag = f.read(8); ln, = struct.unpack('<Q', f.read(8))
    if tag != b'DirEntry':
        raise ValueError('bad dir tag %r at %x' % (tag, base))
    cnt, = struct.unpack('<I', f.read(4))
    ents = []
    for _ in range(cnt):
        size, attr, enc, off = struct.unpack('<QIIQ', f.read(24))
        ft = f.read(8)
        xl, = struct.unpack('<I', f.read(4)); extra = f.read(xl)
        nl, = struct.unpack('<I', f.read(4)); name = f.read(nl).rstrip(b'\0')
        name = name.decode('utf-8' if attr & 0x01000000 else 'cp932', 'replace')
        ents.append(dict(name=path + name, size=size, attr=attr, enc=enc, offset=data_start + off))
    for e in ents:
        if e['attr'] & 0x10:
            sub = read_dir(f, e['offset'] + 16, e['offset'] + 16, e['name'] + '/') if False else None
            # sub directory record: tag 'DirEntry' at offset
            out.append(e)
            out += read_dir(f, e['offset'], e['offset'] + 16, e['name'] + '/')
        else:
            out.append(e)
    return out

def main():
    p = sys.argv[1]
    with open(p, 'rb') as f:
        hdr = f.read(0x40)
        ents = read_dir(f, 0x40, 0x50)
    if '--json' in sys.argv:
        print(json.dumps(ents, ensure_ascii=False, indent=1)); return
    for e in ents:
        print('%-40s %10d attr=%08x enc=%08x off=%x' % (e['name'], e['size'], e['attr'], e['enc'], e['offset']))
    print(len(ents), 'entries', file=sys.stderr)

if __name__ == '__main__':
    main()
