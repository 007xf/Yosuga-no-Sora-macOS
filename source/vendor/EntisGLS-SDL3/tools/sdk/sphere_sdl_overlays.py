"""SakuraGL overlays needed by Sphere's 2008-era skins (Yosuga no Sora, Haruka na Sora)."""


def _replace_once(text, before, after, what):
    if text.count(before) != 1:
        raise ValueError(f'SDK source changed: expected one {what}')
    return text.replace(before, after)


def resource_manager(text):
    # GLS3 skins embed another <page> as an <object> (the nine save/load slots
    # are <object style="ID_PAGE_SAVE_INDEX">). GLS3's EFormResourceManager
    # built them with ReadObjectTag; SGLSkinManager skipped the tag.
    return _replace_once(
        text,
        '\t\telse if ( pxmlTag->GetTag() == L"scroll_bar" )\n'
        '\t\t{\n'
        '\t\t\tpItem = CreateScrollBarItem( *pxmlTag ) ;\n'
        '\t\t}\n',
        '\t\telse if ( pxmlTag->GetTag() == L"scroll_bar" )\n'
        '\t\t{\n'
        '\t\t\tpItem = CreateScrollBarItem( *pxmlTag ) ;\n'
        '\t\t}\n'
        '\t\telse if ( pxmlTag->GetTag() == L"object" )\n'
        '\t\t{\n'
        '\t\t\tstatic int\tnObjectNest = 0 ;\n'
        '\t\t\tSString\tstrStyle = pxmlTag->GetAttrStringAs( L"style", L"" ) ;\n'
        '\t\t\tSXMLDocument *\tpxmlPage = GetFormAs( strStyle ) ;\n'
        '\t\t\tif ( (pxmlPage != NULL) && (nObjectNest < 16) )\n'
        '\t\t\t{\n'
        '\t\t\t\tSGLSpriteFormed *\tpObject = new SGLSpriteFormed ;\n'
        '\t\t\t\tnObjectNest ++ ;\n'
        '\t\t\t\tif ( PrepareFormedPage( *pObject, *pxmlPage )\n'
        '\t\t\t\t\t|| AddFormedPageItems( *pObject, *pxmlPage ) )\n'
        '\t\t\t\t{\n'
        '\t\t\t\t\tdelete\tpObject ;\n'
        '\t\t\t\t\tpObject = NULL ;\n'
        '\t\t\t\t}\n'
        '\t\t\t\tnObjectNest -- ;\n'
        '\t\t\t\tpItem = pObject ;\n'
        '\t\t\t}\n'
        '\t\t}\n',
        'scroll_bar item branch')


# Bit set by the port on plain script sprites (Sprite objects that are not
# windows, buttons or text). Must match kLegacyMousePassThrough in
# native/runtime/cotopha_port/legacy_sprite.cpp.
LEGACY_MOUSE_PASS_THROUGH = '0x0000000100000000ULL'


def sprite(text):
    # GLS3 delivered the mouse only to sprites that handle input. A plain
    # sprite above a dialog (Confirm()'s full-screen sprBlack shares the
    # dialog's priority and lands on top) let clicks reach the buttons below.
    # GLS4 stops at any sprite with pixels; keep that for other sprites.
    return _replace_once(
        text,
        '\t\t\tS2DDVector\tvLocal = vPos ;\n'
        '\t\t\tpChild->GlobalToLocal( vLocal ) ;\n'
        '\t\t\tif ( pChild->IsHitSprite( vLocal.x, vLocal.y ) )\n'
        '\t\t\t{\n'
        '\t\t\t\tvPos = vLocal ;\n'
        '\t\t\t\treturn\tpChild ;\n'
        '\t\t\t}',
        '\t\t\tS2DDVector\tvLocal = vPos ;\n'
        '\t\t\tpChild->GlobalToLocal( vLocal ) ;\n'
        '\t\t\tif ( pChild->IsHitSprite( vLocal.x, vLocal.y ) )\n'
        '\t\t\t{\n'
        f'\t\t\t\tif ( (pChild->m_flagsUI & {LEGACY_MOUSE_PASS_THROUGH})\n'
        '\t\t\t\t\t&& (pChild->m_refMouseListener.GetReference() == NULL) )\n'
        '\t\t\t\t{\n'
        '\t\t\t\t\tS2DDVector\tvInner = vLocal ;\n'
        '\t\t\t\t\tif ( pChild->GetHitSpriteAt( vInner ) == NULL )\n'
        '\t\t\t\t\t{\n'
        '\t\t\t\t\t\tif ( pChild->m_flagsUI & uiModalEnd )\n'
        '\t\t\t\t\t\t{\n'
        '\t\t\t\t\t\t\tbreak ;\n'
        '\t\t\t\t\t\t}\n'
        '\t\t\t\t\t\tcontinue ;\n'
        '\t\t\t\t\t}\n'
        '\t\t\t\t}\n'
        '\t\t\t\tvPos = vLocal ;\n'
        '\t\t\t\treturn\tpChild ;\n'
        '\t\t\t}',
        'GetHitSpriteAt child hit')


def archive_filename(text):
    # 2008-era Sphere archives store CP932 (Shift-JIS) file names, while the
    # SDK converts a wide name to UTF-8 before comparing against the raw
    # directory bytes. Every ASCII name matches either way, so the two
    # non-ASCII entries of Haruka's wallpaper skin (\
    # 'FRM_0801@調整版2.eri' and 'もっつん.eri') silently failed to open and
    # WallPaperViewer crashed on the unregistered image. Retry the lookup with
    # the encoding the original containers actually use.
    before = (
        '\tSArray<uint8_t>\tbufFilename ;\n'
        '\tCharset::Encode( bufFilename, Charset::encodingUTF8, pwszFilename ) ;\n'
        '\tbufFilename.Add( 0 ) ;\n'
        '\t//\n'
        '\treturn\tGetFileInfoAs( bufFilename.GetConstArray() ) ;\n')
    after = (
        '\tSArray<uint8_t>\tbufFilename ;\n'
        '\tCharset::Encode( bufFilename, Charset::encodingUTF8, pwszFilename ) ;\n'
        '\tbufFilename.Add( 0 ) ;\n'
        '\t//\n'
        '\tFileReferenceInfo *\tpfriInfo = GetFileInfoAs( bufFilename.GetConstArray() ) ;\n'
        '\tif ( pfriInfo != NULL )\n'
        '\t{\n'
        '\t\treturn\tpfriInfo ;\n'
        '\t}\n'
        '\t//\n'
        '\t// Sphere 2008 archives keep CP932 names; a UTF-8 comparison can\n'
        '\t// never match their non-ASCII entries. Retry with the archive\n'
        '\t// encoding before reporting the file as missing.\n'
        '\t//\n'
        '\tSArray<uint8_t>\tbufShiftJIS ;\n'
        '\tCharset::Encode( bufShiftJIS, Charset::encodingShiftJIS, pwszFilename ) ;\n'
        '\tbufShiftJIS.Add( 0 ) ;\n'
        '\treturn\tGetFileInfoAs( bufShiftJIS.GetConstArray() ) ;\n')
    return _replace_once(text, before, after, 'archive filename lookup')


def apply(source_copy):
    source_copy('Source/common/sakuraglx/sprite/sglx_resource_manager.cpp', resource_manager)
    source_copy('Source/common/sakuraglx/sprite/sglx_sprite.cpp', sprite)
    source_copy('Source/common/sakuragl/erisa/sgl_erisa_archive_file.cpp', archive_filename)
