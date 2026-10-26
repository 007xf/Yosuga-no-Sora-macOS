#!/usr/bin/env python3
"""Very small stack-folding pretty printer for csx_inspect_legacy.py function dumps.

usage: pseudo.py FILE...   (files written by csx_funcs.py --save)

It folds obj.load/element/operate/call sequences into expressions such as
  this.sprMenu.MovePosition(9, 124)
Control flow (jump/cjump) is printed raw with addresses.  Purely a reading aid.
"""
import re, sys

BIN = {'add': '+', 'sub': '-', 'mul': '*', 'div': '/', 'mod': '%', 'and': '&', 'or': '|', 'xor': '^',
       'shl': '<<', 'shr': '>>', 'lt': '<', 'le': '<=', 'gt': '>', 'ge': '>=', 'eq': '==', 'ne': '!='}


def run(path):
    out, st = [], []
    slot = [0]
    pop = lambda: st.pop() if st else '?'
    for line in open(path, encoding='utf-8'):
        m = re.match(r'^([0-9a-f]{8})\s+(\S+)\s*(.*)$', line.rstrip('\n'))
        if not m:
            if line.strip(): out.append(line.rstrip())
            continue
        addr, op, arg = m.groups()
        arg = arg.strip()
        if op == 'obj.enter':
            if not arg.startswith(','):
                slot[0] = arg.count("'") // 2
            out.append('%s  ### %s' % (addr, arg)); continue
        if op in ('obj.load', 'obj.load.ref'):
            arg = re.sub(r'^(static|global)\[\d+\] \((\w+)\)$', r'\2', arg)
            arg = re.sub(r'^stack\[(\d+)\]$', r'L\1', arg)
            st.append(arg)
        elif op == 'obj.new.stack':
            t, _, n = arg.partition(', ')
            out.append('%s  L%d: %s %s' % (addr, slot[0], t, n)); st.append('L%d' % slot[0]); slot[0] += 1
        elif op == 'obj.element':
            st.append('%s.%s' % (pop(), arg))
        elif op == 'obj.element.pop':
            i = pop(); b = pop(); st.append('%s[%s]' % (b, i))
        elif op.startswith('obj.call.'):
            mm = re.match(r'(\S+), (\d+)', arg)
            name, n = mm.group(1), int(mm.group(2))
            args = [pop() for _ in range(n)][::-1]
            if op == 'obj.call.this':
                st.append('%s.%s(%s)' % (args[0] if args else '?', name, ', '.join(args[1:])))
            else:
                st.append('%s(%s)' % (name, ', '.join(args)))
        elif op.startswith('obj.operate.') or op.startswith('obj.compare.'):
            k = op.split('.')[2]
            b = pop(); a = pop(); st.append('(%s %s %s)' % (a, BIN.get(k, k), b))
        elif op.startswith('obj.store'):
            k = op.split('.')
            v = pop(); t = pop()
            sym = '=' if k[2] == 'pop' or len(k) == 3 else BIN.get(k[2], k[2]) + '='
            out.append('%s  %s %s %s' % (addr, t, sym, v))
        elif op == 'obj.free':
            if st:
                v = st.pop()
                if '(' in v: out.append('%s  %s' % (addr, v))
        elif op in ('obj.cjump.z.pop', 'obj.cjump.nz.pop'):
            c = pop(); out.append('%s  if %s(%s) goto %s' % (addr, '!' if '.z.' in op else '', c, arg))
        elif op == 'obj.jump':
            out.append('%s  goto %s' % (addr, arg))
        elif op == 'obj.return':
            out.append('%s  return %s' % (addr, pop() if st else ''))
            st.clear()
        else:
            out.append('%s  [%s %s]' % (addr, op, arg))
    return out


if __name__ == '__main__':
    for p in sys.argv[1:]:
        print('\n'.join(run(p)))
        print()
