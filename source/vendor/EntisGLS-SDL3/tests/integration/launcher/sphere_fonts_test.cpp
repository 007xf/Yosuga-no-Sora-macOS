// Headless regression for Sphere's static choice text. Uses synthetic Chinese
// labels and system fonts only; no game archives, script text, windows or saves.
#include "compatibility/games/compatibility_profiles.h"
#include "platform/sdl/game_font_aliases.h"
#include "platform/sdl/opentype_font.h"
#include "platform/sdl/system.h"
#include <sakuragl/sakuragl.h>
#include <sakuragl/sgl2d/sgl_font.h>
#include <sakuraglx/sakuraglx.h>
#include <sakuraglx/sprite/sglx_sprite.h>
#include <sakuraglx/sprite/sglx_sprite_text.h>
#include <sakuraglx/sprite/sglx_sprite_message.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace SakuraGL;
using namespace study::platform::sdl;
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

class MessageInputProbe final : public SGLSpriteMessage {
public:
    unsigned clicks = 0;
    void AddPlainCharacter() { m_characters.Add(new Character); }
    void AddFocusedLink() {
        auto* character = new Character;
        character->m_pLink = new LinkInfo;
        SGLRect rect; rect.left=10; rect.top=10; rect.right=20; rect.bottom=20;
        character->m_pLink->m_aHitRects.Add(rect);
        character->m_pLink->m_status = LinkInfo::statusFocus;
        m_pFocusLink = character->m_pLink;
        m_characters.Add(character);
    }
    bool LinkIsFocused() const { return m_pFocusLink && m_pFocusLink->m_status==LinkInfo::statusFocus; }
protected:
    void OnClickLinkInfo(const LinkInfo*) override { ++clicks; }
};

void CheckMessageMouseInput() {
    MessageInputProbe message;
    Require(!message.OnLButtonDown(0,0,0) && !message.OnLButtonUp(0,0,0),
            "empty messages pass ordinary clicks to the base sprite without null dereference");
    message.AddPlainCharacter();
    for(unsigned i=0;i<32;++i)
        Require(!message.OnLButtonDown(0,0,0) && !message.OnLButtonUp(0,0,0),
                "plain message characters safely handle repeated mouse down and up");
    message.AddFocusedLink();
    Require(!message.OnLButtonDown(0,0,0) && !message.OnLButtonUp(0,0,0),
            "clicking outside a focused hyperlink uses the normal base path");
    Require(message.OnLButtonDown(15,15,0) && message.OnLButtonUp(15,15,0),
            "a real focused hyperlink still consumes its click");
    Require(message.clicks==1 && message.LinkIsFocused(), "hyperlink fires once and restores focused state");
    std::puts("Message mouse input PASS: empty and plain messages; 32 repeated down/up pairs; outside link; valid hyperlink once");
}

struct Raster {
    SGLFontMetrics metrics{};
    std::vector<uint8_t> pixels;
};
Raster Glyph(const std::wstring& face) {
    SGLFontStyle style;
    style.pszFace = face.c_str();
    style.nSize = 32;
    style.nStyles = SGLFontStyle::styleBold;
    SGLFont font;
    Require(!font.SetStyle(style), "select synthetic choice font");
    Raster result;
    Require(!font.GetMetrics(nullptr, 0, result.metrics, L'择'), "choice font glyph metrics");
    Require(result.metrics.rctExterior.w > 0 && result.metrics.rctExterior.h > 0,
            "choice glyph has a visible rectangle");
    result.pixels.resize(size_t(result.metrics.rctExterior.w) * result.metrics.rctExterior.h);
    Require(!font.GetMetrics(result.pixels.data(), result.pixels.size(), result.metrics, L'择'),
            "choice font rasterization");
    Require(std::any_of(result.pixels.begin(), result.pixels.end(), [](uint8_t v) { return v; }),
            "choice font raster contains ink");
    return result;
}
bool SameRaster(const Raster& a, const Raster& b) {
    return a.metrics.nAscent == b.metrics.nAscent && a.metrics.nDescent == b.metrics.nDescent &&
        a.metrics.nWidth == b.metrics.nWidth && a.metrics.nHeight == b.metrics.nHeight &&
        a.metrics.rctExterior.x == b.metrics.rctExterior.x && a.metrics.rctExterior.y == b.metrics.rctExterior.y &&
        a.metrics.rctExterior.w == b.metrics.rctExterior.w && a.metrics.rctExterior.h == b.metrics.rctExterior.h &&
        a.pixels == b.pixels;
}

struct ImagePixels {
    unsigned width = 0, height = 0;
    size_t ink = 0, white = 0;
    std::vector<uint8_t> rgba;
};
ImagePixels ReadImage(SGLImageObject& image) {
    SGLImageInfo info;
    Require(!image.GetImageInfo(info) && info.width && info.height &&
            info.format == formatImageARGB && info.depth == 32, "static choice uses premultiplied ARGB32");
    const auto* pixels = image.LockBuffer(info, SGLImageObject::lockRead);
    Require(pixels != nullptr, "lock static choice pixels");
    ImagePixels result;
    result.width = info.width;
    result.height = info.height;
    result.rgba.reserve(size_t(info.width) * info.height * 4);
    for (unsigned y = 0; y < info.height; ++y) for (unsigned x = 0; x < info.width; ++x) {
        SGLPalette color;
        std::memcpy(&color.ui32, pixels + ptrdiff_t(y) * info.pitchLine + x * info.pitchPixel, 4);
        result.rgba.insert(result.rgba.end(), {color.argb.Red, color.argb.Green, color.argb.Blue, color.argb.Alpha});
        result.ink += color.argb.Alpha != 0;
        result.white += color.argb.Alpha >= 245 && color.argb.Red >= 245 &&
            color.argb.Green >= 245 && color.argb.Blue >= 245;
    }
    image.UnlockBuffer(SGLImageObject::lockRead);
    Require(result.ink > 0 && result.white > 0, "static choice contains visible opaque white body pixels");
    return result;
}

SGLSpriteText::TextStyle ChoiceStyle(const wchar_t* face) {
    // Exact ID_FONT_SELECT style and ID_TEXT dimensions, with no game text.
    SGLSpriteText::TextStyle style;
    style.boxAlign = SGLSpriteText::alignBoxLeft | SGLSpriteText::alignBoxTop;
    style.font.pszFace = face;
    style.font.nSize = 32;
    style.font.nStyles = SGLFontStyle::styleBold;
    style.context.ptStartWriting = SGLPoint(0, 0);
    style.context.rectWritable = SGLImageRect(0, 0, 565, 36);
    style.context.typeAlignment = SGLLetteringContext::alignLeft;
    style.context.flagVertical = SGLLetteringContext::writingHorizontal;
    style.context.pitchLine = 34;
    style.context.widthIndent = 0;
    style.decoration.nFlags = SGLLetterer::flagShadow;
    style.decoration.rgbaBody = SGLPalette(0xffffffff);
    style.decoration.rgbaShadow = SGLPalette(0xff21599e);
    style.decoration.ptShadow = SGLPoint(1, 2);
    return style;
}

void CheckStaticText(const wchar_t* face, unsigned aliasIndex) {
    const auto style = ChoiceStyle(face);
    SGLSpriteText sprite;
    sprite.SetTextStyle(style);
    unsigned count = 0;
    for (const auto* text : {L"选择第一项", L"选择第二项"}) {
        SGLPoint origin;
        std::unique_ptr<SGLImageObject> image(SGLSpriteText::CreateTextImage(origin, style, text));
        Require(bool(image), "exact choice style creates a nonempty image");
        const auto expected = ReadImage(*image);
        sprite.SetText(text);
        auto* attached = sprite.GetAttachedImage();
        Require(attached != nullptr, "SetText attaches the choice image");
        const auto actual = ReadImage(*attached);
        Require(actual.width == expected.width && actual.height == expected.height && actual.rgba == expected.rgba,
                "SetText and CreateTextImage produce identical static choice pixels");
        ++count;
        std::printf("Sphere static choice PASS alias-index=%u synthetic-label=%u size=%ux%u ink=%zu opaque-white=%zu\n",
                    aliasIndex, count, actual.width, actual.height, actual.ink, actual.white);
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: launcher_sphere_fonts_test <isolated-fixture-root>\n");
        return 2;
    }
    if (!SDL_Init(0)) return 2;
    bool initialized = false;
    int status = 0;
    try {
        const auto root = std::filesystem::absolute(argv[1]);
        for (const auto* name : {"assets", "storage", "local", "game"})
            std::filesystem::create_directories(root / name);
        SystemPaths paths;
        paths.assetsRoot = (root / "assets").string();
        paths.storageRoot = (root / "storage").string();
        paths.localRoot = (root / "local").string();
        paths.gameRoot = (root / "game").string();
        Require(ConfigureSystemPaths(paths), "configure isolated empty roots");
        SakuraGL::Initialize();
        initialized = true;
        CheckMessageMouseInput();
        using namespace study::launcher;
        Require(ConfigureCompatibility("") && ApplyCompatibilityFonts(""), "generic startup is independent");
        const wchar_t* minchoAliases[] = {L"ＭＳ 明朝", L"ＭＳ Ｐ明朝", L"MS Mincho", L"MS PMincho"};
        for (const auto* alias : minchoAliases)
            Require(!HasStockFont(alias), "generic startup must not create Sphere Mincho aliases");

        OpenTypeRegistration sans;
        for (const auto* path : {L"/System/Library/Fonts/Hiragino Sans GB.ttc",
                                 L"/System/Library/Fonts/STHeiti Medium.ttc",
                                 L"/System/Library/Fonts/STHeiti Light.ttc"}) {
            sans = RegisterOpenTypeFont(path);
            if (sans.loaded) break;
        }
        Require(sans.loaded, "load the existing Sphere sans source");
        const auto sansName = sans.registeredName.empty() ? sans.family : sans.registeredName;
        const auto originalSans = Glyph(sansName);
        Require(ConfigureCompatibility("sphere-2008") && ApplyCompatibilityFonts("sphere-2008"),
                "apply Sphere fonts including choice aliases");
        Require(SameRaster(originalSans, Glyph(sansName)), "Sphere setup does not replace its original sans source");
        for (const auto* alias : {L"ＭＳ ゴシック", L"ＭＳ Ｐゴシック", L"MS Gothic", L"Default"})
            Require(HasStockFont(alias) && SameRaster(originalSans, Glyph(alias)),
                    "Gothic and Default retain the existing sans glyphs");

        const auto serifPath = std::filesystem::path("/System/Library/Fonts/Supplemental/Songti.ttc");
        const bool hasSerif = std::filesystem::is_regular_file(serifPath);
        const auto serif = hasSerif ? RegisterOpenTypeFont(serifPath.wstring()) : OpenTypeRegistration{};
        Require(!hasSerif || serif.loaded, "available system Songti font loads");
        const auto expectedMincho = hasSerif ? Glyph(serif.registeredName.empty() ? serif.family : serif.registeredName) : originalSans;
        if (hasSerif) Require(!SameRaster(expectedMincho, originalSans), "available serif source is distinct from sans");
        unsigned aliasIndex = 0;
        for (const auto* alias : minchoAliases) {
            Require(HasStockFont(alias) && SameRaster(expectedMincho, Glyph(alias)),
                    "Mincho alias uses the available serif source or the existing sans fallback");
            CheckStaticText(alias, ++aliasIndex);
        }
        std::puts("Sphere font test PASS: generic isolation; unchanged Gothic/Default; four Mincho aliases; eight synthetic static choices with opaque white bodies and matching SetText pixels");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Sphere font test FAIL: %s\n", error.what());
        status = 1;
    }
    if (initialized) SakuraGL::Finalize();
    SDL_Quit();
    return status;
}
