#!/usr/bin/env python3
"""Pure-Python ERISA-N ("Nemesis") decoder.

Line-by-line port of SGLERISANDecodeContext (EntisGLS4 SDK,
Cotopha/Source/common/sakuragl/erisa/sgl_erisa_decode_context.cpp and
sgl_erisa_context_model.cpp), 32-bit DWORD arithmetic emulated.
Used for .noa entries with encoding 0x80000010 and for the "object  " record
of cotomi context files (system.dat / config.dat / SaveNNN.dat).
"""
import struct

M32 = 0xFFFFFFFF
TOTAL_LIMIT = 0x2000
SYMBOL_SORTS = 0x101
SUB_SORT_MAX = 0x80
PROB_SLOT_MAX = 0x800
ESC = -1
NEM_SIZE = 0x10000
NEM_MASK = 0xFFFF
NEM_IDX_MASK = 0xFF
SHIFT = (1, 3, 4, 5)
NEW_LIMIT = (0x01, 0x08, 0x10, 0x20)


class Bits:
    def __init__(self, data):
        n = len(data)
        if n & 3:
            data = data + b'\0' * (4 - (n & 3))
        self.data = data
        self.pos = 0
        self.cnt = 0
        self.buf = 0

    def _prefetch(self):
        if self.pos >= len(self.data):
            return False
        self.buf = struct.unpack_from('>I', self.data, self.pos)[0]
        self.pos += 4
        self.cnt = 32
        return True

    def bit(self):
        if self.cnt == 0 and not self._prefetch():
            return 1
        v = -(self.buf >> 31)
        self.cnt -= 1
        self.buf = (self.buf << 1) & M32
        return v

    def nbits(self, n):
        code = 0
        while n:
            if self.cnt == 0 and not self._prefetch():
                break
            c = min(n, self.cnt)
            if c < 32:
                code = ((code << c) | (self.buf >> (32 - c))) & M32
                n -= c
                self.cnt -= c
                self.buf = (self.buf << c) & M32
            else:
                code = self.buf
                n -= c
                self.cnt -= c
                self.buf = 0
        return code


class Model:
    __slots__ = ('total', 'sorts', 'occ', 'sym', 'sub_occ', 'sub_sym')

    def __init__(self):
        self.total = SYMBOL_SORTS
        self.sorts = SYMBOL_SORTS
        self.occ = [1] * SYMBOL_SORTS
        self.sym = list(range(256)) + [ESC]
        self.sub_occ = [0] * SUB_SORT_MAX
        self.sub_sym = [-1] * SUB_SORT_MAX

    def half(self):
        t = 0
        for i in range(self.sorts):
            self.occ[i] = (self.occ[i] + 1) >> 1
            t += self.occ[i]
        self.total = t
        for i in range(SUB_SORT_MAX):
            self.sub_occ[i] >>= 1

    def increase(self, index):
        occ, sym = self.occ, self.sym
        w = (occ[index] + 1) & 0xFFFF
        s = sym[index]
        index -= 1
        while index >= 0:
            if occ[index] >= w:
                break
            occ[index + 1] = occ[index]
            sym[index + 1] = sym[index]
            index -= 1
        index += 1
        occ[index] = w
        sym[index] = s
        self.total += 1
        if self.total >= TOTAL_LIMIT:
            self.half()
        return index

    def add(self, s):
        i = self.sorts
        self.sorts += 1
        self.total += 1
        if i >= len(self.occ):
            self.occ.append(1); self.sym.append(s)
        else:
            self.sym[i] = s; self.occ[i] = 1
        return i


def _empty_model():
    m = Model.__new__(Model)
    m.total = 0
    m.sorts = 0
    m.occ = [0] * SYMBOL_SORTS
    m.sym = [0] * SYMBOL_SORTS
    m.sub_occ = [0] * SUB_SORT_MAX
    m.sub_sym = [-1] * SUB_SORT_MAX
    return m


class ERISANDecoder:
    def __init__(self, data):
        self.bs = Bits(data)
        self.work = []
        self.base = Model()
        self.last = [0, 0, 0, 0]
        self.ilast = 0
        self.code = self.bs.nbits(32)
        self.augend = 0xFFFF
        self.post = 0
        self.nbuf = bytearray(NEM_SIZE)
        self.lookup_first = [0] * 256
        self.lookup_index = [[0] * 256 for _ in range(256)]
        self.nidx = 0
        self.plen = Model()
        self.pidx = Model()
        self.rlen = Model()
        self.nleft = 0
        self.nnext = -1
        self.eof = False

    def _index(self, m):
        acc = ((self.code * m.total) & M32) // self.augend
        wacc = acc & 0xFFFF
        fs = 0
        i = 0
        occ = m.occ
        while True:
            o = occ[i]
            if wacc < o:
                break
            wacc -= o
            fs += o
            i += 1
            if i >= m.sorts:
                return -1
        self.code = (self.code - (((self.augend * fs) & M32) + m.total - 1) // m.total) & M32
        self.augend = ((self.augend * o) & M32) // m.total
        if self.augend == 0:
            raise ValueError('ERISA augend underflow')
        bs = self.bs
        while not (self.augend & 0x8000):
            nb = bs.bit()
            if nb == 1:
                self.post += 1
                if self.post >= 256:
                    return -1
                nb = 0
            self.code = ((self.code << 1) | (nb & 1)) & M32
            self.augend = (self.augend << 1) & M32
        self.code &= 0xFFFF
        return i

    def _code(self, m):
        i = self._index(m)
        if i < 0:
            return ESC
        s = m.sym[i]
        m.increase(i)
        return s

    def _emit(self, b, out):
        self.last[self.ilast] = b
        self.ilast = (self.ilast + 1) & 3
        f = self.lookup_first[b]
        self.lookup_index[b][f] = self.nidx
        self.lookup_first[b] = (f + 1) & NEM_IDX_MASK
        self.nbuf[self.nidx] = b
        self.nidx = (self.nidx + 1) & NEM_MASK
        out.append(b)

    def decode(self, count):
        out = bytearray()
        if self.eof:
            return bytes(out)
        base = self.base
        while len(out) < count:
            if self.nleft > 0:
                n = min(self.nleft, count - len(out))
                lastb = self.nbuf[(self.nidx - 1) & NEM_MASK]
                for _ in range(n):
                    b = lastb
                    if self.nnext >= 0:
                        b = self.nbuf[self.nnext]
                        self.nnext = (self.nnext + 1) & NEM_MASK
                    self._emit(b, out)
                    lastb = b
                self.nleft -= n
                continue
            model = base
            ideg = 0
            while ideg < 4:
                il = self.last[(self.ilast + 3 - ideg) & 3] >> SHIFT[ideg]
                if model.sub_sym[il] < 0:
                    break
                model = self.work[model.sub_sym[il]]
                ideg += 1
            isym = self._index(model)
            if isym < 0:
                return bytes(out)
            sym = model.sym[isym]
            model.increase(isym)
            nemesis = False
            if sym == ESC:
                if model is not base:
                    isym = self._index(base)
                    if isym < 0:
                        return bytes(out)
                    sym = base.sym[isym]
                    base.increase(isym)
                    if sym != ESC:
                        model.add(sym)
                    else:
                        nemesis = True
                else:
                    nemesis = True
            if nemesis:
                pi = self._code(self.pidx)
                if pi == ESC:
                    self.eof = True
                    return bytes(out)
                ln = self._code(self.rlen if pi == 0 else self.plen)
                if ln == ESC:
                    return bytes(out)
                lastb = self.nbuf[(self.nidx - 1) & NEM_MASK]
                self.nleft = ln
                if pi == 0:
                    self.nnext = -1
                else:
                    nn = self.lookup_index[lastb][(self.lookup_first[lastb] - pi) & NEM_IDX_MASK]
                    self.nnext = (nn + 1) & NEM_MASK
                continue
            b = sym & 0xFF
            self._emit(b, out)
            if len(self.work) < PROB_SLOT_MAX and ideg < 4:
                isub = b >> SHIFT[ideg]
                model.sub_occ[isub] += 1
                if model.sub_occ[isub] >= NEW_LIMIT[ideg]:
                    parent = model
                    model = base
                    i = 0
                    while i <= ideg:
                        isub = self.last[(self.ilast + 3 - i) & 3] >> SHIFT[i]
                        if model.sub_sym[isub] < 0:
                            break
                        model = self.work[model.sub_sym[isub]]
                        i += 1
                    if i <= ideg and model.sub_sym[isub] < 0:
                        new = _empty_model()
                        model.sub_sym[isub] = len(self.work)
                        self.work.append(new)
                        j = 0
                        for k in range(parent.sorts):
                            w = parent.occ[k] >> 4
                            if w > 0 and parent.sym[k] != ESC:
                                new.total += w
                                new.occ[j] = w
                                new.sym[j] = parent.sym[k]
                                j += 1
                        new.total += 1
                        new.occ[j] = 1
                        new.sym[j] = ESC
                        new.sorts = j + 1
        return bytes(out)


def decode(data, size):
    return ERISANDecoder(data).decode(size)
