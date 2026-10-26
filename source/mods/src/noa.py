"""Read and write ERISA-Archive (.noa) files.

Layout (EntisGLS): a 0x40-byte header ("Entis\\x1a", "ERISA-Archive file",
u64 at 0x38 = file length - 0x40), then a "DirEntry" record and one
"filedata" record per entry. Entry offsets count from 0x40.
Directory entry: u64 size, u32 attr, u32 encoding (0 stored,
0x80000010 ERISA-N), u64 offset, 8-byte FILETIME, u32 extra length + extra,
u32 name length + CP932 name (NUL-terminated).
"""
import struct

STORED = 0
ERISAN = 0x80000010


class Entry:
    def __init__(self, name, size, attr, encoding, filetime, extra, record):
        self.name, self.size, self.attr, self.encoding = name, size, attr, encoding
        self.filetime, self.extra = filetime, extra
        self.record = record  # the whole "filedata" record, header included

    @classmethod
    def stored(cls, name, data, filetime=b'\0' * 8):
        record = b'filedata' + struct.pack('<Q', len(data)) + data
        return cls(name, len(data), 0, STORED, filetime, b'', record)

    def payload(self):
        length = struct.unpack_from('<Q', self.record, 8)[0]
        return self.record[16:16 + length]


def read(path):
    data = open(path, 'rb').read()
    header = data[:0x40]
    if data[:5] != b'Entis' or b'ERISA-Archive file' not in header:
        raise ValueError(f'{path}: not an ERISA archive')
    tag, length = data[0x40:0x48], struct.unpack_from('<Q', data, 0x48)[0]
    if tag != b'DirEntry':
        raise ValueError(f'{path}: no DirEntry record')
    pos = 0x50
    count = struct.unpack_from('<I', data, pos)[0]
    pos += 4
    entries = []
    for _ in range(count):
        size, attr, encoding, offset = struct.unpack_from('<QIIQ', data, pos)
        pos += 24
        filetime = data[pos:pos + 8]
        pos += 8
        extra_length = struct.unpack_from('<I', data, pos)[0]
        pos += 4
        extra = data[pos:pos + extra_length]
        pos += extra_length
        name_length = struct.unpack_from('<I', data, pos)[0]
        pos += 4
        name = data[pos:pos + name_length].rstrip(b'\0').decode('cp932')
        pos += name_length
        start = 0x40 + offset
        if data[start:start + 8] != b'filedata':
            raise ValueError(f'{path}: {name} has no filedata record')
        record_length = struct.unpack_from('<Q', data, start + 8)[0]
        entries.append(Entry(name, size, attr, encoding, filetime, extra,
                             data[start:start + 16 + record_length]))
    return header, entries


def write(path, header, entries):
    names = [e.name.encode('cp932') + b'\0' for e in entries]
    directory_length = 4 + sum(24 + 8 + 4 + len(e.extra) + 4 + len(n) for e, n in zip(entries, names))
    offset = 16 + directory_length  # records follow the DirEntry record
    directory = bytearray(struct.pack('<I', len(entries)))
    for entry, name in zip(entries, names):
        directory += struct.pack('<QIIQ', entry.size, entry.attr, entry.encoding, offset)
        directory += entry.filetime + struct.pack('<I', len(entry.extra)) + entry.extra
        directory += struct.pack('<I', len(name)) + name
        offset += len(entry.record)
    body = b'DirEntry' + struct.pack('<Q', directory_length) + bytes(directory)
    body += b''.join(e.record for e in entries)
    out = bytearray(header[:0x38]) + struct.pack('<Q', len(body)) + body
    open(path, 'wb').write(out)
