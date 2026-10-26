"""Restore GLS3 behaviour that 2008-era Cotopha images (Yosuga no Sora, Haruka na Sora) rely on."""

from pathlib import Path


def _replace_once(text: str, before: str, after: str, what: str) -> str:
    if text.count(before) != 1:
        raise ValueError(f'Expected exactly one {what}')
    return text.replace(before, after)


def patch_source(relative_path: str, text: str) -> str:
    name = Path(relative_path).name
    if name == 'glscsobj_structure.cpp':
        # Older runtimes replaced a member that a constructor defines twice
        # (AdvScreen::AdvScreen declares fZoom twice). The SDK kept that
        # assignment as a comment when it began rejecting duplicates.
        text = _replace_once(
            text,
            '//\t\tm_varArray.SetAt( i, pObj ) ;\n\t\treturn\tESLErrorMsg(',
            '\t\tm_varArray.SetAt( i, pObj ) ;\n\t\treturn\teslErrSuccess ;\n\t\treturn\tESLErrorMsg(',
            'commented duplicate-member replacement')
    elif name == 'glsscriptobj.h':
        # ECSStrTagArray::LoadArray fills only the base name table, so a
        # dynamic structure read from system.dat has names but an empty
        # m_lstStrTagBuf. Copying it then dropped every member name, and
        # sysObj.bookmark[i].fSave became undefined after a restart.
        # Copy what the source table actually holds.
        text = _replace_once(
            text,
            '\t\t\tm_lstStrTagBuf = array.m_lstStrTagBuf ;\n'
            '\t\t\tm_idxSorted = array.m_idxSorted ;\n'
            '\t\t\t//\n'
            '\t\t\tunsigned int\ti, nCount ;\n'
            '\t\t\tnCount = m_lstStrTagBuf.GetSize() ;\n'
            '\t\t\tSetSize( nCount ) ;\n'
            '\t\t\tfor ( i = 0; i < nCount; i ++ )\n'
            '\t\t\t{\n'
            '\t\t\t\tESLAssert( m_lstStrTagBuf.GetAt(i) != NULL ) ;\n'
            '\t\t\t\tSetAt( i, m_lstStrTagBuf.GetAt(i)->CharPtr() ) ;\n'
            '\t\t\t}\n'
            '\t\t\treturn\t*this ;\n',
            '\t\t\tif ( &array == this )\n'
            '\t\t\t\treturn\t*this ;\n'
            '\t\t\tRemoveAll( ) ;\n'
            '\t\t\tfor ( unsigned int i = 0; i < array.GetSize(); i ++ )\n'
            '\t\t\t{\n'
            '\t\t\t\tconst wchar_t *\tpwszName = array.GetAt( i ) ;\n'
            '\t\t\t\tif ( pwszName != NULL )\n'
            '\t\t\t\t\tAdd( pwszName ) ;\n'
            '\t\t\t\telse\n'
            '\t\t\t\t\tECSStrTagArray::Add( NULL ) ;\n'
            '\t\t\t}\n'
            '\t\t\treturn\t*this ;\n',
            'ECSStrBufTagArray copy assignment')
    return text
