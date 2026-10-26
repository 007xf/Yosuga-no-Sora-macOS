"""Build the 『空澄』 interface as override archives (frame.noa, title.noa).

Every original non-XML NOA entry, including all click masks, is preserved
byte-for-byte. New textures are added as PNG entries and the skin XML points
at them; the only other XML changes are the typography/layout edits declared
in sky_ui (each asserted). The 4x masters are written next to the build so
the HD pipeline can publish them as the textures' high-resolution companions.
"""
from pathlib import Path
import hashlib
import io
import json
import re
import sys

from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "tools/noa"))
import noa  # noqa: E402
import erisan  # noqa: E402
import sky_ui  # noqa: E402


def decode(entry):
    data = entry.payload()
    return erisan.decode(data, entry.size) if entry.encoding == noa.ERISAN else data[:entry.size]


def png_bytes(im):
    raw = io.BytesIO()
    im.save(raw, format="PNG")
    return raw.getvalue()


def redirect(xml, old, new):
    token = f'src="{old}"'
    if xml.count(token) != 1:
        raise ValueError(f"expected one resource reference for {old}")
    return xml.replace(token, f'src="{new}"', 1)


def build_archive(source, target, masters, expected_sizes, xml_edit=None, extra_entries=(), xml_transform=None,
                  work=None, new_textures=None):
    """Write target: source + reduced masters (+extra PNG entries) with a redirected skin.

    masters: archive texture name -> 4x master; expected_sizes: name -> logical size.
    xml_transform(xml) may add resources/buttons (applied before redirection).
    new_textures: PNG entry name -> 4x master for textures the transform references
    directly (reduced and published as HD companions like the redirected ones).
    """
    header, entries = noa.read(source)
    skin = next(e for e in entries if e.name == "system.inf")
    original_xml = decode(skin).decode("cp932")
    xml = xml_transform(original_xml) if xml_transform else original_xml
    if xml_edit:
        xml = xml_edit(xml)
    edited_xml = xml
    added, report = [], {}
    work = Path(work) if work else None
    for old, master in masters.items():
        size = expected_sizes[old]
        if master.size != (size[0] * sky_ui.S, size[1] * sky_ui.S):
            raise ValueError(f"{old}: master {master.size} is not 4x {size}")
        im = sky_ui.reduce(master, size)
        new = "sky_" + Path(old).stem + ".png"
        xml = redirect(xml, old, new)
        data = png_bytes(im)
        added.append(noa.Entry.stored(new, data, skin.filetime))
        digest = hashlib.sha256(data).hexdigest()
        report[old] = {"entry": new, "size": list(size), "sha256": digest}
        if work:
            (work / "png").mkdir(parents=True, exist_ok=True)
            (work / "masters").mkdir(parents=True, exist_ok=True)
            im.save(work / "png" / new)
            master.save(work / "masters" / (digest + ".png"))
    for name, master in (new_textures or {}).items():
        if master.width % sky_ui.S or master.height % sky_ui.S:
            raise ValueError(f"{name}: master {master.size} is not a 4x size")
        size = (master.width // sky_ui.S, master.height // sky_ui.S)
        im = sky_ui.reduce(master, size)
        data = png_bytes(im)
        added.append(noa.Entry.stored(name, data, skin.filetime))
        digest = hashlib.sha256(data).hexdigest()
        report[name] = {"entry": name, "size": list(size), "sha256": digest}
        if work:
            (work / "png").mkdir(parents=True, exist_ok=True)
            (work / "masters").mkdir(parents=True, exist_ok=True)
            im.save(work / "png" / name)
            master.save(work / "masters" / (digest + ".png"))
    for name, data in extra_entries:
        added.append(noa.Entry.stored(name, data, skin.filetime))
    # Only resource redirection may differ from the declared edits.
    strip = lambda value: re.sub(r'src="sky_([^"]+)\.png"', r'src="\1.eri"', value)
    if strip(xml) != edited_xml:
        raise AssertionError("Unexpected XML change beyond redirection and declared edits")
    result = [e for e in entries if e.name != "system.inf"] + added + \
             [noa.Entry.stored("system.inf", xml.encode("cp932"), skin.filetime)]
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    noa.write(target, header, result)
    _, readback = noa.read(target)
    by_name = {e.name: e for e in readback}
    for entry in entries:
        if entry.name != "system.inf" and by_name[entry.name].record != entry.record:
            raise AssertionError("Original entry changed: " + entry.name)
    for entry in added:
        if by_name[entry.name].payload() != entry.payload():
            raise AssertionError("PNG read-back mismatch: " + entry.name)
    if work:
        (work / "system.utf8.xml").write_text(xml, encoding="utf-8")
        (work / "validation.json").write_text(json.dumps({
            "source": str(source), "output": str(target),
            "source_sha256": hashlib.sha256(Path(source).read_bytes()).hexdigest(),
            "output_sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
            "textures": report,
            "validation": "NOA round trip exact; original non-XML entries unchanged; XML differs only by "
                          "declared sky_ui edits and resource redirection; masters are exact 4x",
        }, ensure_ascii=False, indent=2), encoding="utf-8")
    return target


def original_size(game, kind, name):
    return Image.open(ROOT / "design/assets" / game / kind / (Path(name).stem + ".png")).size


def build(game_dir, game, out_dir):
    """The message/system skin: <out>/<game>/frame.noa."""
    masters = sky_ui.frame_masters(game)
    sizes = {name: sky_ui.RESIZED.get(name, original_size(game, "frame", name)) for name in masters}
    return build_archive(Path(game_dir) / "system/frame.noa", Path(out_dir) / game / "frame.noa",
                         masters, sizes, xml_edit=sky_ui.frame_xml,
                         work=Path(out_dir) / "work/frame" / game)


def build_title(game_dir, game, out_dir, xml_transform=None, extra_entries=(), extra_textures=None):
    """The title menu: <out>/<game>/title.noa with the 攻略进度 entry (Yosuga also gains Haruka's).

    xml_transform runs after sky_ui.title_xml.
    """
    names = sky_ui.TITLE_BUTTONS[game]
    sizes = {name: sky_ui.TITLE_RESIZED[game].get(name, original_size(game, "title", name)) for name in names}
    masters = sky_ui.title_masters(game, sizes)
    textures, masks = sky_ui.title_new_textures(game)
    textures.update(extra_textures or {})
    extra = list(extra_entries) + [(name, png_bytes(im)) for name, im in masks.items()]

    def transform(xml):
        xml = sky_ui.title_xml(game, xml)
        return xml_transform(xml) if xml_transform else xml

    return build_archive(Path(game_dir) / "system/title.noa", Path(out_dir) / game / "title.noa",
                         masters, sizes, xml_transform=transform, extra_entries=extra,
                         work=Path(out_dir) / "work/title" / game, new_textures=textures)


def hd_original(entry):
    """The accepted 4x companion of an original archive texture, when the HD set is present."""
    manifest = ROOT / "hd/bulk/manifest.tsv"
    if not manifest.exists():
        return None
    key = hashlib.sha256(decode(entry)).hexdigest()
    for line in manifest.read_text(encoding="utf-8").splitlines():
        if line.startswith(key + "\t0\t"):
            return Image.open(ROOT / "hd/bulk" / line.split("\t")[4]).convert("RGBA")
    return None


def build_option(game_dir, game, out_dir):
    """The appreciation screens: <out>/<game>/option.noa."""
    source = Path(game_dir) / "system/option.noa"
    _, entries = noa.read(source)
    by_name = {e.name: e for e in entries}
    names = ["FRM_0702.eri", "FRM_0731.eri", "FRM_0741.eri", "FRM_0732.eri", "FRM_0734.eri", "FRM_0733.eri",
             "FRM_0735.eri", "FRM_0742.eri", "FRM_0743.eri", "FRM_0751.eri", "FRM_0752.eri", "FRM_0119.eri"]
    names += list(sky_ui.OPTION_TAGS[game]) + [n for n in sky_ui.PAGE_TABS if n in by_name]
    sizes = {name: original_size(game, "option", name) for name in names}
    faces = {}
    x0, y0, x1, y1 = sky_ui.TAG_FACE
    for name in sky_ui.OPTION_TAGS[game]:
        hd = hd_original(by_name[name])
        if hd is not None:
            faces[name] = hd.crop(tuple(v * sky_ui.S for v in (x0, y0, x1, y1)))
        else:
            original = Image.open(ROOT / "design/assets" / game / "option" / (Path(name).stem + ".png")).convert("RGBA")
            faces[name] = original.crop((x0, y0, x1, y1)).resize(((x1 - x0) * sky_ui.S, (y1 - y0) * sky_ui.S),
                                                                  Image.Resampling.LANCZOS)
    masters = sky_ui.option_masters(game, sizes, faces)
    return build_archive(source, Path(out_dir) / game / "option.noa", masters, sizes,
                         work=Path(out_dir) / "work/option" / game)
