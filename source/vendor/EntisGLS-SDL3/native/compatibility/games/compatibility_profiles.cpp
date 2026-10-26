#include "compatibility/games/compatibility_profiles.h"
#include "platform/log.h"
#include "platform/sdl/game_font_aliases.h"
#include "platform/sdl/opentype_font.h"
#include <mutex>

namespace study::launcher {
namespace {
constexpr char studySteadyProfile[] = "study-steady-r18";
constexpr wchar_t traditionalChineseFamily[] = L"Noto Serif CJK TC";
std::mutex profileMutex;
std::string activeProfile;

// Sphere's 2008-era EntisGLS titles (Yosuga no Sora, Haruka na Sora; the
// Simplified Chinese releases). Their scripts ask for Gothic in dialogue and
// Mincho in static text such as the selectable ADV choices.
constexpr char sphereProfile[] = "sphere-2008";

bool KnownProfile(const std::string& id) {
    return id.empty() || id == studySteadyProfile || id == sphereProfile;
}

// macOS system fonts with Simplified Chinese and Japanese coverage, in order
// of preference. They are read in place and never copied into the app.
bool ApplySphereFonts() {
    using namespace study::platform;
    using namespace study::platform::sdl;
    // The merged app runs both games in one process; the font stock persists.
    static bool applied = false;
    if (applied) return true;
    static const wchar_t* candidates[] = {
        L"/System/Library/Fonts/Hiragino Sans GB.ttc",
        L"/System/Library/Fonts/STHeiti Medium.ttc",
        L"/System/Library/Fonts/STHeiti Light.ttc",
    };
    std::wstring sansFamily;
    for (const auto* path : candidates) {
        const auto registered = RegisterOpenTypeFont(path);
        if (!registered.loaded) continue;
        sansFamily = registered.registeredName.empty() ? registered.family : registered.registeredName;
        break;
    }
    if (sansFamily.empty()) {
        LogWrite(LogPriority::Error, "EntisGLS", "sphere-2008 profile: no Simplified Chinese system font could be loaded");
        return false;
    }
    for (const auto* alias : {L"ＭＳ ゴシック", L"ＭＳ Ｐゴシック", L"MS Gothic", L"Default"})
        if (!RegisterFontAlias(alias, sansFamily)) return false;

    // The original choice style requests MS Mincho. An absent stock face makes
    // SGLSpriteText silently produce no image, leaving empty choice buttons.
    // Read the system serif font in place; retain readable text on Macs where
    // that optional font is unavailable, without changing the dialogue aliases.
    std::wstring serifFamily = sansFamily;
    const auto serif = RegisterOpenTypeFont(L"/System/Library/Fonts/Supplemental/Songti.ttc");
    if (serif.loaded)
        serifFamily = serif.registeredName.empty() ? serif.family : serif.registeredName;
    for (const auto* alias : {L"ＭＳ 明朝", L"ＭＳ Ｐ明朝", L"MS Mincho", L"MS PMincho"})
        if (!RegisterFontAlias(alias, serifFamily)) return false;
    applied = true;
    return true;
}
} // namespace

bool ConfigureCompatibility(const std::string& profileId) {
    if (!KnownProfile(profileId)) return false;
    std::lock_guard<std::mutex> lock(profileMutex);
    activeProfile = profileId;
    return true;
}

std::string ActiveCompatibilityProfile() {
    std::lock_guard<std::mutex> lock(profileMutex);
    return activeProfile;
}

bool ApplyCompatibilityFonts(const std::string& profileId) {
    using namespace study::platform;
    using namespace study::platform::sdl;
    if (profileId.empty()) return true;
    if (!KnownProfile(profileId)) return false;
    if (profileId == sphereProfile) return ApplySphereFonts();
    // The original bitmap fonts remain registered by this game's own SDK XML.
    // These aliases only fill absent names; explicit configuration always wins.
    if (!RegisterFontAlias(L"Default", L"MsgFont") ||
        !RegisterFontAlias(L"@Default", L"@MsgFont")) {
        LogWrite(LogPriority::Error, "EntisGLS",
                 "study-steady-r18 profile: configured MsgFont/@MsgFont bitmap fonts are unavailable");
        return false;
    }
    if (HasStockFont(traditionalChineseFamily)) return true;
    // This optional, separately licensed compatibility asset also supports
    // existing Android imports containing only NOA archives. A generic game
    // never reads it and building the launcher does not require it.
    for (const auto* path : {L"assets://fonts/NotoSerifCJKtc-Bold.otf",
                            L"storage://game/SETUP_ZHTW/NotoSerifCJKtc-Bold.otf"}) {
        if (RegisterOpenTypeFont(path, traditionalChineseFamily).loaded) return true;
    }
    LogWrite(LogPriority::Warn, "EntisGLS",
             "study-steady-r18 profile: Traditional Chinese requires Noto Serif CJK TC; "
             "provide the game's SETUP_ZHTW font or the optional compatibility font asset");
    // The Japanese game does not need this font. Do not reject every language
    // because an optional localization font is absent from a generic build.
    return true;
}

bool CheckCompatibilityFonts(const std::string& profileId) {
    using namespace study::platform::sdl;
    if (profileId.empty()) return true;
    if (!KnownProfile(profileId)) return false;
    if (profileId == sphereProfile) return true;
    const std::vector<uint32_t> japanese{L'A', L'あ', L'日'};
    return CheckFontAlias(L"Default", L"MsgFont", japanese) &&
           CheckFontAlias(L"@Default", L"@MsgFont", japanese) &&
           CheckOpenTypeFont(traditionalChineseFamily,
                             {L'名', L'姓', L'樹', L'和', L'說', L'腳', L'夠', L'，', L'。', L'「', L'」'},
                             {L' ', L'　'});
}
} // namespace study::launcher
