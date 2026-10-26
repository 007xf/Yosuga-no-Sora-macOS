"""List or extract stored entries from an ERISA-Archive (.noa). Usage: noa.py <archive> [outdir] [name-glob]"""
import fnmatch, os, struct, sys

def entries(path):
    with open(path, 'rb') as f:
        f.seek(0x40)
        tag = f.read(8); length = struct.unpack('<Q', f.read(8))[0]
        assert tag == b'DirEntry', tag
        base = 0x50
        count = struct.unpack('<I', f.read(4))[0]
        out = []
        for _ in range(count):
            size, attr, enc, off = struct.unpack('<QIIQ', f.read(24))
            f.read(8)
            extra = struct.unpack('<I', f.read(4))[0]; f.read(extra)
            n = struct.unpack('<I', f.read(4))[0]
            name = f.read(n).rstrip(b'\0').decode('cp932', 'replace')
            out.append(dict(name=name, size=size, attr=attr, enc=enc, offset=base + off))
        return out

def read(path, e):
    with open(path, 'rb') as f:
        f.seek(e['offset'])
        tag = f.read(8); length = struct.unpack('<Q', f.read(8))[0]
        return tag, f.read(length)

if __name__ == '__main__':
    arc = sys.argv[1]
    es = entries(arc)
    if len(sys.argv) < 3:
        for e in es: print(f"{e['name']:28} size={e['size']:9} enc={e['enc']:#x}")
        sys.exit()
    out = sys.argv[2]; pat = sys.argv[3] if len(sys.argv) > 3 else '*'
    os.makedirs(out, exist_ok=True)
    for e in es:
        if fnmatch.fnmatch(e['name'], pat):
            tag, data = read(arc, e)
            open(os.path.join(out, e['name'] + ('' if e['enc'] == 0 else '.erisan')), 'wb').write(data)
            print(e['name'], tag, len(data), hex(e['enc']))
