"""High-density image overlays; supplied SDK sources remain read-only inputs."""

from prepare_sdl_system import function_body, replace_body, replace_once

HEADER = '#include "platform/sdl/hd_images.h"\n'


def image(text):
    start, end = function_body(text, 'SGLSmartImage::NormalizeBufferFormat')
    body = text[start + 1:end - 1]
    body = '\n    auto* const hdOriginalBuffer = pImageBuf;\n' + body
    body = replace_once(body, '\tif ( nFlags & formatCastSize )\n',
        '\tif (pImageBuf != hdOriginalBuffer)\n'
        '\t\tstudy::platform::sdl::PreserveHDImageBuffer(hdOriginalBuffer, pImageBuf, false);\n'
        '\tif ( nFlags & formatCastSize )\n')
    text = replace_body(text, 'SGLSmartImage::NormalizeBufferFormat', body)
    # LoadImage's extension-selected decoder bypasses ReadImage; cover that
    # success branch as well as all stream/MIME based image loading.
    start, end = function_body(text, 'SGLImageObject::LoadImage')
    body = text[start + 1:end - 1]
    body = replace_once(body, '\t\t\t\treturn\tsglErrSuccess ;',
                        '\t\t\t\tstudy::platform::sdl::RegisterDecodedHDImage(*this, *file);\n'
                        '\t\t\t\treturn\tsglErrSuccess ;')
    text = replace_body(text, 'SGLImageObject::LoadImage', body)
    start, end = function_body(text, 'SGLImageObject::ReadImage')
    body = text[start + 1:end - 1]
    text = replace_body(text, 'SGLImageObject::ReadImage', '''
    const auto decode = [&]() -> SGLError {
''' + body + '''
    };
    const auto result = decode();
    if (!result && file) study::platform::sdl::RegisterDecodedHDImage(*this, *file);
    return result;
''')
    start, end = function_body(text, 'SGLImageObject::CreateCloneImage')
    body = text[start + 1:end - 1]
    text = replace_body(text, 'SGLImageObject::CreateCloneImage', '''
    const auto clone = [&]() -> SGLError {
''' + body + '''
    };
    const auto result = clone();
    if (!result) study::platform::sdl::CopyHDImageMetadata(imgObj, *this);
    return result;
''')
    start, end = function_body(text, 'SGLImageObject::CopyImage')
    body = text[start + 1:end - 1]
    text = replace_body(text, 'SGLImageObject::CopyImage', '''
    const auto copy = [&]() -> SGLError {
''' + body + '''
    };
    const auto result = copy();
    if (!result && pSrcImage)
        study::platform::sdl::CopyHDImageRegionMetadata(*pSrcImage, *this, xDst, yDst, pSrcRect);
    return result;
''')
    return HEADER + text


def image_buffer(text):
    # Normalization copies visible pixels into a new POT/pitch-aligned root and
    # then collapses references onto it. Transfer metadata *before* old storage
    # is released, retaining the logical crop and the corresponding HD origin.
    anchor = '\t\tpImage->NotifyReBufferedImage() ;\n'
    text = replace_once(text, anchor,
        '\t\tstudy::platform::sdl::PreserveHDImageBuffer(pImage, pNormalized);\n' + anchor)
    return HEADER + text


def paint_buffer(text):
    start, end = function_body(text, 'SGLPaintBuffer::AttachTargetImage')
    body = text[start + 1:end - 1]
    body = replace_once(body, '\tm_pbytTarget =\n',
        '\tstudy::platform::sdl::BeginHDCPUCanvas(*this, m_pTarget, m_pZBuffer, m_rctView);\n'
        '\tif (m_opFillType != fillFlat) study::platform::sdl::InvalidateHDCPUCanvas(*this);\n'
        '\tm_pbytTarget =\n')
    text = replace_body(text, 'SGLPaintBuffer::AttachTargetImage', body)
    start, end = function_body(text, 'SGLPaintBuffer::DetachTargetImage')
    body = text[start + 1:end - 1]
    body = replace_once(body, '\tm_flagUpdateTarget = false ;\n',
        '\tstudy::platform::sdl::EndHDCPUCanvas(*this);\n\tm_flagUpdateTarget = false ;\n')
    text = replace_body(text, 'SGLPaintBuffer::DetachTargetImage', body)
    for method, statement in (
        ('FillClearTarget', 'MirrorHDCPUClear(*this, argb, flags)'),
        ('FillPolygon', 'MirrorHDCPUShape(*this, 0, vertices, count, argb, z, flags)'),
        ('DrawPoints', 'MirrorHDCPUShape(*this, 1, pPoints, nPoints, argb, z, flags)'),
        ('DrawThinLines', 'MirrorHDCPUShape(*this, 2, pLines, nLines, argb, z, flags)'),
        ('FreeGradation', 'MirrorHDCPUGradient(*this, 0, 0, 0, 0, 0, nullptr, 0)'),
        ('SetLinearGradation', 'MirrorHDCPUGradient(*this, 1, x0, y0, x1, y1, pGradation, nCount)'),
        ('SetRingedGradation', 'MirrorHDCPUGradient(*this, 2, xCenter, yCenter, radAngle, 0, pGradation, nCount)'),
    ):
        start, end = function_body(text, 'SGLPaintBuffer::' + method)
        body = text[start + 1:end - 1]
        text = replace_body(text, 'SGLPaintBuffer::' + method,
            '\n    study::platform::sdl::' + statement + ';\n' + body)
    start, end = function_body(text, 'SGLPaintBuffer::DrawImage')
    body = text[start + 1:end - 1]
    body = '\n    study::platform::sdl::MirrorHDCPUImage(*this, ppPaint, pSrcImage, pSrcClip);\n' + body
    body = replace_once(body, '\tSGLImageInfo\tinfSource ;\n',
        '\tstudy::platform::sdl::TraceHDImageOperation("cpu.DrawImage", pSrcImage, m_pTarget, pSrcClip, &afTransform);\n'
        '\tSGLImageInfo\tinfSource ;\n')
    text = replace_body(text, 'SGLPaintBuffer::DrawImage', body)
    # Never publish a mirror if the logical operation itself failed.
    for method in ('DrawImage', 'FillClearTarget', 'FillPolygon', 'DrawPoints', 'DrawThinLines',
                   'SetLinearGradation', 'SetRingedGradation'):
        start, end = function_body(text, 'SGLPaintBuffer::' + method)
        body = text[start + 1:end - 1]
        text = replace_body(text, 'SGLPaintBuffer::' + method, '''
    const auto logicalOperation = [&]() -> SGLError {
''' + body + '''
    };
    const auto result = logicalOperation();
    if (result) study::platform::sdl::InvalidateHDCPUCanvas(*this);
    return result;
''')
    return HEADER + text


def _draw_entries(text, owner):
    for method, call in (
        ('DrawImage', 'DrawHDImage(*this, ppPaint, pSrcImage, pSrcClip, hdResult)'),
        ('DrawMesh', 'DrawHDMesh(*this, pDstMesh, pSrcMesh, widthMesh, heightMesh, ppPaint, pSrcImage, pSrcClip, hdResult)'),
    ):
        start, end = function_body(text, owner + '::' + method)
        body = text[start + 1:end - 1]
        text = replace_body(text, owner + '::' + method, '''
    SGLError hdResult;
    if (study::platform::sdl::''' + call + ''') return hdResult;
''' + body)
    return text


def buffered(text):
    text = _draw_entries(text, 'S3DRenderBufferedContext')
    start, end = function_body(text, 'S3DRenderBufferedContext::DrawMultiImages')
    body = text[start + 1:end - 1]
    # Skin/form draw lists batch atlas references and otherwise bypass DrawImage.
    # Only split batches containing an actual companion; retain the SDK's normal
    # batching for untouched textures and preserve the original submission order.
    text = replace_body(text, 'S3DRenderBufferedContext::DrawMultiImages', '''
    if (pParams && ppSrcImages && study::platform::sdl::HDImagesEnabled()) {
        bool hdBatch = false;
        for (size_t i = 0; i < nCount && !hdBatch; ++i) {
            study::platform::sdl::HDImageView view;
            hdBatch = study::platform::sdl::ResolveHDImage(ppSrcImages[i], view);
        }
        if (hdBatch) {
            SGLError result = sglErrSuccess;
            for (size_t i = 0; i < nCount; ++i) {
                result = DrawImage(pParams[i], ppSrcImages[i], pSrcClips ? pSrcClips + i : nullptr);
                if (result) return result;
            }
            return result;
        }
    }
''' + body)
    return HEADER + text


def opengl_render(text):
    text = _draw_entries(text, 'S3DOpenGLDirectlyRenderer')
    # Release only after a synchronous Finish consumed the queued materials.
    # Flush alone can defer work to the GL thread and is not a safe lifetime.
    for owner in ('S3DOpenGLBufferedRenderer', 'S3DOpenGLDirectlyRenderer'):
        name = owner + '::Finish'
        start, end = function_body(text, name)
        body = text[start + 1:end - 1]
        text = replace_body(text, name, '''
    const auto finish = [&]() -> SGLError {
''' + body + '''
    };
    const auto result = finish();
    if (!result) study::platform::sdl::FinishHDRenderer(this);
    return result;
''')
    return HEADER + text


def opengl_context(text):
    text = replace_once(text,
        '\t\tstrFragSrc += pFragmentSrc[i].DecodeSource() ;\n\t}\n',
        '\t\tstrFragSrc += pFragmentSrc[i].DecodeSource() ;\n\t}\n'
        '\tstudy::platform::sdl::PatchHDFragmentSource(strFragSrc);\n')
    start, end = function_body(text, 'SGLOpenGLDefaultShader::BindTexture')
    body = text[start + 1:end - 1]
    anchor = '\t\t//\n\t\tSetGLTextureParameter\n'
    body = replace_once(body, anchor, '''
        // The four-tap cubic fallback reads the current texture (including
        // video/CPU changes). Static 4x companions keep their original sampler.
        const GLint hdLocation = glGetUniformLocation(m_glProgram, "u_vHDResample");
        if (hdLocation >= 0) {
            const bool cubic = study::platform::sdl::HDImagesEnabled()
                && !study::platform::sdl::IsHDPhysicalImage(pTextureImage)
                && m_fEnableTextureSmoothing && pglTexture->m_flagSmoothable
                && (flagsShading & shadingTextureSmoothing);
            glUniform3f(hdLocation, 1.0f / pglTexture->m_imginf.width,
                        1.0f / pglTexture->m_imginf.height, cubic ? 1.0f : 0.0f);
            study::platform::sdl::TraceHDSampler(pTextureImage, pglTexture->m_imginf.width,
                                               pglTexture->m_imginf.height, cubic, true);
        } else {
            study::platform::sdl::TraceHDSampler(pTextureImage, pglTexture->m_imginf.width,
                                               pglTexture->m_imginf.height, false, false);
        }
''' + anchor)
    text = replace_body(text, 'SGLOpenGLDefaultShader::BindTexture', body)
    return HEADER + text


def sprite_text(text):
    # Static text (names, choices, save slots, confirmations) is lettered at
    # the logical size; publish the same lettering at HD density beside it.
    start, end = function_body(text, 'SGLSpriteText::CreateTextImage')
    body = text[start + 1:end - 1]
    body = replace_once(body, '\tletterer.WriteLetter( font, context, pwszText ) ;\n',
        '\tletterer.WriteLetter( font, context, pwszText ) ;\n'
        '\tconst auto hdPlan = study::platform::sdl::PrepareHDText( letterer, style.font, style.decoration ) ;\n')
    body = replace_once(body, '\tpImage->UnlockBuffer() ;\n\t//\n\treturn\tpImage ;',
        '\tpImage->UnlockBuffer() ;\n'
        '\tstudy::platform::sdl::AttachHDText( *pImage, hdPlan ) ;\n\t//\n\treturn\tpImage ;')
    text = replace_body(text, 'SGLSpriteText::CreateTextImage', body)
    return '#include "platform/sdl/hd_text.h"\n' + text


def apply(source_copy):
    source_copy('Source/common/sakuraglx/sprite/sglx_sprite_text.cpp', sprite_text)
    source_copy('Source/common/sakuragl/sgl2d/sgl_image.cpp', image)
    source_copy('Source/common/sakuragl/sgl2d/sgl_image_buffer.cpp', image_buffer)
    source_copy('Source/common/sakuragl/sgl2d/sgl_paint_buffer.cpp', paint_buffer)
    source_copy('Source/common/sakuragl/sgl3d/sgl_render_buffered_context.cpp', buffered)
