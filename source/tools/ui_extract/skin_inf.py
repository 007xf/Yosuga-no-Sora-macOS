#!/usr/bin/env python3
"""Decode an EntisGLS skin definition (system.inf inside frame/title/option.noa) into
a readable component map.

usage: skin_inf.py <extracted archive dir> [--json out.json] [--md out.md]

The directory must contain system.inf (CP932 XML) and manifest.json written by
tools/eri_export (for image sizes).  Output: resources, styles (per state image
rect + text style), and forms (<page>/<window>) with every item resolved to the
image rect it will draw.
"""
import json, os, re, sys
import xml.etree.ElementTree as ET

STATES = ['normal', 'focus', 'pushed', 'pushed_focus', 'active', 'active_pushed', 'disabled', 'push_disabled']
RICH = re.compile(r'^\s*([A-Za-z0-9_]+)\s*(?::\s*RECT\s*\(\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\))?\s*$')


def load(dirpath):
    raw = open(os.path.join(dirpath, 'system.inf'), 'rb').read()
    text = raw.decode('cp932', 'replace').replace('encoding="Shift_JIS"', '')
    root = ET.fromstring(text)
    sizes = {}
    mf = os.path.join(dirpath, 'manifest.json')
    if os.path.exists(mf):
        for e in json.load(open(mf, encoding='utf-8'))['entries']:
            if 'width' in e:
                sizes[e['name'].lower()] = (e['width'], e['height'])
    return root, sizes


def color(v):
    if not v: return None
    v = v.strip()
    if v.upper().endswith('H'):
        n = int(v[:-1], 16)
        return '#%06X' % (n & 0xFFFFFF) + ('' if (n >> 24) == 0 else ' a=%02X' % (n >> 24))
    return v


def parse(dirpath):
    root, sizes = load(dirpath)
    res = {}
    for el in root.find('resource') if root.find('resource') is not None else []:
        rid, src = el.get('id'), el.get('src')
        res[rid] = {'tag': el.tag, 'src': src, 'size': sizes.get((src or '').lower()),
                    'present': (src or '').lower() in sizes}

    def rich(spec):
        if not spec: return None
        m = RICH.match(spec)
        if not m: return {'raw': spec}
        rid = m.group(1)
        r = res.get(rid)
        out = {'id': rid, 'file': r['src'] if r else None}
        if m.group(2) is not None:
            out['rect'] = [int(m.group(i)) for i in range(2, 6)]
        elif r and r['size']:
            out['rect'] = [0, 0, r['size'][0], r['size'][1]]
        if not r or not r['present']:
            out['missing'] = True
        return out

    def textstyle(node):
        d = {}
        a = node.find('arrange')
        if a is not None:
            d['arrange'] = {k: v for k, v in a.attrib.items() if v != ''}
        f = node.find('font')
        if f is not None:
            d['font'] = {'face': f.get('face'), 'size': f.get('size'), 'bold': f.get('bold'), 'italic': f.get('italic')}
        t = node.find('text')
        if t is not None:
            d['text'] = {'color': color(t.get('color')), 'transparency': t.get('transparency')}
        s = node.find('shadow')
        if s is not None:
            d['shadow'] = {'color': color(s.get('color')), 'x': s.get('x'), 'y': s.get('y'),
                           'transparency': s.get('transparency')}
        b = node.find('border')
        if b is not None:
            d['border'] = dict(b.attrib)
        return d

    styles = {}
    ds = root.find('declare_style')
    for st in (ds if ds is not None else []):
        if st.tag != 'style': continue
        sid, typ = st.get('id'), st.get('type')
        d = {'type': typ}
        if typ == 'button':
            a = st.find('arrange')
            d['button_type'] = a.get('type') if a is not None else 'button'
            states = {}
            for s in STATES:
                n = st.find(s)
                if n is None: continue
                states[s] = {'image': rich(n.get('image'))}
                if n.get('color'): states[s]['back_color'] = color(n.get('color'))
                states[s].update(textstyle(n))
            d['states'] = states
            m = st.find('mask')
            if m is not None:
                d['mask'] = rich(m.get('image'))
                if m.get('rect'): d['mask_rect_hit'] = m.get('rect')
        elif typ == 'scroll_bar':
            a = st.find('arrange')
            d['orientation'] = a.get('type') if a is not None else None
            d['bar_offset'] = a.get('bar_offset') if a is not None else None
            for s in ('up_button', 'down_button'):
                n = st.find(s)
                if n is not None: d[s] = n.get('style')
            d['states'] = {}
            for s in ('normal', 'focus', 'tracking', 'disabled'):
                n = st.find(s)
                if n is not None:
                    d['states'][s] = {'bar': rich(n.get('bar')), 'column': rich(n.get('column'))}
        else:
            d.update(textstyle(st))
        styles[sid] = d

    forms = {}
    for fm in root:
        if fm.tag not in ('page', 'window'): continue
        fid = fm.get('id')
        f = {'kind': fm.tag, 'width': fm.get('width'), 'height': fm.get('height')}
        if fm.tag == 'window':
            f['bg'] = rich(fm.get('bg'))
            if f['bg'] and f['bg'].get('rect'):
                f['size_from_bg'] = f['bg']['rect'][2:]
        items = []
        for it in fm:
            d = {'tag': it.tag}
            for k in ('id', 'style', 'x', 'y', 'width', 'height', 'text', 'group', 'rsrc'):
                if it.get(k) not in (None, ''):
                    d[k] = it.get(k)
            if it.tag == 'image':
                d['image'] = rich(it.get('rsrc'))
            elif it.tag == 'button' and it.get('style') in styles:
                st = styles[it.get('style')]
                d['images'] = {s: v['image'] for s, v in st['states'].items() if v.get('image')}
                n = d['images'].get('normal')
                if n and n.get('rect') and (not it.get('width') or it.get('width') == '0'):
                    d['size'] = n['rect'][2:]
                else:
                    d['size'] = [int(it.get('width') or 0), int(it.get('height') or 0)]
            elif it.tag == 'scroll_bar' and it.get('style') in styles:
                d['length'] = it.get('width')
                d['orientation'] = styles[it.get('style')].get('orientation')
            elif it.tag == 'object':
                d['subform'] = it.get('style')
            items.append(d)
        f['items'] = items
        forms[fid] = f

    used = {}
    def mark(r, who):
        if r and r.get('id'): used.setdefault(r['id'], set()).add(who)
    for sid, st in styles.items():
        for s, v in st.get('states', {}).items():
            for k in ('image', 'bar', 'column'):
                mark(v.get(k), sid + '.' + s)
        mark(st.get('mask'), sid + '.mask')
    for fid, f in forms.items():
        mark(f.get('bg'), fid + '.bg')
        for it in f['items']:
            mark(it.get('image'), fid + '.' + (it.get('id') or it['tag']))
    for rid in res:
        res[rid]['used_by'] = sorted(used.get(rid, []))
    return {'resources': res, 'styles': styles, 'forms': forms}


def rect_s(r):
    if not r: return ''
    s = r.get('file') or r.get('id') or r.get('raw', '?')
    if r.get('rect'):
        x, y, w, h = r['rect']
        s += ' [%d,%d %dx%d]' % (x, y, w, h)
    if r.get('missing'): s += ' (MISSING)'
    return s


def to_md(d, title):
    o = ['## ' + title, '', '### Resources', '', '| id | file | size | used by (skin) |', '|---|---|---|---|']
    for rid, r in d['resources'].items():
        sz = '%dx%d' % tuple(r['size']) if r['size'] else 'n/a'
        o.append('| %s | %s | %s | %s |' % (rid, r['src'], sz, ', '.join(r['used_by']) or '-'))
    o += ['', '### Styles', '']
    for sid, st in d['styles'].items():
        if st['type'] == 'button':
            o.append('- **%s** (%s%s)' % (sid, st['button_type'], ', mask ' + rect_s(st['mask']) if st.get('mask') else ''))
            for s, v in st['states'].items():
                extra = []
                if v.get('arrange', {}).get('width') or v.get('arrange', {}).get('height'):
                    a = v['arrange']; extra.append('text@%s,%s %sx%s' % (a.get('left', ''), a.get('top', ''), a.get('width', ''), a.get('height', '')))
                if v.get('font'): extra.append('font %s %s%s' % (v['font']['face'], v['font']['size'], ' bold' if v['font']['bold'] == 'true' else ''))
                if v.get('text'): extra.append('color %s' % v['text']['color'])
                if v.get('shadow'): extra.append('shadow %s (%s,%s)' % (v['shadow']['color'], v['shadow']['x'], v['shadow']['y']))
                o.append('  - %s: %s%s' % (s, rect_s(v.get('image')), ('; ' + '; '.join(extra)) if extra else ''))
        elif st['type'] == 'scroll_bar':
            o.append('- **%s** (scroll_bar %s)' % (sid, st.get('orientation')))
            for s, v in st['states'].items():
                o.append('  - %s: bar %s, column %s' % (s, rect_s(v['bar']), rect_s(v['column'])))
        else:
            f = st.get('font', {}); a = st.get('arrange', {}); t = st.get('text', {}); sh = st.get('shadow', {})
            o.append('- **%s** (%s): font %s size %s%s, align %s, line_height %s, color %s, shadow %s (%s,%s)' % (
                sid, st['type'], f.get('face'), f.get('size'), ' bold' if f.get('bold') == 'true' else '',
                a.get('align'), a.get('line_height'), t.get('color'), sh.get('color'), sh.get('x'), sh.get('y')))
    o += ['', '### Forms (pages / windows)', '']
    for fid, f in d['forms'].items():
        hdr = '#### %s  (<%s> %sx%s' % (fid, f['kind'], f['width'], f['height'])
        if f.get('bg'): hdr += ', bg ' + rect_s(f['bg'])
        o += [hdr + ')', '', '| item | id | x,y | size | style / image |', '|---|---|---|---|---|']
        for it in f['items']:
            if it['tag'] == 'button':
                imgs = it.get('images', {})
                st = '%s: normal %s; focus %s' % (it.get('style'), rect_s(imgs.get('normal')), rect_s(imgs.get('focus')))
                if imgs.get('disabled'): st += '; disabled ' + rect_s(imgs['disabled'])
                sz = 'x'.join(map(str, it.get('size', [])))
            elif it['tag'] == 'image':
                st = rect_s(it.get('image')); sz = ''
                if it.get('image', {}).get('rect'): sz = '%dx%d' % tuple(it['image']['rect'][2:])
            elif it['tag'] == 'scroll_bar':
                st = '%s (%s, length %s)' % (it.get('style'), it.get('orientation'), it.get('length')); sz = ''
            elif it['tag'] == 'object':
                st = 'sub-form ' + it.get('subform', ''); sz = ''
            else:
                st = '%s text="%s"' % (it.get('style'), it.get('text', '')); sz = '%sx%s' % (it.get('width', ''), it.get('height', ''))
            o.append('| %s | %s | %s,%s | %s | %s |' % (it['tag'], it.get('id', ''), it.get('x', ''), it.get('y', ''), sz, st))
        o.append('')
    return '\n'.join(o) + '\n'


def main():
    a = sys.argv[1:]
    d = parse(a[0])
    if '--json' in a:
        json.dump(d, open(a[a.index('--json') + 1], 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    if '--md' in a:
        open(a[a.index('--md') + 1], 'w', encoding='utf-8').write(to_md(d, a[a.index('--title') + 1] if '--title' in a else a[0]))
    if '--json' not in a and '--md' not in a:
        sys.stdout.write(to_md(d, a[0]))


if __name__ == '__main__':
    main()
