#!/usr/bin/env python3
"""Read-only reference for progress.json. Never writes or changes a save file."""
import argparse
import json
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/noa'))
import cotomi_dump


def flag_set(words, bit):
    index, shift = divmod(bit, 32)
    return index < len(words) and bool((words[index] & 0xffffffff) & (1 << shift))


def calculate(game, obj, event='load', timestamp=None):
    spec = json.loads((Path(__file__).parent / 'manifest.json').read_text())['games'][game]
    words = {k: obj.get(k, {}).get('flag', []) for k in ['readFlag', 'cgFlag', 'recollectFlag']}
    if not all(isinstance(v, list) and all(isinstance(w, int) for w in v) for v in words.values()):
        raise ValueError('Invalid flag arrays')
    read = sum(flag_set(words['readFlag'], bit) for low, high in spec['read_ranges'] for bit in range(low, high + 1))
    gallery_open = game == 'haruka' or bool(obj.get('gameClear', 0))
    return {
        'schema_version': 1, 'game': game, 'title': spec['title'], 'source': 'system.dat',
        'updated_at_unix': int(time.time()) if timestamp is None else timestamp,
        'snapshot_event': event, 'is_live': False,
        'routes': [{'id': route['id'], 'name': route['name'], 'read_flag': route['read_flag'],
                    'complete': flag_set(words['readFlag'], route['read_flag'])} for route in spec['routes']],
        'cg': {'unlocked': sum(flag_set(words['cgFlag'], bit) for bit in spec['cg_flags']),
               'total': len(spec['cg_flags'])},
        'recollection': {'unlocked': sum(flag_set(words['recollectFlag'], bit) for bit in spec['recollection_flags']),
                        'total': len(spec['recollection_flags'])},
        'music': {'unlocked': spec['music_total'] if gallery_open else 0, 'total': spec['music_total'],
                  'note': '任一路线通关后开放全部曲目' if game == 'yosuga' else '原作音乐鉴赏默认全部开放'},
        'read': {'completed': read, 'total': spec['read_total'], 'percent': read * 100 / spec['read_total']},
    }


def comparable(result):
    return {k: (v.copy() if isinstance(v, dict) else v) for k, v in result.items()
            if k not in ['updated_at_unix', 'snapshot_event']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', required=True, choices=['yosuga', 'haruka'])
    parser.add_argument('system_dat', type=Path)
    parser.add_argument('--compare', type=Path, help='Assert runtime snapshot matches this exact system.dat')
    args = parser.parse_args()
    result = calculate(args.game, cotomi_dump.load(args.system_dat))
    if args.compare:
        actual = json.loads(args.compare.read_text())
        a, b = comparable(actual), comparable(result)
        # C++ JSON uses sufficient decimal precision; compare percent numerically.
        pa, pb = a['read'].pop('percent'), b['read'].pop('percent')
        if a != b or abs(pa - pb) > 0.000001:
            raise SystemExit('Runtime progress snapshot differs from decoded system.dat')
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
