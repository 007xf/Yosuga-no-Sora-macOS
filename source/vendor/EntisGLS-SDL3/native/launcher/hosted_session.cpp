#include "compatibility/sdk/legacy/gls.h"
#include "launcher/hosted_session.h"
#include "runtime/cotopha_port/host_bridge.h"
#include "runtime/cotopha_port/game_progress.h"
#include "platform/log.h"
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

namespace entis::host {
namespace {

struct Arg {
    enum { Integer, Real, Text } type;
    long long integer = 0;
    double real = 0;
    const wchar_t* text = nullptr;
    Arg(int value) : type(Integer), integer(value) {}
    Arg(double value) : type(Real), real(value) {}
    Arg(const wchar_t* value) : type(Text), text(value) {}
};

// The script stack before a call, so a call that ended in an uncaught error
// (the app ending the session from inside Title()) can be unwound.
struct StackMark {
    unsigned blocks = 0, objects = 0;
    DWORD lastFlags = 0;
};
StackMark Mark(ECSContext& context) {
    StackMark mark{context.m_stack.m_block.GetSize(), context.m_stack.m_varArray.GetSize(), 0};
    if (auto* last = context.m_stack.m_block.GetLastAt(0)) mark.lastFlags = last->m_dwFlags;
    return mark;
}
void Unwind(ECSContext& context, const StackMark& mark) {
    ECSWideString* name = nullptr;
    while (context.m_stack.m_block.GetSize() > mark.blocks) context.m_stack.ReleaseNameBlock(name, context);
    while (context.m_stack.m_varArray.GetSize() > mark.objects) {
        ECSObject* object = context.m_stack.m_varArray.Pop();
        if (object) context.delete_CSObject(object);
    }
    if (auto* last = context.m_stack.m_block.GetLastAt(0)) last->m_dwFlags = mark.lastFlags;
    context.m_stack.UpdateCurrentFrame();
}

// Calls a global script function and returns its Integer result in result.
ESLError Call(ECSContext& context, ECSExecutionImage& image, const wchar_t* name,
              std::initializer_list<Arg> args = {}, long long* result = nullptr) {
    const DWORD* address = image.GetFunctionAddress(name);
    if (!address) {
        study::platform::LogPrint(study::platform::LogPriority::Error, "EntisGLS",
                                  "Hosted session: script has no function %ls", name);
        return eslErrGeneral;
    }
    ECSObjArray<ECSObject> list;
    for (const auto& arg : args) {
        if (arg.type == Arg::Integer) list.Add(new ECSInteger(arg.integer));
        else if (arg.type == Arg::Real) list.Add(new ECSReal(arg.real));
        else list.Add(new ECSString(arg.text));
    }
    if (context.GetStatus() != ECSContext::xsExecution) context.SetStatus(ECSContext::xsExecution);
    const StackMark mark = Mark(context);
    const ESLError error = context.CallFunction(*address, list);
    if (error) Unwind(context, mark);
    if (result) {
        *result = 0;
        if (auto* value = ESLTypeCast<ECSInteger>(ECSObject::GetEntity(context.GetReturnValue())))
            *result = value->GetValue();
    }
    return error;
}

std::wstring GlobalMemberString(ECSExecutionImage& image, const wchar_t* global, const wchar_t* member) {
    const int index = image.m_csgGlobal.m_staObjName.FindIndex(global);
    if (index < 0) return {};
    auto* structure = ESLTypeCast<ECSStructure>(ECSObject::GetEntity(image.m_csgGlobal.m_varArray.GetAt(index)));
    if (!structure) return {};
    const ECSWideString value = structure->GetMemberAsStr(member, L"");
    return static_cast<const wchar_t*>(value);
}

ECSStructure* GlobalStructure(ECSExecutionImage& image, const wchar_t* global) {
    const int index = image.m_csgGlobal.m_staObjName.FindIndex(global);
    return index < 0 ? nullptr : ESLTypeCast<ECSStructure>(
        ECSObject::GetEntity(image.m_csgGlobal.m_varArray.GetAt(index)));
}

// The diagnostics are unavailable during normal launches, and may touch only
// the explicitly isolated savedata opener selected by the development driver.
bool DiagnosticSaveRootMatches(ECSContext& context, Game game, const char* testRoot) {
    auto* environment = context.GetEnvironment();
    auto* opener = environment ? environment->GetWritableFileOpener() : nullptr;
    SSystem::SString direct;
    if (!opener || opener->DirectPathOf(direct, L"")) return false;
    for (unsigned i = 0; i < 8 && SSystem::g_defURLOpener.FindScheme(direct) >= 0; ++i) {
        SSystem::SString next;
        if (SSystem::g_defURLOpener.DirectPathOf(next, direct) || next == direct) return false;
        direct = next;
    }
    if (SSystem::g_defURLOpener.FindScheme(direct) >= 0) return false;
    try {
        namespace fs = std::filesystem;
        const auto expected = fs::absolute(fs::u8path(testRoot)) /
            (game == Game::Haruka ? "haruka" : "yosuga") / "savedata";
        const auto actual = fs::u8path(direct.ToCharArray().GetConstArray());
        return fs::canonical(actual) == fs::canonical(expected);
    } catch (...) { return false; }
}

ESLError UnlockDiagnosticExtras(ECSExecutionImage& image) {
    auto* system = GlobalStructure(image, L"sysObj");
    if (!system) return eslErrGeneral;
    std::vector<ECSInteger*> words;
    for (const auto* name : {L"cgFlag", L"recollectFlag"}) {
        auto* value = system->GetMemberAs(name);
        auto* flag = value ? ESLTypeCast<ECSStructure>(ECSObject::GetEntity(value)) : nullptr;
        value = flag ? flag->GetMemberAs(L"flag") : nullptr;
        auto* array = value ? ESLTypeCast<ECSArray>(ECSObject::GetEntity(value)) : nullptr;
        const unsigned expected = std::wcscmp(name, L"cgFlag") == 0 ? 128u : 1u;
        if (!array || array->m_varArray.GetSize() != expected) return eslErrGeneral;
        for (unsigned i = 0; i < expected; ++i) {
            auto* word = ESLTypeCast<ECSInteger>(ECSObject::GetEntity(array->m_varArray.GetAt(i)));
            if (!word) return eslErrGeneral;
            words.push_back(word);
        }
    }
    // Validate both original Flag arrays before modifying either one. These
    // are the original debug unlock operations; readFlag is left untouched.
    for (auto* word : words) word->SetValue(-1);
    system->SetMemberAsInt(L"gameClear", -1);
    return eslErrSuccess;
}

ESLError RunStartupDiagnostic(ECSContext& context, ECSExecutionImage& image, Game game,
                             bool& ran, int& titleMenu) {
    ran = false;
    const char* driver = std::getenv("ENTISGLS_DRIVER");
    const char* testRoot = std::getenv("ENTISGLS_TEST_SAVEDIR");
    const char* requested = std::getenv("ENTISGLS_TEST_ACTION");
    static bool consumed = false;
    if (consumed || !driver || !*driver || !testRoot || !*testRoot || !requested || !*requested)
        return eslErrSuccess;
    consumed = ran = true;
    const std::string action(requested);
    study::platform::LogPrint(study::platform::LogPriority::Info, "EntisGLS",
        "Hosted diagnostic start: game=%ls action=%s", GameName(game), action.c_str());
    ESLError error = eslErrSuccess;
    long long result = 0;
    if (!DiagnosticSaveRootMatches(context, game, testRoot)) {
        study::platform::LogPrint(study::platform::LogPriority::Error, "EntisGLS",
            "Hosted diagnostic rejected: writable savedata is not the isolated test directory");
        error = eslErrGeneral;
    } else if (action == "movie") {
        error = Call(context, image, L"StopBgm");
        if (!error && game == Game::Haruka) {
            auto* config = GlobalStructure(image, L"cnfObj");
            if (!config) error = eslErrGeneral;
            else {
                const double volume = config->GetMemberAsInt(L"flgPlayBgm", 0) == -1 ?
                    config->GetMemberAsReal(L"volBgm", 0.0) * 0.75 : 0.0;
                error = Call(context, image, L"PlayMovie", {L"opening.mei", volume}, &result);
            }
        } else if (!error) error = Call(context, image, L"PlayMovie", {L"opening.mei", 0}, &result);
    } else if (action == "eyecatch" || action == "staffroll") {
        error = Call(context, image, L"SetupAdvScreen");
        if (!error) {
            if (action == "eyecatch") {
                error = Call(context, image, L"EyeCatchEnter", {L"NORMAL", 0});
                if (!error) error = Call(context, image, L"EyeCatchLeave", {}, &result);
            } else error = Call(context, image, L"StaffRoll", {4}, &result);  // Sora in both works.
            const ESLError destroyed = Call(context, image, L"DestroyAdvScreen");
            if (!error) error = destroyed;
        }
    } else if (action == "extras") {
        const char* unlock = std::getenv("ENTISGLS_TEST_UNLOCK");
        if (unlock && std::string(unlock) == "1") {
            error = UnlockDiagnosticExtras(image);
            study::platform::LogPrint(study::platform::LogPriority::Info, "EntisGLS",
                "Hosted diagnostic isolated extras unlock: result=%lld", static_cast<long long>(error));
        }
        if (!error) error = Call(context, image, L"Appreciation", {}, &result);
    } else if (action.rfind("scenario:", 0) == 0) {
        const std::string id = action.substr(9);
        bool valid = !id.empty() && id.size() <= 64;
        for (const char c : id)
            valid = valid && ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_');
        const std::wstring scenario(id.begin(), id.end());
        const std::wstring function = L"sc" + scenario;
        if (!valid || !image.GetFunctionAddress(function.c_str())) {
            study::platform::LogPrint(study::platform::LogPriority::Error, "EntisGLS",
                "Hosted diagnostic rejected: scenario must name an existing sc-prefixed scenario");
            error = eslErrGeneral;
        } else {
            error = Call(context, image, L"ScenarioLoop", {scenario.c_str()}, &result);
            if (!error && game == Game::Haruka) titleMenu = result == 110 ? 1 : result == 120 ? 2 : 0;
        }
    } else {
        study::platform::LogPrint(study::platform::LogPriority::Error, "EntisGLS",
            "Hosted diagnostic rejected: action is not in the fixed diagnostic list");
        error = eslErrGeneral;
    }
    study::platform::LogPrint(study::platform::LogPriority::Info, "EntisGLS",
        "Hosted diagnostic completed: game=%ls action=%s status=%s error=%lld result=%lld",
        GameName(game), action.c_str(), error ? "failed" : "completed",
        static_cast<long long>(error), result);
    return error;
}

// Title() returned code: what the original Starting() would do next.
ESLError DispatchYosuga(ECSContext& context, ECSExecutionImage& image, long long code, bool& end) {
    end = false;
    switch (code) {
        case 1: return Call(context, image, L"ScenarioLoop", {L"00_Z000"});
        case 2: {
            const auto scenario = GlobalMemberString(image, L"scObj", L"scenarioCall");
            return Call(context, image, L"ScenarioLoop", {scenario.c_str()});
        }
        case 3: {
            ESLError error = Call(context, image, L"Appreciation");
            if (!error) error = Call(context, image, L"PlayBgm", {L"BGM_01"});
            return error;
        }
        case 10: {
            ESLError error = Call(context, image, L"StopBgm");
            if (!error) error = Call(context, image, L"PlayMovie", {L"opening.mei"});
            return error;
        }
        default: end = true; return eslErrSuccess;  // 4: exit game
    }
}

ESLError DispatchHaruka(ECSContext& context, ECSExecutionImage& image, long long code,
                        bool& end, int& titleMenu) {
    end = false;
    titleMenu = 0;
    auto scenarioLoop = [&](const wchar_t* scenario) {
        long long result = 0;
        const ESLError error = Call(context, image, L"ScenarioLoop", {scenario}, &result);
        if (!error) {
            // Original Starting(): 110 -> ret 7 -> Title(1),
            // 120 -> ret 8 -> Title(2); applies to new and resumed stories.
            if (result == 110) titleMenu = 1;
            else if (result == 120) titleMenu = 2;
        }
        return error;
    };
    static const wchar_t* scenarios[] = {L"00_A001", L"00_G001", L"00_H001", L"KARAOKE",
        L"WEB01", L"WEB02", L"WEB03", L"WEB04", L"WEB05", L"WEB06", L"COUNTDOWN"};
    if (code >= 11 && code <= 21) return scenarioLoop(scenarios[code - 11]);
    switch (code) {
        case 2: {
            const auto scenario = GlobalMemberString(image, L"scObj", L"scenarioCall");
            return scenarioLoop(scenario.c_str());
        }
        case 3: return Call(context, image, L"Appreciation");
        case 22: return Call(context, image, L"WallPaper");
        case 7: titleMenu = 1; return eslErrSuccess;
        case 8: titleMenu = 2; return eslErrSuccess;
        case 10: return Call(context, image, L"PlayMovie", {L"opening.mei"});
        default: end = true; return eslErrSuccess;  // 4: exit game
    }
}

}  // namespace

ESLError RunHostedSession(ECSContext& context, ECSExecutionImage& image, Game game) {
    auto& bridge = Bridge::Get();
    ESLError error = Call(context, image, L"Initialize");
    if (error) return error;
    // The brand logo plays once, when the app opens on Yosuga no Sora; a
    // switch between the two games goes straight to the other title.
    static bool firstSession = true;
    const bool showLogo = firstSession && game == Game::Yosuga && bridge.StartupLogoEnabled();
    firstSession = false;
    study::platform::LogPrint(study::platform::LogPriority::Info, "EntisGLS",
        "Hosted session: game=%ls brand-logo=%s startup-preference=%s", GameName(game),
        showLogo ? "play" : "skip", bridge.StartupLogoEnabled() ? "on" : "off");
    bridge.BeginSession(game);
    entis::progress::BeginGuideSession(context, image);
    int harukaTitleMenu = 0;
    std::wstring guideScenario;
    const bool guideRestored = entis::progress::RestoreGuideSession(context, image, guideScenario);
    if (guideRestored) {
        // EnterLoad has restored the original ScenarioObject. Resume only from
        // the clean hosted call stack, exactly as normal loading does.
        long long guideResult = 0;
        error = Call(context, image, L"ScenarioLoop", {guideScenario.c_str()}, &guideResult);
        if (!error && game == Game::Haruka)
            harukaTitleMenu = guideResult == 110 ? 1 : guideResult == 120 ? 2 : 0;
    }
    bool diagnosticRan = false;
    if (!guideRestored)
        error = RunStartupDiagnostic(context, image, game, diagnosticRan, harukaTitleMenu);
    if (diagnosticRan || guideRestored) {
        Game next;
        bool quit;
        const bool leaving = bridge.TakeLeave(next, quit);
        if (leaving) error = eslErrSuccess;
        if (error || leaving) {
            const ESLError saved = Call(context, image, L"Postprocessing");
            bridge.EndSession();
            return error ? error : saved;
        }
    }
    if (showLogo && !diagnosticRan && !guideRestored) {
        error = Call(context, image, L"Logo");
        Game next;
        bool quit;
        if (bridge.TakeLeave(next, quit)) error = eslErrSuccess;
        if (error || next != Game::None || quit) {
            Call(context, image, L"Postprocessing");
            bridge.EndSession();
            return error;
        }
    }
    for (;;) {
        bridge.EnteringTitle();
        long long code = 0;
        error = game == Game::Haruka ? Call(context, image, L"Title", {harukaTitleMenu}, &code)
                                     : Call(context, image, L"Title", {}, &code);
        Game next;
        bool quit;
        if (bridge.TakeLeave(next, quit)) { error = eslErrSuccess; break; }
        if (error) break;
        bool end = false;
        error = game == Game::Haruka ? DispatchHaruka(context, image, code, end, harukaTitleMenu)
                                     : DispatchYosuga(context, image, code, end);
        if (bridge.TakeLeave(next, quit)) { error = eslErrSuccess; break; }
        if (error || end) break;
    }
    // Leaving through a Window.GetCommand error leaves the context stopped;
    // save the system data and settings the way main() would have.
    const ESLError saved = Call(context, image, L"Postprocessing");
    if (saved) study::platform::LogPrint(study::platform::LogPriority::Warn, "EntisGLS",
                                         "Hosted session: Postprocessing failed");
    bridge.EndSession();
    return error ? error : saved;
}

}  // namespace entis::host
