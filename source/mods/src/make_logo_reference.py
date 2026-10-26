"""Restore straight alpha in the original Haruka logo, without redrawing it.

The exporter copied premultiplied RGB into a straight-RGBA PNG. Interpreting
that file as ordinary RGBA multiplies its white antialias/shadow pixels twice,
creating dark fringes. This recipe corrects only that representation problem:
source alpha, geometry and every opaque RGB pixel remain exactly unchanged.

The 4x file is interpolation from original pixels, not generated detail. It is
an explicitly authorized corrected reference, not a PASS against the original
HD comparator; no check_hd threshold or manifest is modified.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import struct
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/"design/assets/haruka/title/FRM_0612.png"
REFERENCE=ROOT/"hd/reference"
NATIVE=REFERENCE/"haruka_logo_no_outline.png"
HD=REFERENCE/"haruka_logo_no_outline@4x.png"
REPORT=REFERENCE/"haruka_logo_no_outline.json"
# Optional review sheet destination (the earlier maintainer's evidence folder).
DELIVERY=Path(os.environ["LOGO_REVIEW_DIR"]) if os.environ.get("LOGO_REVIEW_DIR") else None


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def straight_rgba(premultiplied):
    a=premultiplied[...,3:4]
    rgb=np.divide(premultiplied[...,:3]*255.0,a,
                  out=np.zeros_like(premultiplied[...,:3]),where=a>0)
    rgba=np.concatenate((np.clip(rgb,0,255),np.clip(a,0,255)),axis=2)
    return Image.fromarray(np.rint(rgba).astype(np.uint8),"RGBA")


def ensure_reference(source=SOURCE):
    source=Path(source)
    before=digest(source)
    original=Image.open(source).convert("RGBA")
    if original.size!=(354,75):
        raise ValueError("Unexpected original Haruka logo dimensions")
    raw=np.asarray(original).astype(np.float32)
    visible=raw[...,3]>0
    if np.any(raw[...,:3]>raw[...,3:4]):
        raise ValueError("Source no longer has the audited premultiplied-alpha signature")
    native=straight_rgba(raw)
    corrected=np.asarray(native)
    if not np.array_equal(corrected[...,3],raw[...,3].astype(np.uint8)):
        raise AssertionError("Source alpha changed")
    opaque=raw[...,3]==255
    if not np.array_equal(corrected[opaque,:3],raw[opaque,:3].astype(np.uint8)):
        raise AssertionError("Opaque source RGB changed")
    # Resize premultiplied channels, then unpremultiply exactly once. This
    # avoids dark transparent texels bleeding into interpolated edge colours.
    planes=[]
    for c in range(4):
        plane=Image.fromarray(raw[...,c],"F").resize((1416,300),Image.Resampling.LANCZOS)
        planes.append(np.asarray(plane))
    expanded=np.stack(planes,axis=2)
    expanded[...,3]=np.clip(expanded[...,3],0,255)
    expanded[...,:3]=np.minimum(np.maximum(expanded[...,:3],0),expanded[...,3:4])
    hd=straight_rgba(expanded)
    REFERENCE.mkdir(parents=True,exist_ok=True)
    native.save(NATIVE)
    hd.save(HD)
    neutral=(raw[...,0]==raw[...,1])&(raw[...,1]==raw[...,2])&(raw[...,2]==raw[...,3])&visible
    eri_evidence=None
    archive=ROOT/"testgames/haruka/system/title.noa"
    if archive.exists():
        sys.path.insert(0,str(ROOT/"tools/noa"))
        from noa_list import read_dir,read_entry
        with archive.open("rb") as f:
            entry=next(e for e in read_dir(f) if e["name"]=="FRM_0612.eri")
            data=read_entry(f,entry)
        offset=data.index(b"ImageInf")
        format_flags=struct.unpack_from("<I",data,offset+16+12)[0]
        eri_evidence={"archive":str(archive),"entry":"FRM_0612.eri",
                      "format_flags":hex(format_flags),
                      "alpha_flag":bool(format_flags&0x04000000),
                      "no_product_of_alpha_flag":bool(format_flags&0x08000000)}
    report={
        "decision":"User authorized no dark outline; use source-derived straight-alpha correction",
        "method":"RGB/alpha unpremultiplication; 4x Lanczos interpolation in premultiplied space",
        "source":str(source),"source_sha256":before,
        "native":str(NATIVE),"native_sha256":digest(NATIVE),
        "four_times":str(HD),"four_times_sha256":digest(HD),
        "original_size":[354,75],"native_size":list(native.size),"four_times_size":list(hd.size),
        "visible_source_pixels":int(visible.sum()),
        "all_visible_pixels_have_RGB_le_alpha":True,
        "neutral_RGB_equals_alpha_pixels":int(neutral.sum()),
        "original_eri_format_evidence":eri_evidence,
        "native_alpha_exactly_equal":True,"opaque_RGB_exactly_equal":True,
        "source_file_unchanged":digest(source)==before,
        "old_HD_comparator":"NOT CLAIMED PASS: intentional corrected reference; original thresholds and manifest unchanged",
        "generated_candidate":"not used; hd/inbox/haruka_logo.png has incorrect source aspect ratio",
        "runtime_validation":"pending title driver screenshot from root task",
        "detail_limit":"Interpolation only; no newly recovered or invented detail"
    }
    REPORT.write_text(json.dumps(report,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    return str(HD)


def composite_logo(im,bg,size=(708,150)):
    canvas=Image.new("RGBA",size,bg)
    logo=im.resize(size,Image.Resampling.LANCZOS)
    canvas.alpha_composite(logo)
    return canvas.convert("RGB")


def render_comparison():
    original=Image.open(SOURCE).convert("RGBA")
    corrected=Image.open(HD).convert("RGBA")
    out=Image.new("RGB",(1600,990),"#EFF4F8")
    d=ImageDraw.Draw(out)
    face="/System/Library/Fonts/Hiragino Sans GB.ttc"
    def label(x,y,value,size=24,color="#28495F"):
        d.text((x,y),value,font=ImageFont.truetype(face,size),fill=color,anchor="lt")
    label(40,28,"ハルカナソラ · 去除深色边缘",38)
    label(40,87,"沿用原图校正 Alpha；字形、蓝白配色、位置与透明轮廓保持原样",23,"#647F92")
    label(40,152,"原始导出",27)
    label(840,152,"源像素校正",27)
    for y,bg,title in [(225,"#D8E6F0","浅色背景"),(440,"#17384E","深色背景")]:
        label(40,y-27,title,20,"#647F92")
        out.paste(composite_logo(original,bg),(40,y))
        out.paste(composite_logo(corrected,bg),(840,y))
    label(40,628,"缩到游戏按钮尺寸（这里以 2 倍显示）",24)
    # The title integration renders a 212x45 logo plus 8px padding each side.
    for x,im in [(40,original),(840,corrected)]:
        actual=im.resize((212,45),Image.Resampling.LANCZOS)
        cell=Image.new("RGBA",(228,61),"#A9CFDE")
        cell.alpha_composite(actual,(8,8))
        out.paste(cell.convert("RGB").resize((456,122),Image.Resampling.NEAREST),(x,684))
    label(40,850,"原尺寸 354×75  ·  四倍源 1416×300  ·  1x Alpha 逐像素一致",23,"#647F92")
    label(40,890,"四倍版本来自插值放大，没有重画或宣称新增细节；旧比较器不冒称通过。",22,"#647F92")
    if DELIVERY is None:
        return None
    DELIVERY.mkdir(parents=True,exist_ok=True)
    target=DELIVERY/"14 悠之空标志去黑边对比.png"
    out.save(target)
    shutil.copy2(HD,DELIVERY/"15 悠之空标志无黑边4倍.png")
    return target


def append_review():
    path=ROOT/"hd/REVIEW_LOG.md"
    tag="源像素反预乘校正"
    old=path.read_text(encoding="utf-8") if path.exists() else ""
    if tag not in old:
        line="2026-10-23 | haruka_logo | 采用显式校正参考 | 原尺寸354×75；Alpha逐像素一致；不透明RGB逐像素一致；4倍为插值放大 | "+tag+"，去除重复Alpha乘法造成的灰黑边；图生图候选未采用；不宣称旧比较器PASS | 用户已授权去黑边，其余自行定夺\n"
        with path.open("a",encoding="utf-8") as f:
            f.write(line)


if __name__=="__main__":
    print(ensure_reference())
    print(render_comparison())
    append_review()
