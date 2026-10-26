#!/usr/bin/env python3
"""Cross-check native calculations, graph denominators and unsigned flag words."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parent))
from reference import ROOT, calculate
import build_manifest


def empty():
    return {k: {'flag': [0] * n} for k, n in [('readFlag', 1500), ('cgFlag', 128), ('recollectFlag', 1)]}


def mark(obj, name, *bits):
    for bit in bits:
        obj[name]['flag'][bit // 32] |= 1 << (bit % 32)


class ProgressTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='progress-', dir=ROOT / 'work/progress')
        cls.probe = Path(cls.directory.name) / 'model-probe'
        native = ROOT / 'vendor/EntisGLS-SDL3/native'
        subprocess.run(['clang++', '-std=c++17', '-DENTISGLS_PROGRESS_MODEL_ONLY', '-I', str(native),
                        str(native / 'runtime/cotopha_port/game_progress.cpp'),
                        str(Path(__file__).parent / 'model_probe.cpp'), '-o', str(cls.probe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def native(self, game, obj):
        fields = [game, str(int(bool(obj.get('gameClear', 0))))]
        for name in ['readFlag', 'cgFlag', 'recollectFlag']:
            words = obj[name]['flag']; fields.append(str(len(words)))
            fields.extend(str(word & 0xffffffff) for word in words)
        result = subprocess.run([str(self.probe)], input=' '.join(fields), text=True, capture_output=True, check=True)
        return json.loads(result.stdout)

    def compare(self, game, obj):
        expected = calculate(game, obj, 'save', 12345)
        actual = self.native(game, obj)
        self.assertAlmostEqual(actual['read'].pop('percent'), expected['read'].pop('percent'), places=8)
        self.assertEqual(actual, expected)
        return actual

    def test_blank_does_not_divide_by_flag_capacity(self):
        for game, total, cg, recollect in [('yosuga', 39343, 128, 18), ('haruka', 16088, 87, 8)]:
            with self.subTest(game=game):
                result = self.compare(game, empty())
                self.assertEqual(result['read'], {'completed': 0, 'total': total})
                self.assertEqual(result['cg'], {'unlocked': 0, 'total': cg})
                self.assertEqual(result['recollection'], {'unlocked': 0, 'total': recollect})
                self.assertEqual(result['music']['unlocked'], 0 if game == 'yosuga' else 22)
                self.assertFalse(any(route['complete'] for route in result['routes']))

    def test_all_bits_signed_words_complete_exactly_once(self):
        for game in ['yosuga', 'haruka']:
            obj = empty()
            for flag in obj.values():
                flag['flag'][:] = [-1] * len(flag['flag'])
            obj['gameClear'] = -1
            result = self.compare(game, obj)
            for key in ['cg', 'recollection', 'music']:
                self.assertEqual(result[key]['unlocked'], result[key]['total'])
            self.assertEqual(result['read']['completed'], result['read']['total'])
            self.assertTrue(all(route['complete'] for route in result['routes']))

    def test_phantom_bits_do_not_inflate_counts(self):
        spec = build_manifest.build()['games']
        for game in spec:
            obj = empty()
            known = {i for low, high in spec[game]['read_ranges'] for i in range(low, high + 1)}
            hole = next(i for i in range(1, 48000) if i not in known)
            mark(obj, 'readFlag', 0, hole, 47999)
            mark(obj, 'cgFlag', 0, 4095)
            mark(obj, 'recollectFlag', 0, 31)
            result = self.compare(game, obj)
            self.assertEqual(result['read']['completed'], 0)
            self.assertEqual(result['cg']['unlocked'], 0)
            self.assertEqual(result['recollection']['unlocked'], 0)

    def test_route_names_are_attached_to_specific_terminal_bits(self):
        for game in ['yosuga', 'haruka']:
            obj = empty()
            flag = 33342 if game == 'yosuga' else 9751
            mark(obj, 'readFlag', flag, 31, 32)
            result = self.compare(game, obj)
            complete = [r['name'] for r in result['routes'] if r['complete']]
            self.assertEqual(complete, ['一叶'] if game == 'yosuga' else ['八寻'])

    def test_generated_data_is_current_and_read_graph_excludes_stub(self):
        manifest = build_manifest.build()
        self.assertEqual(json.loads((Path(__file__).parent / 'manifest.json').read_text()), manifest)
        self.assertEqual(manifest['games']['yosuga']['excluded_scenarios'], ['00_B18B'])
        self.assertEqual(manifest['games']['haruka']['excluded_scenarios'], [])

    def test_chapters_partition_real_read_units_and_keep_detours_in_their_route(self):
        for game, spec in build_manifest.build()['games'].items():
            covered = set()
            for chapter in spec['chapters']:
                ids = {i for a, b in chapter['read_ranges'] for i in range(a, b + 1)}
                self.assertFalse(covered & ids, (game, chapter['id']))
                self.assertEqual(len(ids), chapter['read_total'])
                self.assertIn(chapter['entry'], chapter['scenarios'])
                if chapter['ending_flag']:
                    self.assertIn(chapter['ending_flag'], ids)
                covered.update(ids)
            known = {i for a, b in spec['read_ranges'] for i in range(a, b + 1)}
            self.assertEqual(covered, known)
            catalog = {s['id']: s for s in spec['scenario_catalog']}
            self.assertEqual(set(catalog), set(spec['reachable_scenarios']))
            self.assertTrue(all(set(s['next']) <= set(catalog) for s in catalog.values()))
            if game == 'yosuga':
                self.assertTrue(all(catalog[f'00_G{i:03}']['chapter'] == 'E' for i in range(4)))
                self.assertEqual(set(c['id'] for c in spec['chapters']), set('ZABCDE'))

    def test_special_entries_keep_original_commands_and_unlock_rules(self):
        source = json.loads((ROOT / 'analysis/extras/title_actions.json').read_text())
        for game, spec in build_manifest.build()['games'].items():
            commands = {a.get('original', {}).get('command') for a in source[game]['actions']}
            commands.update(f'ID_WEB{i:02}' for i in range(1, 7))
            self.assertTrue(all(s['command'] in commands for s in spec['specials']))
            gallery = next(s for s in spec['specials'] if s['id'] == 'gallery')
            self.assertEqual(gallery['condition'], 'game_clear' if game == 'yosuga' else 'always')
            if game == 'haruka':
                self.assertEqual(len(spec['specials']), 13)
                self.assertTrue(all(s['condition'] == 'always' for s in spec['specials']))


if __name__ == '__main__':
    unittest.main()
