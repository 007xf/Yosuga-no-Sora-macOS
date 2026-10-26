"""Representation, artifact integrity and resume checks for bulk HD conversion."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np
from PIL import Image, ImageDraw

_spec = importlib.util.spec_from_file_location("bulk_upscale", Path(__file__).with_name("bulk_upscale.py"))
hd = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hd)
_build_spec = importlib.util.spec_from_file_location("build_bulk", Path(__file__).with_name("build_bulk.py"))
builder = importlib.util.module_from_spec(_build_spec)
_build_spec.loader.exec_module(builder)


class FaithfulUpscaleTests(unittest.TestCase):
    def test_unpremultiplication_only_changes_visible_translucent_rgb(self):
        raw = np.array([[[0, 0, 0, 0], [128, 64, 0, 128], [20, 30, 40, 255]]], dtype=np.uint8)
        canonical = np.asarray(hd.straight_reference(Image.fromarray(raw), "premultiplied"))
        np.testing.assert_array_equal(canonical[..., 3], raw[..., 3])
        np.testing.assert_array_equal(canonical[0, 2], raw[0, 2])
        np.testing.assert_array_equal(canonical[0, 1], [255, 128, 0, 128])
        np.testing.assert_array_equal(canonical[0, 0], [0, 0, 0, 0])

    def test_constant_edge_colour_does_not_acquire_black_outline(self):
        original = Image.new("RGBA", (24, 24), (0, 0, 0, 0))
        draw = ImageDraw.Draw(original)
        draw.rectangle((5, 5, 18, 18), fill=(240, 160, 80, 128))
        result = np.asarray(hd.faithful_resize(original, 3))
        visible = result[..., 3] > 0
        # Every visible edge pixel retains the same RGB, however low its alpha.
        np.testing.assert_array_equal(result[visible, :3], np.tile([240, 160, 80], (visible.sum(), 1)))
        np.testing.assert_array_equal(result[~visible, :3], np.zeros((int((~visible).sum()), 3)))
        # Alpha 128 sits directly on the legacy binary-mask threshold. The gate
        # must expose even a subtle roundtrip silhouette change, not relax IoU.
        report = hd.fidelity_metrics(original, Image.fromarray(result), 3)
        self.assertEqual(report["result"], "FAIL", report)
        self.assertLess(report["alpha_iou"], 0.97)

    def test_lossless_write_resume_and_tampered_output_recovery(self):
        with tempfile.TemporaryDirectory(prefix="yosuga-hd-") as folder:
            root = Path(folder)
            source = root / "source.png"
            image = Image.new("RGBA", (32, 24), (128, 180, 210, 255))
            ImageDraw.Draw(image).rectangle((5, 5, 20, 18), fill=(40, 70, 90, 255))
            image.save(source)
            original_hash = hd.sha256_file(source)
            asset = {"id": "test", "source": "source.png", "output": "hd.png", "alpha_mode": "straight", "scale": 3}
            first = hd.process_asset(asset, root)
            self.assertEqual(first["result"], "PASS", first)
            self.assertFalse(first["cached"])
            self.assertTrue(hd.png_rgba8(root / "hd.png", (96, 72)))
            second = hd.process_asset(asset, root)
            self.assertTrue(second["cached"])
            (root / "hd.png").write_bytes(b"tampered")
            repaired = hd.process_asset(asset, root)
            self.assertFalse(repaired["cached"])
            self.assertEqual(repaired["output_sha256"], first["output_sha256"])
            self.assertEqual(hd.sha256_file(source), original_hash)

    def test_never_overwrites_original(self):
        with tempfile.TemporaryDirectory(prefix="yosuga-hd-") as folder:
            root = Path(folder)
            Image.new("RGB", (8, 8), "white").save(root / "source.png")
            asset = {"id": "test", "source": "source.png", "output": "source.png", "alpha_mode": "straight", "scale": 3}
            with self.assertRaisesRegex(ValueError, "never overwrite"):
                hd.process_asset(asset, root)

    def test_source_consistency_improves_existing_edges_without_external_input(self):
        raw = np.zeros((13, 202, 4), dtype=np.uint8)
        raw[3:10, :, 3] = 255
        raw[3:10, ::2, :3] = 255
        original = Image.fromarray(raw)
        initial = hd.faithful_resize(original, 4)
        corrected = hd.source_consistency_step(original, initial)
        before = hd.fidelity_metrics(original, initial, 4)
        after = hd.fidelity_metrics(original, corrected, 4)
        self.assertLess(after["colour_diff"], before["colour_diff"])
        self.assertGreater(after["ssim"], before["ssim"])
        # An extreme one-pixel alternating pattern still fails strict fidelity;
        # improving a metric must never be sufficient to waive the threshold.
        self.assertEqual(after["result"], "FAIL")

    def test_runtime_manifest_uses_decompressed_source_hash_and_all_frames(self):
        with tempfile.TemporaryDirectory(prefix="yosuga-hd-") as folder:
            root = Path(folder)
            sources, output = root / "sources", root / "output"
            encoded, source_hash = "1" * 64, "2" * 64
            directory = sources / encoded
            directory.mkdir(parents=True)
            output.mkdir()
            metadata = {"width": 16, "height": 12, "frames": 2, "format": 1,
                        "alpha_mode": "opaque", "origin": [2, 3], "duration_ms": 100,
                        "sequence": [[0, 40], [1, 60]],
                        "verified_source_metadata_sha256": "3" * 64, "pngs": []}
            for frame in range(2):
                path = directory / f"frame{frame:04d}.png"
                Image.new("RGBA", (16, 12), (50 + frame * 80, 100, 180, 255)).save(path)
                metadata["pngs"].append({"frame": frame, "sha256": hd.sha256_file(path)})
            (directory / "metadata.json").write_text(json.dumps(metadata), encoding="utf-8")
            asset = {"id": "first", "encoded_sha256": encoded, "source_sha256": source_hash,
                     "metadata_sha256": "3" * 64, "game": "yosuga", "category": "ui_title",
                     "source_size": [16, 12], **{k: metadata[k] for k in
                        ("width", "height", "frames", "format", "alpha_mode", "origin", "duration_ms", "sequence")}}
            tasks, errors, unique = builder.make_tasks({"assets": [asset, {**asset, "id": "alias"}]}, sources, output, 4)
            self.assertFalse(errors)
            self.assertEqual(len(unique), 1)
            self.assertEqual(len(tasks), 2)
            self.assertEqual(tasks[1]["sequence"], metadata["sequence"])
            reports = [builder.convert((task, root, output / "audit", False)) for task in tasks]
            self.assertTrue(all(r["result"] == "PASS" for r in reports), reports)
            manifest = output / "manifest.tsv"
            self.assertEqual(builder.publish_manifest(manifest, reports, output), 2)
            lines = [line.split("\t") for line in manifest.read_text().splitlines()]
            self.assertEqual([line[0] for line in lines], [source_hash, source_hash])
            self.assertEqual([line[1] for line in lines], ["0", "1"])
            self.assertEqual(lines[0][2:4], ["16", "12"])

    def test_half_opacity_rounding_is_corrected_with_bounded_alpha_only(self):
        with tempfile.TemporaryDirectory(prefix="yosuga-hd-") as folder:
            root = Path(folder)
            original = Image.new("RGBA", (24, 24), (0, 0, 0, 0))
            ImageDraw.Draw(original).rectangle((5, 5, 18, 18), fill=(240, 160, 80, 128))
            original.save(root / "source.png")
            report = hd.process_asset({"id": "half", "source": "source.png", "output": "hd.png",
                                       "alpha_mode": "straight", "scale": 4}, root)
            self.assertEqual(report["result"], "PASS", report)
            self.assertGreater(report["alpha_quantization_corrections"], 0)
            self.assertLessEqual(report["alpha_quantization_max_byte_change"], 16)
            np.testing.assert_array_equal(np.asarray(Image.open(root / "source.png")), np.asarray(original))


if __name__ == "__main__":
    unittest.main()
