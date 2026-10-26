#!/usr/bin/env python3
"""Split a linear csx_inspect_legacy.py dump into functions and query them.

usage: csx_funcs.py DUMP --grep REGEX            # list functions whose body matches
       csx_funcs.py DUMP --show NAME [NAME...]   # print function bodies (obj.free removed)
       csx_funcs.py DUMP --save DIR NAME...      # write DIR/NAME.txt
"""
import re, sys, os

def load(path):
    funcs, cur, name = {}, [], None
    order = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if '  obj.enter ' in line and not re.search(r'obj\.enter\s+,', line):
                if name: funcs[name] = cur
                m = re.search(r'obj\.enter\s+(\S+?),', line)
                name = m.group(1) if m else line.split()[2]
                if name in funcs: name = name + '#' + line.split()[0]
                order.append(name)
                cur = [line.rstrip('\n')]
            elif name:
                cur.append(line.rstrip('\n'))
    if name: funcs[name] = cur
    return funcs, order

def main():
    a = sys.argv[1:]
    funcs, order = load(a[0])
    if a[1] == '--grep':
        rx = re.compile(a[2])
        for n in order:
            hits = [l for l in funcs[n] if rx.search(l)]
            if hits: print('%-45s %d' % (n, len(hits)))
    elif a[1] == '--show':
        for n in a[2:]:
            print('\n'.join(l for l in funcs.get(n, ['<missing %s>' % n]) if 'obj.free' not in l)); print()
    elif a[1] == '--save':
        d = a[2]; os.makedirs(d, exist_ok=True)
        for n in a[3:]:
            body = funcs.get(n)
            if not body: print('missing', n); continue
            fn = os.path.join(d, n.replace('::', '__').replace('/', '_') + '.txt')
            open(fn, 'w').write('\n'.join(body) + '\n')
            print(fn, len(body))

if __name__ == '__main__':
    main()
