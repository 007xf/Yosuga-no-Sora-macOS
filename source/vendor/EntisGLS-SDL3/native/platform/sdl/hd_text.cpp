#include "platform/sdl/hd_text.h"
#include "platform/sdl/hd_images.h"
#include <sakuragl/sgl2d/sgl_image_buffer.h>
#include <sakuragl/sgl2d/sgl_image_conversion.h>

#include <SDL3/SDL.h>
#include <exception>
#include <list>
#include <string>

namespace study::platform::sdl {
namespace {
using namespace SakuraGL;
constexpr int density = 4;
bool Debugging(){static const bool on=[]{const char* v=SDL_getenv("ENTISGLS_HD_DEBUG");return v&&*v&&*v!='0';}();return on;}

// The SDK letterer itself, run at density: its rasterizer, white-ARGB glyph
// convention and decorator (body colour, gradation, borders, shadow) are the
// very code that produced the logical text, so only the scale differs.
class DenseLetterer final : public SGLLetterer {
public:
    // Returns one rasterized character (caller releases), at its own offset.
    SGLImageBuffer* Glyph(SGLFont& font, const Decoration* decoration, uint32_t code, SGLPoint& offset) {
        Character character{};
        SSystem::SArray<uint8_t> gray;
        SGLLetterer::RasterizeCharacter(font, character, gray, wchar_t(code));
        if (!character.pImage) return nullptr;
        if (decoration) DoDecoration(*decoration, character);
        if (character.pImage && character.pImage->format == formatImageGray) NormalizeCharacterGrayToARGB(character);
        offset = character.ptOffset;
        return character.pImage;
    }
    void Decorate(const Decoration& decoration, SGLImageBuffer*& image, const SGLPoint& offset, const SGLSize& size) {
        Character character{};
        character.pImage = image;
        character.ptOffset = offset;
        character.sizeChar = size;
        DoDecoration(decoration, character);
        image = character.pImage;
    }
};

SGLLetterer::Decoration Dense(const SGLLetterer::Decoration& logical) {
    auto dense = logical;
    dense.widthBorder *= density;
    dense.widthBorder2 *= density;
    dense.ptShadow.x *= density;
    dense.ptShadow.y *= density;
    dense.nGradationHeight *= density;
    return dense;
}

// Fonts own a FreeType face each; text is lettered on the script thread in
// short bursts of a few styles, so keep the recent ones.
SGLFont* CachedFont(const SGLFontStyle& style, unsigned scale) {
    if (!style.pszFace || !style.nSize || style.nSize * scale > 4096) return nullptr;
    struct Entry { std::wstring face; uint32_t size, styles; std::unique_ptr<SGLFont> font; };
    thread_local std::list<Entry> cache;
    const uint32_t size = style.nSize * scale;
    for (auto at = cache.begin(); at != cache.end(); ++at)
        if (at->size == size && at->styles == style.nStyles && at->face == style.pszFace) {
            cache.splice(cache.begin(), cache, at);
            return cache.front().font.get();
        }
    Entry entry{style.pszFace, size, style.nStyles, std::make_unique<SGLFont>()};
    SGLFontStyle scaled = style;
    scaled.nSize = size;
    if (entry.font->SetStyle(scaled) != sglErrSuccess) return nullptr;
    cache.push_front(std::move(entry));
    if (cache.size() > 8) cache.pop_back();
    return cache.front().font.get();
}

// The glyph's own raster offset at the logical size. The letterer adds line
// alignment on top of it (Character::ptOffset); that part is a layout offset
// and scales by density like the writing position.
bool RawOffset(const SGLFontStyle& style, uint32_t code, SGLPoint& offset) {
    auto* font = CachedFont(style, 1);
    SGLFontMetrics metrics{};
    if (!font || font->GetMetrics(nullptr, 0, metrics, code) != sglErrSuccess) return false;
    offset = SGLPoint(metrics.rctExterior.x, metrics.rctExterior.y);
    return true;
}

SGLImageBuffer* Canvas(uint32_t width, uint32_t height) {
    SGLImageInfo info;
    info.format = formatImageARGB;
    info.depth = 32;
    info.width = width;
    info.height = height;
    info.pitchPixel = 4;
    info.pitchLine = width * 4;
    auto* canvas = sglCreateImageBuffer(info);
    if (canvas) sglFillImageBuffer(*canvas, SGLPalette(0));
    return canvas;
}

// Publishes `dense` (placed at x, y) as the companion of the logical image.
void Publish(SGLImageObject& logical, const SGLImageBuffer& dense, int x, int y, const char* key) {
    SGLImageInfo info;
    if (logical.GetImageInfo(info) || !info.width || !info.height) return;
    auto physical = std::make_shared<SGLImage>();
    if (physical->CreateImage(info.width * density, info.height * density, formatImageARGB, 32)) return;
    SGLImageBuffer target;
    target.ptrBuffer = physical->LockBuffer(target, SGLImageObject::lockWrite);
    if (!target.ptrBuffer) return;
    sglFillImageBuffer(target, SGLPalette(0));
    sglCopyImageBuffer(target, dense, x, y);
    physical->UnlockBuffer(SGLImageObject::lockWrite);
    AttachHDCompanion(logical, std::move(physical), key);
}
} // namespace

struct HDTextPlan {
    SGLImageBuffer* image = nullptr;
    ~HDTextPlan() { if (image) sglReleaseImageBuffer(image); }
};

void AttachHDGlyph(SGLImageObject& glyph, const SGLFontStyle& style,
                   const SGLLetterer::Decoration& decoration, uint32_t code,
                   const SGLPoint& logicalOffset) try {
    if (!HDImagesEnabled()) return;
    if (Debugging()) {
        static uint32_t logged[8] = {};
        static unsigned count = 0;
        bool seen = false;
        for (unsigned i = 0; i < count; ++i) seen = seen || logged[i] == (style.nSize << 8 | decoration.nFlags);
        if (!seen && count < 8) logged[count++] = style.nSize << 8 | decoration.nFlags;
        if (!seen)
            SDL_Log("HD glyph decoration size=%u styles=%x flags=%x body=%08x border=%08x/%u shadow=%08x@%d,%d",
                    style.nSize, style.nStyles, decoration.nFlags, decoration.rgbaBody.ui32, decoration.rgbaBorder.ui32,
                    decoration.widthBorder, decoration.rgbaShadow.ui32, decoration.ptShadow.x, decoration.ptShadow.y);
    }
    auto* font = CachedFont(style, density);
    SGLPoint raw;
    if (!font || !RawOffset(style, code, raw)) return;
    DenseLetterer letterer;
    const auto dense = Dense(decoration);
    SGLPoint offset;
    auto* image = letterer.Glyph(*font, &dense, code, offset);
    if (!image) return;
    // The logical image starts at its raster offset (plus layout, which both
    // copies share), so the dense copy is placed relative to density x raw.
    Publish(glyph, *image, offset.x - raw.x * density, offset.y - raw.y * density, "derived:hd-glyph");
    sglReleaseImageBuffer(image);
} catch (const std::exception& error) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "HD glyph: %s", error.what());
}

std::shared_ptr<HDTextPlan> PrepareHDText(const SGLLetterer& letters, const SGLFontStyle& style,
                                          const SGLLetterer::Decoration& decoration) try {
    if (!HDImagesEnabled()) return {};
    if (Debugging())
        SDL_Log("HD text decoration size=%u styles=%x flags=%x body=%08x border=%08x/%u shadow=%08x@%d,%d",
                style.nSize, style.nStyles, decoration.nFlags, decoration.rgbaBody.ui32, decoration.rgbaBorder.ui32,
                decoration.widthBorder, decoration.rgbaShadow.ui32, decoration.ptShadow.x, decoration.ptShadow.y);
    // The same rectangles SGLLetterer::CombineLetter unites.
    SGLRect rect(0, 0, -1, -1), outer(0, 0, -1, -1);
    for (size_t i = 0; i < letters.GetLetterLength(); ++i) {
        const auto* character = letters.GetCharacterAt(i);
        if (!character || !character->pImage) continue;
        const SGLRect glyph = character->pImage->GetImageRect() + character->ptWriting + character->ptOffset;
        const SGLRect cell = SGLImageRect(character->ptWriting, character->sizeChar);
        if (rect.IsEmpty()) { rect = glyph; outer = cell; }
        else { rect |= glyph; outer |= cell; }
    }
    if (rect.IsEmpty()) return {};
    auto* font = CachedFont(style, density);
    if (!font) return {};
    auto plan = std::make_shared<HDTextPlan>();
    plan->image = Canvas(uint32_t(rect.GetWidth()) * density, uint32_t(rect.GetHeight()) * density);
    if (!plan->image) return {};
    DenseLetterer letterer;
    for (size_t i = 0; i < letters.GetLetterLength(); ++i) {
        const auto* character = letters.GetCharacterAt(i);
        if (!character || !character->pImage) continue;
        SGLPoint offset, raw;
        if (!RawOffset(style, character->wchCode, raw)) return {};
        auto* glyph = letterer.Glyph(*font, nullptr, character->wchCode, offset);
        if (!glyph) return {};
        // Layout = where the letterer put the raster beyond the font's own offset.
        const SGLPoint at(character->ptWriting.x + character->ptOffset.x - raw.x,
                          character->ptWriting.y + character->ptOffset.y - raw.y);
        // Every glyph keeps exactly density x its logical layout position, so
        // line breaks, alignment and spacing cannot drift from the logical text.
        if (Debugging() && i < 3)
            SDL_Log("HD text glyph code=%u logical-at=%d,%d logical-offset=%d,%d logical-size=%ux%u dense-offset=%d,%d dense-size=%ux%u rect=%d,%d,%d,%d",
                    character->wchCode, character->ptWriting.x, character->ptWriting.y, character->ptOffset.x,
                    character->ptOffset.y, character->pImage->width, character->pImage->height, offset.x, offset.y,
                    glyph->width, glyph->height, rect.left, rect.top, rect.GetWidth(), rect.GetHeight());
        sglBlendImageBuffer(*plan->image, *glyph,
                            (at.x - rect.left) * density + offset.x,
                            (at.y - rect.top) * density + offset.y);
        sglReleaseImageBuffer(glyph);
    }
    letterer.Decorate(Dense(decoration), plan->image,
                      SGLPoint((rect.left - outer.left) * density, (rect.top - outer.top) * density),
                      SGLSize(outer.GetWidth() * density, outer.GetHeight() * density));
    return plan->image ? plan : nullptr;
} catch (const std::exception& error) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "HD text: %s", error.what());
    return {};
}

void AttachHDText(SGLImageObject& logical, const std::shared_ptr<HDTextPlan>& plan) try {
    if (!plan || !plan->image) return;
    if (Debugging())
        SDL_Log("HD text attach logical=%ux%u dense=%ux%u", logical.GetImageWidth(), logical.GetImageHeight(),
                plan->image->width, plan->image->height);
    Publish(logical, *plan->image, 0, 0, "derived:hd-text");
} catch (const std::exception& error) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "HD text attach: %s", error.what());
}

} // namespace study::platform::sdl
