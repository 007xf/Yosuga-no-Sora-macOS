"""Builds the override archives the merged app loads before the originals.

Yosuga no Sora's title.noa gains a 'ハルカナソラ' button (ID_HARUKA) under
the menu; the app switches to Haruka na Sora's own title when it is pressed.

usage: python3 build_mods.py <yosuga game dir> <haruka game dir> <out dir>
Writes both games' title.noa, frame.noa and option.noa overrides (the 空澄 interface).
Original files are only read.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools', 'noa'))
import noa  # noqa: E402
import erisan  # noqa: E402
import make_frame_skin  # noqa: E402
import make_logo_reference  # noqa: E402

CRLF = '\r\n'


def entry_bytes(entry):
    data = entry.payload()
    return erisan.decode(data, entry.size) if entry.encoding == noa.ERISAN else data[:entry.size]


def logo_png(haruka_dir, work):
    """Use the user's no-dark-outline, source-derived corrected reference.

    This deterministic correction takes precedence over generated HD layouts.
    It does not change or bypass the original HD review manifest or thresholds.
    """
    source = os.path.join(HERE, '..', '..', 'design', 'assets', 'haruka', 'title', 'FRM_0612.png')
    if os.path.exists(source):
        return make_logo_reference.ensure_reference(source)
    hd = os.path.join(HERE, '..', '..', 'hd', 'accepted', 'haruka_logo.png')
    if os.path.exists(hd):
        return hd
    png = os.path.join(work, 'haruka_FRM_0612.png')
    if not os.path.exists(png):
        source = os.path.join(HERE, '..', '..', 'design', 'assets', 'haruka', 'title', 'FRM_0612.png')
        if not os.path.exists(source):
            raise SystemExit('Run the ERI exporter first: missing ' + source)
        with open(source, 'rb') as src, open(png, 'wb') as dst:
            dst.write(src.read())
    return png


def add_haruka_entry(xml, width, height):
    # Resources and the button style.
    xml = xml.replace('\t</resource>', '\t\t<image id="ID_FRM_HARUKA" src="haruka_entry.png"/>' + CRLF +
                      '\t\t<image id="ID_FRM_HARUKA_" src="haruka_entry_mask.png"/>' + CRLF + '\t</resource>', 1)
    style = (f'\t\t<style id="ID_BTN_TITLE_HARUKA" type="button">{CRLF}'
             f'\t\t\t<arrange type="button"/>{CRLF}'
             f'\t\t\t<normal image="ID_FRM_HARUKA:RECT(0,0,{width},{height})"/>{CRLF}'
             f'\t\t\t<pushed image="ID_FRM_HARUKA:RECT({width},0,{width},{height})"/>{CRLF}'
             f'\t\t\t<focus image="ID_FRM_HARUKA:RECT({width},0,{width},{height})"/>{CRLF}'
             f'\t\t\t<mask image="ID_FRM_HARUKA_"/>{CRLF}'
             f'\t\t</style>{CRLF}')
    xml = xml.replace('\t</declare_style>', style + '\t</declare_style>', 1)
    # The menu pages are 178 wide; centre the wider logo under the buttons.
    # Unbuffered, so the logo is not clipped to the page rectangle.
    x = (178 - width) // 2
    for page in ('ID_PAGE_MENU', 'ID_PAGE_MENU_FULL'):
        start = xml.index(f'id="{page}"')
        open_tag = xml.rindex('<page', 0, start)
        close = xml.index('</page>', start)
        head_end = xml.index('>', start) + 1
        head = xml[open_tag:head_end]
        last_y = max(int(part.split('"')[0]) for part in xml[head_end:close].split(' y="')[1:])
        y = last_y + 28 + 20
        new_height = y + height
        old_height = head.split('height="')[1].split('"')[0]
        new_head = head.replace(f'height="{old_height}"', f'height="{new_height}"', 1)
        new_head = new_head.replace('<page ', '<page buffered="false" ', 1)
        button = (f'\t\t<button group="true" height="0" id="ID_HARUKA" style="ID_BTN_TITLE_HARUKA" '
                  f'width="0" x="{x}" y="{y}"/>{CRLF}')
        line_start = xml.rindex('\n', 0, close) + 1
        xml = xml[:open_tag] + new_head + xml[head_end:line_start] + button + xml[line_start:]
    return xml


def build_yosuga_title(yosuga_dir, haruka_dir, out_dir):
    logo = os.path.join(HERE, '..', '..', 'hd', 'reference', 'haruka_logo_no_outline@4x.png')
    if not os.path.exists(logo):
        logo_png(haruka_dir, os.path.join(out_dir, 'work'))  # writes the corrected reference
    master, (width, height) = make_frame_skin.sky_ui.haruka_entry_master(make_frame_skin.Image.open(logo))
    mask = make_frame_skin.png_bytes(make_frame_skin.Image.new('RGBA', (width, height), (255, 255, 255, 255)))
    return str(make_frame_skin.build_title(
        yosuga_dir, 'yosuga', out_dir,
        xml_transform=lambda xml: add_haruka_entry(xml, width, height),
        extra_entries=[(make_frame_skin.sky_ui.HARUKA_ENTRY_MASK, mask)],
        extra_textures={make_frame_skin.sky_ui.HARUKA_ENTRY: master}))


if __name__ == '__main__':
    print(build_yosuga_title(sys.argv[1], sys.argv[2], sys.argv[3]))
    print(make_frame_skin.build_title(sys.argv[2], 'haruka', sys.argv[3]))
    print(make_frame_skin.build(sys.argv[1], 'yosuga', sys.argv[3]))
    print(make_frame_skin.build(sys.argv[2], 'haruka', sys.argv[3]))
    print(make_frame_skin.build_option(sys.argv[1], 'yosuga', sys.argv[3]))
    print(make_frame_skin.build_option(sys.argv[2], 'haruka', sys.argv[3]))
