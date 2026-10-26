#!/usr/bin/env python3
"""Dump a Cotopha "cotomi context file" written by File.SaveObject
(system.dat, config.dat, SaveNNN.dat, QSave.dat) as JSON.

usage: python3 -I cotomi_dump.py <file.dat> [--summary]

Layout (EntisGLS3 GLS3/Source/glsctpfile.cpp ECSFile::SaveObject/OpenSaveFile):
  [optional BMP thumbnail ("BM", bfSize bytes) - written by SaveThumbnailImage]
  EMC header: "Entis\\x1a\\0\\0", fid 0xFFFFFFFF, 0x30 bytes "cotomi context file"
  record "title   " u64 len : ECSContext::SaveObject(title)   (0xFF = null)
  record "object  " u64 len : u32 rawSize + ERISA-N( ECSContext::SaveObject(obj) )
Object stream (glscs_context.cpp / glscsobj_*.cpp Save):
  u8 type (0 Object,1 Reference,2 Array,3 Hash,4 Integer,5 Real,6 String, 0xFF null)
  Object : u32 n + UTF-16 typename, then Structure body = Array body + u32 k names
  Array  : u32 n, n objects, default-object, u32 bounds
  Integer: i64 value, i64 mask     Real: f64     String: u32 n, UTF-16, u32 index, u32 lock
  Hash   : u32 n, n x (u32 taglen, UTF-16 tag, object), default-object
  Reference: i32 varOffset, i32 bounds, i32 funcOffset, i32 baseMode, u32 dims, dims x i32, object
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import erisan  # noqa: E402


class R:
    def __init__(self, b):
        self.b, self.p = b, 0

    def u8(self):
        v = self.b[self.p]; self.p += 1; return v

    def u32(self):
        v = struct.unpack_from('<I', self.b, self.p)[0]; self.p += 4; return v

    def i32(self):
        v = struct.unpack_from('<i', self.b, self.p)[0]; self.p += 4; return v

    def i64(self):
        v = struct.unpack_from('<q', self.b, self.p)[0]; self.p += 8; return v

    def f64(self):
        v = struct.unpack_from('<d', self.b, self.p)[0]; self.p += 8; return v

    def wstr(self, n):
        s = self.b[self.p:self.p + 2 * n].decode('utf-16le', 'replace'); self.p += 2 * n; return s


def obj(r):
    t = r.u8()
    if t == 0xFF:
        return None
    if t == 4:
        v = r.i64(); r.i64(); return v
    if t == 5:
        return r.f64()
    if t == 6:
        n = r.u32(); s = r.wstr(n); r.u32(); r.u32(); return s
    if t == 2:
        return array_body(r)[0]
    if t == 3:
        n = r.u32(); d = {}
        for _ in range(n):
            ln = r.u32()
            if ln == 0xFFFFFFFF:
                continue
            k = r.wstr(ln); d[k] = obj(r)
        obj(r)
        return d
    if t == 1:
        r.i32(); r.i32(); r.i32(); r.i32()
        for _ in range(r.u32()):
            r.i32()
        return {'@ref': obj(r)}
    if t == 0:
        tn = r.wstr(r.u32())
        vals, _ = array_body(r)
        names = [r.wstr(r.u32()) for _ in range(r.u32())]
        out = {'@type': tn}
        for i, v in enumerate(vals):
            out[names[i] if i < len(names) and names[i] else '#%d' % i] = v
        return out
    raise ValueError('unknown type %#x at %#x' % (t, r.p - 1))


def array_body(r):
    n = r.u32()
    vals = [obj(r) for _ in range(n)]
    d = obj(r)
    r.u32()
    return vals, d


def load(path):
    b = open(path, 'rb').read()
    p = 0
    if b[:2] == b'BM':
        p = struct.unpack_from('<I', b, 2)[0]
    if b[p:p + 6] != b'Entis\x1a':
        raise SystemExit('not an EMC file at %#x' % p)
    p += 0x40
    recs = {}
    while p + 16 <= len(b):
        tag = b[p:p + 8].decode('ascii', 'replace').strip()
        ln = struct.unpack_from('<Q', b, p + 8)[0]
        recs[tag] = b[p + 16:p + 16 + ln]
        p += 16 + ln
    o = recs['object']
    raw = erisan.decode(o[4:], struct.unpack_from('<I', o)[0])
    return obj(R(raw))


def flagbits(f):
    arr = f.get('flag') if isinstance(f, dict) else None
    if not arr:
        return []
    return [i * 32 + k for i, w in enumerate(arr) for k in range(32) if (w >> k) & 1]


def main():
    d = load(sys.argv[1])
    if '--summary' in sys.argv and isinstance(d, dict):
        for k, v in list(d.items()):
            if isinstance(v, dict) and v.get('@type') == 'Flag':
                d[k] = {'@type': 'Flag', 'set_bits': flagbits(v)}
        if isinstance(d.get('bookmark'), list):
            d['bookmark'] = {i: b for i, b in enumerate(d['bookmark']) if b and b.get('fSave')}
    json.dump(d, sys.stdout, ensure_ascii=False, indent=1)
    print()


if __name__ == '__main__':
    main()
