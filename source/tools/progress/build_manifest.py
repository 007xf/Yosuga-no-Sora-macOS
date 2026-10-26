#!/usr/bin/env python3
"""Generate progress units from reachable scenarios and the original gallery.

Only identifiers, flag numbers and source hashes are emitted; no game dialogue.
Run --check to verify the checked-in manifest/native constants match the sources.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'vendor/EntisGLS-SDL3/native/runtime/cotopha_port'
ROUTES = {
    'yosuga': [('A034', '穹', 10614, 'A033', 4), ('B034', '奈绪', 17899, 'B033', 3),
               ('C036', '瑛', 26706, 'C035B', 2), ('D044', '一叶', 33342, 'D043', 5),
               ('E037', '元香', 39078, 'E036B', 6)],
    'haruka': [('A022', '穹', 5554, 'A022', 4), ('G025', '八寻', 9751, 'G025', 8),
               ('H032', '梢', 14700, 'H032', 9)],
}

CHAPTER_NAMES = {
    'yosuga': {'Z': '共通篇', 'A': '穹篇', 'B': '奈绪篇', 'C': '瑛篇', 'D': '一叶篇', 'E': '元香篇'},
    'haruka': {'A': '穹篇', 'G': '八寻篇', 'H': '梢篇',
               'WEB': '网络短篇', 'KARAOKE': '卡拉 OK', 'COUNTDOWN': '倒计时'},
}


def scenario_links(text, scripts):
    targets = re.findall(r'\bChange\("([^"\x00]+)"\)', text)
    targets += re.findall(r'\bsc([A-Za-z0-9_]+)\(', text)[1:]
    return sorted({t.upper().replace('-', '_') for t in targets
                   if t.upper().replace('-', '_') in scripts})


def guide_catalog(game, scripts, seen, routes, all_reads):
    """Group actual scripts/read flags without exporting dialogue or options.

    A chapter is a route/common section, not an invented story title. Yosuga's
    G000--G003 are detours reached only from Motoka's E route, not another route.
    Verify that ownership from the graph rather than classifying by letter.
    """
    links = {key: scenario_links(scripts[key], scripts) for key in seen}
    incoming = {key: set() for key in seen}
    for source, targets in links.items():
        for target in targets:
            if target in incoming and source != target:
                incoming[target].add(source)
    owners = {}
    for key in seen:
        group = key[3:4] if key.startswith('00_') else key.rstrip('0123456789')
        if group in CHAPTER_NAMES[game]:
            owners[key] = group
    while len(owners) < len(seen):
        changed = False
        for key in sorted(seen - owners.keys()):
            parents = incoming[key]
            groups = {owners[parent] for parent in parents if parent in owners}
            if parents and parents <= owners.keys() and len(groups) == 1:
                owners[key] = next(iter(groups)); changed = True
        if not changed:
            raise ValueError(f'{game}: unproven chapter ownership {sorted(seen - owners.keys())}')
    chapters, covered = [], set()
    for group, name in CHAPTER_NAMES[game].items():
        members = sorted(key for key in seen if owners[key] == group)
        ids = {int(n) for key in members for n in re.findall(r'\bHitret\((\d+)', scripts[key])}
        if ids & covered:
            raise ValueError(f'{game}: read units overlap between chapters')
        covered.update(ids)
        route = next((r for r in routes if r['id'].startswith(group)), None)
        chapters.append({'id': group, 'name': name, 'entry': members[0],
                         'route_id': route['id'] if route else '',
                         'ending_flag': route['read_flag'] if route else 0,
                         'read_ranges': ranges(ids), 'read_total': len(ids),
                         'scenarios': members})
    if covered != all_reads:
        raise ValueError(f'{game}: chapters do not cover the exact read denominator')
    specials = [{'id': 'gallery', 'title': '鉴赏', 'command': 'ID_APPRECIATION',
                 'condition': 'game_clear' if game == 'yosuga' else 'always'}]
    if game == 'haruka':
        for key, title, command in [('sora', '穹的短篇', 'ID_SORA'),
                                    ('yahiro', '八寻的短篇', 'ID_YAHIRO'),
                                    ('kozue', '梢的短篇', 'ID_KOZUE'),
                                    ('karaoke', '卡拉 OK', 'ID_KARAOKE')]:
            specials.append({'id': key, 'title': title, 'command': command, 'condition': 'always'})
        specials.extend({'id': f'web{i:02}', 'title': f'网络短篇 {i}',
                         'command': f'ID_WEB{i:02}', 'condition': 'always'} for i in range(1, 7))
        specials.extend([{'id': 'countdown', 'title': '倒计时', 'command': 'ID_COUNTDOWN', 'condition': 'always'},
                         {'id': 'wallpaper', 'title': '壁纸鉴赏', 'command': 'ID_WALLPAPER', 'condition': 'always'}])
    return chapters, specials, [{'id': key, 'chapter': owners[key], 'next': links[key]} for key in sorted(seen)]


def ranges(values):
    out = []
    for x in sorted(values):
        if out and x == out[-1][1] + 1:
            out[-1][1] = x
        else:
            out.append([x, x])
    return out


def build():
    manifest = {'schema_version': 1, 'games': {}}
    for game in ROUTES:
        paths = sorted((ROOT / 'analysis/decomp' / game).glob('sc*.cos'))
        scripts = {p.stem[2:].upper(): p.read_text() for p in paths}
        roots = (['00_Z000'] if game == 'yosuga' else
                 ['00_A001', '00_G001', '00_H001', 'KARAOKE', 'COUNTDOWN'] +
                 [f'WEB{i:02}' for i in range(1, 7)])
        seen, todo = set(), roots[:]
        while todo:
            key = todo.pop().upper().replace('-', '_')
            if key == 'EXIT_SCENARIO' or key in seen:
                continue
            if key not in scripts:
                raise ValueError(f'{game}: missing reachable scenario {key}')
            seen.add(key)
            text = scripts[key]
            todo.extend(re.findall(r'\bChange\("([^"\x00]+)"\)', text))
            todo.extend(re.findall(r'\bsc([A-Za-z0-9_]+)\(', text)[1:])
        text = '\n'.join(scripts[k] for k in sorted(seen))
        ids = {int(s) for s in re.findall(r'\bHitret\((\d+)', text)}
        if not ids or min(ids) < 1 or max(ids) >= 48000:
            raise ValueError(f'{game}: invalid reachable read ids')
        extras = json.loads((ROOT / f'analysis/extras/{game}_extras.json').read_text())
        cg = sorted({p['flag'] for p in extras['palette'] if p['kind'] == 'cg'})
        # Every denominator entry must have a real scenario which can set its bit.
        cgs = {s.upper() for s in re.findall(r'\bSetCg\("([^"\x00]+)"', text)}
        bustups = {s.upper() for s in re.findall(r'\bSetBustup\("([^"\x00]+)"', text)}
        reachable_flags = set()
        for rule in extras['cg_unlock_rules']:
            match = (rule['name'].upper() in cgs if rule['kind'] == 'cg' else
                     any(rule['group'].upper() in s and rule['name'].upper() in s for s in bustups))
            if match:
                reachable_flags.update((rule['base_flag'], rule['flag']))
        if set(cg) - reachable_flags:
            raise ValueError(f'{game}: gallery includes unreachable flags {set(cg) - reachable_flags}')
        recollect = sorted({p['recollect_flag'] for p in extras['recollection']})
        set_recollect = {int(x) for x in re.findall(r'\bOnRecollectFlag\((\d+)', text)}
        if set(recollect) - set_recollect:
            raise ValueError(f'{game}: unreachable recollection')
        routes = []
        for route, name, flag, roll, roll_id in ROUTES[game]:
            ending = scripts['00_' + route]
            found = re.findall(r'\bHitret\((\d+)', ending)
            if int(found[-1]) != flag or f'StaffRoll({roll_id})' not in scripts['00_' + roll]:
                raise ValueError(f'{game}: route ending changed: {route}')
            routes.append({'id': route, 'name': name, 'read_flag': flag,
                           'ending_source': f'sc00_{route}.cos',
                           'staff_roll_source': f'sc00_{roll}.cos', 'staff_roll_id': roll_id})
        digest = hashlib.sha256()
        for p in paths:
            digest.update(p.name.encode()); digest.update(p.read_bytes())
        chapters, specials, scenario_catalog = guide_catalog(game, scripts, seen, routes, ids)
        manifest['games'][game] = {
            'title': '缘之空' if game == 'yosuga' else '悠之空',
            'routes': routes, 'cg_flags': cg, 'recollection_flags': recollect,
            'music_total': len(extras['music']),
            'music_rule': 'game_clear' if game == 'yosuga' else 'always',
            'read_ranges': ranges(ids), 'read_total': len(ids),
            'reachable_scenarios': sorted(seen),
            'excluded_scenarios': sorted(set(scripts) - seen),
            'script_sha256': digest.hexdigest(),
            'chapters': chapters, 'specials': specials, 'scenario_catalog': scenario_catalog,
        }
    return manifest


def header(manifest):
    lines = ['// Generated by tools/progress/build_manifest.py; do not edit.',
             '// Only reachable, distinct read flag IDs and original gallery units.',
             '#pragma once', 'namespace entis::progress::data {',
             'struct Range { unsigned first, last; };',
             'struct Route { const char *id, *name; unsigned flag; };',
             'struct Chapter { const char *id, *name, *entry, *routeId; unsigned endingFlag;',
             '    const Range *readRanges; unsigned rangeCount, readTotal; };',
             'struct Scenario { const char *id, *chapter; };',
             'struct SpecialEntry { const char *id, *title, *command, *condition; };']
    for game, d in manifest['games'].items():
        for name, values in [('Cg', d['cg_flags']), ('Recollection', d['recollection_flags'])]:
            lines.append(f'inline constexpr unsigned {game}{name}[] = {{' + ','.join(map(str, values)) + '};')
        lines.append(f'inline constexpr Range {game}Read[] = {{' +
                     ','.join('{%d,%d}' % tuple(pair) for pair in d['read_ranges']) + '};')
        lines.append(f'inline constexpr Route {game}Routes[] = {{' +
                     ','.join('{"%s","%s",%d}' % (r['id'], r['name'], r['read_flag']) for r in d['routes']) + '};')
        for chapter in d['chapters']:
            lines.append(f'inline constexpr Range {game}Chapter{chapter["id"]}Read[] = {{' +
                         ','.join('{%d,%d}' % tuple(pair) for pair in chapter['read_ranges']) + '};')
        lines.append(f'inline constexpr Chapter {game}Chapters[] = {{' +
                     ','.join('{"%s","%s","%s","%s",%d,%sChapter%sRead,%d,%d}' %
                              (c['id'], c['name'], c['entry'], c['route_id'], c['ending_flag'],
                               game, c['id'], len(c['read_ranges']), c['read_total']) for c in d['chapters']) + '};')
        lines.append(f'inline constexpr Scenario {game}Scenarios[] = {{' +
                     ','.join('{"%s","%s"}' % (s['id'], s['chapter']) for s in d['scenario_catalog']) + '};')
        lines.append(f'inline constexpr SpecialEntry {game}Specials[] = {{' +
                     ','.join('{"%s","%s","%s","%s"}' %
                              (s['id'], s['title'], s['command'], s['condition']) for s in d['specials']) + '};')
    lines.append('} // namespace entis::progress::data')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(); parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    manifest = build()
    outputs = {ROOT / 'tools/progress/manifest.json': json.dumps(manifest, ensure_ascii=False, indent=2) + '\n',
               NATIVE / 'game_progress_data.h': header(manifest)}
    for path, text in outputs.items():
        if args.check:
            if not path.exists() or path.read_text() != text:
                raise SystemExit(f'Stale generated progress data: {path}')
        else:
            path.write_text(text)
    print(json.dumps({g: {k: d[k] for k in ['read_total', 'music_total', 'excluded_scenarios']}
                      for g, d in manifest['games'].items()}))


if __name__ == '__main__':
    main()
