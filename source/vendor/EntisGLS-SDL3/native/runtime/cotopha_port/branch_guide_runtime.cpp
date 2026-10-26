#include "compatibility/sdk/legacy/gls.h"
#include "compatibility/games/compatibility_profiles.h"
#include "runtime/cotopha_port/game_progress.h"
#include "runtime/cotopha_port/game_progress_data.h"
#include "runtime/cotopha_port/branch_guide_store.h"
#include "runtime/cotopha_port/branch_native_state.h"
#include "runtime/cotopha_port/host_bridge.h"
#include "platform/log.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace entis::progress {
namespace {
using entis::guide::Store;
using entis::guide::Observation;
using entis::guide::PreparedRestore;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
struct Session {
    std::string game, root, activeChoice, activeScenario, sections;
    int activeOrdinal = -1;
    std::set<std::string> chapterSnapshots;
    Clock::time_point nextLive{};
    Flags previousFlags;
    bool haveFlags = false;
    bool inTitle = false, loading = false;
    bool restorePending = false, restoreSucceeded = false;
    std::string restoreMessage;
};
thread_local Session current;
std::mutex transferMutex;
struct Transfer {
    PreparedRestore target;
    std::string originalGame, originalResourceHash;
    std::vector<std::uint8_t> rollback;
    bool armed = false, restoringRollback = false;
};
Transfer transfer;
std::mutex permissionsMutex;
std::map<std::string, bool> clearedGames;

ECSObject* Entity(ECSObject* object) { return ECSObject::GetEntity(object); }
ECSStructure* Structure(ECSObject* object) { return object ? ESLTypeCast<ECSStructure>(Entity(object)) : nullptr; }
ECSArray* Array(ECSObject* object) { return object ? ESLTypeCast<ECSArray>(Entity(object)) : nullptr; }
ECSObject* Global(ECSExecutionImage& image, const wchar_t* name) {
    const int index = image.m_csgGlobal.m_staObjName.FindIndex(name);
    return index < 0 ? nullptr : Entity(image.m_csgGlobal.m_varArray.GetAt(index));
}
std::wstring WString(ECSStructure& object, const wchar_t* name) {
    const ECSWideString value = object.GetMemberAsStr(name, L"");
    return static_cast<const wchar_t*>(value);
}
std::string Utf8(const std::wstring& value) {
    const SSystem::SString string(value.c_str());
    const auto utf8 = string.ToUTF8();
    std::string result(reinterpret_cast<const char*>(utf8.GetConstArray()), utf8.GetLength());
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}
std::string J(const std::string& value) {
    std::string result = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
        else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += char(c);
    }
    return result + '"';
}
std::string Identify(ECSExecutionImage& image) {
    if (image.GetFunctionAddress(L"sc00_Z000")) return "yosuga";
    if (image.GetFunctionAddress(L"scKARAOKE")) return "haruka";
    return {};
}
const data::Chapter* Chapters(const std::string& game, size_t& count) {
    if (game == "yosuga") { count = std::size(data::yosugaChapters); return data::yosugaChapters; }
    count = std::size(data::harukaChapters); return data::harukaChapters;
}
const data::SpecialEntry* Specials(const std::string& game, size_t& count) {
    if (game == "yosuga") { count = std::size(data::yosugaSpecials); return data::yosugaSpecials; }
    count = std::size(data::harukaSpecials); return data::harukaSpecials;
}
std::string ChapterOf(const std::string& game, const std::string& scenario) {
    if (game == "yosuga") {
        for (const auto& item : data::yosugaScenarios) if (scenario == item.id) return item.chapter;
    } else if (game == "haruka") {
        for (const auto& item : data::harukaScenarios) if (scenario == item.id) return item.chapter;
    }
    return {};
}
const data::Chapter* Chapter(const std::string& game, const std::string& id) {
    size_t count; const auto* list = Chapters(game, count);
    for (size_t i = 0; i < count; ++i) if (id == list[i].id) return list + i;
    return nullptr;
}
bool Bit(const std::vector<std::uint32_t>& words, unsigned bit) {
    return bit / 32 < words.size() && (words[bit / 32] & (std::uint32_t(1) << (bit % 32)));
}
bool Words(ECSStructure& system, const wchar_t* name, size_t count, std::vector<std::uint32_t>& output) {
    auto* flag = Structure(system.GetMemberAs(name));
    auto* words = flag ? Array(flag->GetMemberAs(L"flag")) : nullptr;
    if (!words || words->m_varArray.GetSize() != count) return false;
    output.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        auto* word = ESLTypeCast<ECSInteger>(Entity(words->m_varArray.GetAt(i)));
        if (!word) return false;
        output.push_back(static_cast<std::uint32_t>(word->GetValue()));
    }
    return true;
}
bool ReadFlags(ECSExecutionImage& image, Flags& flags) {
    auto* system = Structure(Global(image, L"sysObj"));
    if (!system || !Words(*system, L"readFlag", 1500, flags.read) ||
        !Words(*system, L"cgFlag", 128, flags.cg) || !Words(*system, L"recollectFlag", 1, flags.recollection)) return false;
    flags.gameClear = system->GetMemberAsInt(L"gameClear", 0) != 0;
    return true;
}
bool Resolve(SSystem::SString& path) {
    for (unsigned i = 0; i < 8 && SSystem::g_defURLOpener.FindScheme(path) >= 0; ++i) {
        SSystem::SString next;
        if (SSystem::g_defURLOpener.DirectPathOf(next, path) || next == path) return false;
        path = next;
    }
    return SSystem::g_defURLOpener.FindScheme(path) < 0;
}
std::string Root(ECSContext& context) {
    auto* opener = context.GetEnvironment()->GetWritableFileOpener();
    SSystem::SString path;
    if (!opener || opener->DirectPathOf(path, L"") || !Resolve(path)) return {};
    return path.ToCharArray().GetConstArray();
}
std::string ScriptPath(ECSContext& context, const std::string& game) {
    auto* environment = context.GetEnvironment();
    const wchar_t* filename = game == "yosuga" ? L"yosuga.csx" : L"Haruka.csx";
    for (size_t i = 0; i < environment->GetFileOpenerCount(); ++i) {
        auto* opener = environment->GetFileOpenerAt(i);
        SSystem::SString path;
        if (!opener || opener->DirectPathOf(path, filename) || !Resolve(path)) continue;
        const fs::path native = fs::u8path(path.ToCharArray().GetConstArray());
        std::error_code error;
        if (fs::is_regular_file(native, error)) return fs::canonical(native, error).u8string();
    }
    return {};
}
bool CopyMember(ECSContext& context, ECSStructure& target, const wchar_t* name, ECSObject* source) {
    return source && !target.MoveMemberAs(context, name, Entity(source));
}
bool StripTransientBustupSprites(ECSStructure& copy) {
    auto* list = Array(copy.GetMemberAs(L"bustup"));
    if (!list || list->m_varArray.GetSize() > 64) return false;
    auto strip = [](ECSObject* object) {
        if (!object) return true;
        auto* info = Structure(object);
        if (!info || std::wcscmp(info->GetTypeName(), L"BustupInfo")) return false;
        // Haruka adds renderer-owned sprites to BustupInfo. Original LeaveLoad
        // recreates them with SetBustup/SetSprite from the metadata below; no
        // saved sprite is used. Remove these fields only from our deep copy,
        // keeping guide files self-contained and the native-class guard strict.
        for (const auto* name : {L"spr", L"sprImage"}) {
            const int index = info->FindVariableIndex(name);
            if (index >= 0) info->m_varArray.SetAt(unsigned(index), nullptr);
        }
        return true;
    };
    for (unsigned i = 0; i < list->m_varArray.GetSize(); ++i)
        if (!strip(list->m_varArray.GetAt(i))) return false;
    return strip(list->m_pDefObj);
}
std::vector<std::uint8_t> Capture(ECSContext& context, ECSExecutionImage& image) {
    auto failure = [](const char* phase) {
        study::platform::LogPrint(study::platform::LogPriority::Warn, "EntisGLS",
            "Guide native capture rejected game=%s phase=%s", current.game.c_str(), phase);
        return std::vector<std::uint8_t>();
    };
    auto* scenario = Structure(Global(image, L"scObj"));
    auto* adv = Structure(Global(image, L"adv"));
    const int nativeVersion = current.game == "haruka" ? 100 : 200;
    if (!scenario || !adv || scenario->GetMemberAsInt(L"version", 0) != nativeVersion) return failure("scenario-version");
    std::unique_ptr<ECSObject> object(scenario->Duplicate());
    auto* copy = Structure(object.get());
    if (!copy) return failure("duplicate");
    // Exactly the presentation fields synchronized by original file-9 Save(),
    // without its synchronous screen capture, disk write, or global mutation.
    if (!CopyMember(context, *copy, L"cg", adv->GetMemberAs(L"cg"))) return failure("cg");
    if (!CopyMember(context, *copy, L"zoomParam", adv->GetMemberAs(L"zoomParam"))) return failure("zoom");
    auto* bustup = Structure(adv->GetMemberAs(L"bustupMan"));
    if (!bustup || !CopyMember(context, *copy, L"bustup", bustup->GetMemberAs(L"info"))) return failure("bustup-copy");
    copy->SetMemberAsInt(L"fCgRGB", adv->GetMemberAsInt(L"fSetCgRGB", 0));
    copy->SetMemberAsInt(L"colSetCgRGB", adv->GetMemberAsInt(L"colSetCgRGB", 0));
    copy->SetMemberAsInt(L"fToneFilter", adv->GetMemberAsInt(L"fToneFilter", 0));
    copy->SetMemberAsStr(L"toneFilter", WString(*adv, L"typeToneFilter").c_str());
    copy->SetMemberAsInt(L"fZoom", adv->GetMemberAsInt(L"fZoom", 0));
    auto* frame = Structure(adv->GetMemberAs(L"msgFrame"));
    if (frame) copy->SetMemberAsInt(L"viewType", frame->GetMemberAsInt(L"viewType", 0));
    auto* bgms = Array(Global(image, L"bgm"));
    auto* bgmIndex = ESLTypeCast<ECSInteger>(Global(image, L"bgmIndex"));
    const auto index = bgmIndex ? bgmIndex->GetValue() : -1;
    auto* bgm = bgms && index >= 0 && static_cast<unsigned long long>(index) < bgms->m_varArray.GetSize() ?
        Structure(bgms->m_varArray.GetAt(unsigned(index))) : nullptr;
    copy->SetMemberAsStr(L"playBgm", bgm ? WString(*bgm, L"filename").c_str() : L"");
    copy->SetMemberAsInt(L"fPauseBgm", bgm ? bgm->GetMemberAsInt(L"fPause", 0) : 0);
    ECSArray environmentSounds;
    if (auto* sounds = ESLTypeCast<ECSHash>(Global(image, L"envSe"))) {
        for (unsigned i = 0; i < sounds->m_varArray.GetSize(); ++i) {
            auto* entry = sounds->m_varArray.GetAt(i);
            auto* info = entry ? Structure(entry->GetObject()) : nullptr;
            if (info && info->GetMemberAsInt(L"fEndingProcess", 0) == 0)
                environmentSounds.m_varArray.Add(new ECSString(static_cast<const wchar_t*>(entry->Tag())));
        }
    }
    if (!CopyMember(context, *copy, L"playEnvSe", &environmentSounds)) return failure("environment-sounds");
    if (!StripTransientBustupSprites(*copy)) return failure("bustup-sanitize");
    EMemoryFile memory;
    if (memory.Create(0x10000) || context.SaveObject(memory, copy) || memory.GetLength() > 2 * 1024 * 1024) return failure("serialize");
    const auto* bytes = static_cast<const std::uint8_t*>(memory.GetBuffer());
    std::vector<std::uint8_t> output(bytes, bytes + memory.GetLength());
    if (guide::IsBoundedNativeState(output)) return output;
    // Only isolated driver runs can export a diagnostic save, next to that
    // driver's files. Never export real player state during ordinary gameplay.
    const auto* driver = std::getenv("ENTISGLS_DRIVER");
    const auto* savedir = std::getenv("ENTISGLS_TEST_SAVEDIR");
    if (driver && *driver && savedir && *savedir) {
        const auto file = fs::u8path(driver).parent_path() / ("guide-capture-" + current.game + ".raw");
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(output.data()), std::streamsize(output.size()));
    }
    return failure("bounded-format");
}
std::unique_ptr<ECSObject> Decode(ECSContext& context, const std::string& game,
                                  const std::vector<std::uint8_t>& bytes, std::wstring& scenario) {
    if (!guide::IsBoundedNativeState(bytes)) return {};
    EMemoryFile memory;
    if (memory.Open(bytes.data(), bytes.size())) return {};
    ECSObject* raw = nullptr;
    if (context.LoadObject(memory, raw)) { delete raw; return {}; }
    std::unique_ptr<ECSObject> object(raw);
    auto* state = Structure(raw);
    if (!state || std::wcscmp(state->GetTypeName(), L"ScenarioObject") ||
        state->GetMemberAsInt(L"version", 0) != (game == "haruka" ? 100 : 200)) return {};
    scenario = WString(*state, L"scenarioCall");
    if (scenario.empty() || scenario.size() > 64 || ChapterOf(game, Utf8(scenario)).empty()) return {};
    const int count = state->GetMemberAsInt(L"hitretCount", -1);
    auto* selections = Array(state->GetMemberAs(L"select"));
    auto* flags = Structure(state->GetMemberAs(L"flag"));
    auto* words = flags ? Array(flags->GetMemberAs(L"flag")) : nullptr;
    if (count < 0 || count > 100000 || !selections || selections->m_varArray.GetSize() > 256 ||
        !words || words->m_varArray.GetSize() != 16) return {};
    for (unsigned i = 0; i < selections->m_varArray.GetSize(); ++i) {
        auto* value = ESLTypeCast<ECSInteger>(Entity(selections->m_varArray.GetAt(i)));
        if (!value || value->GetValue() < 0 || value->GetValue() > 32) return {};
    }
    return object;
}
bool CallLoadFunction(ECSContext& context, ECSExecutionImage& image, const wchar_t* name) {
    const auto* address = image.GetFunctionAddress(name);
    if (!address) return false;
    const unsigned blocks = context.m_stack.m_block.GetSize();
    const unsigned objects = context.m_stack.m_varArray.GetSize();
    auto* last = context.m_stack.m_block.GetLastAt(0);
    const DWORD lastFlags = last ? last->m_dwFlags : 0;
    auto unwind = [&] {
        ECSWideString* name = nullptr;
        while (context.m_stack.m_block.GetSize() > blocks)
            context.m_stack.ReleaseNameBlock(name, context);
        while (context.m_stack.m_varArray.GetSize() > objects) {
            auto* object = context.m_stack.m_varArray.Pop();
            if (object) context.delete_CSObject(object);
        }
        if (auto* frame = context.m_stack.m_block.GetLastAt(0)) frame->m_dwFlags = lastFlags;
        context.m_stack.UpdateCurrentFrame();
        context.SetStatus(ECSContext::xsExecution);
    };
    try {
        if (context.GetStatus() != ECSContext::xsExecution) context.SetStatus(ECSContext::xsExecution);
        ECSObjArray<ECSObject> args;
        if (!context.CallFunction(*address, args)) return true;
    } catch (...) {}
    unwind();
    return false;
}
bool EnterLoad(ECSContext& context, ECSExecutionImage& image) {
    // Original file-9 Load does all of these before ScenarioLoop creates ADV.
    // In particular LeaveLoad later waits for HideLoadEffect's attached sprite.
    for (const auto* name : {L"StopBgm", L"StopEnvSe", L"StopSe", L"StopVoice", L"ShowLoadEffect", L"EnterLoad"})
        if (!CallLoadFunction(context, image, name)) return false;
    return true;
}
void AwaitRestore(bool success, std::string message, const PreparedRestore& target) {
    current.restorePending = true;
    current.restoreSucceeded = success;
    current.restoreMessage = std::move(message);
    Store::Get().SetAction(current.game, "restoring", "正在恢复原作画面与输入等待点", target.nodeId, target.checkpointId);
}
void ClearTraversal() {
    if (!current.game.empty()) Store::Get().ResetTraversal(current.game);
    current.activeChoice.clear(); current.activeScenario.clear(); current.activeOrdinal = -1;
}
std::string AlwaysSpecials(const std::string& game) {
    std::ostringstream json; json << '['; bool first = true;
    size_t count; const auto* list = Specials(game, count);
    for (size_t i = 0; i < count; ++i) {
        const auto& entry = list[i];
        if (std::string(entry.condition) != "always") continue;
        if (!first) json << ','; first = false;
        json << "{\"id\":" << J("special:" + std::string(entry.id)) << ",\"title\":" << J(entry.title)
             << ",\"section\":\"特别内容\",\"unlocked\":true,\"enterable\":true,\"command\":" << J(entry.command)
             << ",\"description\":\"原作默认开放；章节统计待首次进入后读取\"}";
    }
    return json.str() + ']';
}
void RefreshOverview(ECSExecutionImage& image, const Flags& flags) {
    {
        std::lock_guard<std::mutex> lock(permissionsMutex);
        clearedGames[current.game] = flags.gameClear;
    }
    std::ostringstream chapters, endings, specials;
    chapters << '['; endings << '['; specials << '[';
    bool firstChapter = true, firstEnding = true, firstSpecial = true;
    size_t count; const auto* list = Chapters(current.game, count);
    for (size_t i = 0; i < count; ++i) {
        const auto& chapter = list[i];
        unsigned read = 0;
        for (unsigned r = 0; r < chapter.rangeCount; ++r)
            for (unsigned bit = chapter.readRanges[r].first; bit <= chapter.readRanges[r].last; ++bit) read += Bit(flags.read, bit);
        if (!read) continue;
        const bool done = chapter.endingFlag ? Bit(flags.read, chapter.endingFlag) : read == chapter.readTotal;
        if (!firstChapter) chapters << ','; firstChapter = false;
        // Store resolves checkpoint availability for this chapter node in its
        // serializer. The UI never substitutes a naked scenario jump.
        chapters << "{\"id\":" << J("chapter:" + std::string(chapter.id)) << ",\"title\":" << J(chapter.name)
                 << ",\"chapter_node_id\":" << J("chapter-" + std::string(chapter.id))
                 << ",\"unlocked\":true,\"completed\":" << (done ? "true" : "false")
                 << ",\"read\":" << read << ",\"total\":" << chapter.readTotal
                 << ",\"percent\":" << (chapter.readTotal ? 100.0 * read / chapter.readTotal : 0)
                 << ",\"description\":\"只可从实际到达后保留的章节记录恢复\"}";
        if (chapter.endingFlag && done) {
            if (!firstEnding) endings << ','; firstEnding = false;
            endings << "{\"id\":" << J(chapter.routeId) << ",\"title\":" << J(std::string(chapter.name) + "结局")
                    << ",\"chapter_id\":" << J("chapter:" + std::string(chapter.id)) << ",\"completed\":true}";
        }
    }
    const auto* entries = Specials(current.game, count);
    for (size_t i = 0; i < count; ++i) {
        const auto& entry = entries[i];
        if (std::string(entry.condition) != "always" && !flags.gameClear) continue;
        if (!firstSpecial) specials << ','; firstSpecial = false;
        specials << "{\"id\":" << J("special:" + std::string(entry.id)) << ",\"title\":" << J(entry.title)
                 << ",\"section\":\"特别内容\",\"unlocked\":true,\"enterable\":true,\"command\":" << J(entry.command)
                 << ",\"description\":\"使用原作标题入口\"}";
    }
    chapters << ']'; endings << ']'; specials << ']';
    const auto combined = chapters.str() + endings.str() + specials.str();
    if (combined != current.sections) {
        Store::Get().SetOverview(current.game, chapters.str(), endings.str(), specials.str());
        current.sections = combined;
    }
}
void PollPrepared(ECSContext& context, ECSExecutionImage& image, bool safe) {
    if (!safe) return;
    for (const auto* game : {"yosuga", "haruka"}) {
        PreparedRestore ready;
        if (!Store::Get().TakePreparedRestore(game, ready)) continue;
        if (!ready.ok) continue; // Store already published a visible failure.
        std::wstring scenario;
        // The sibling work can contain different script/native class layouts.
        // Inspect its bounded wire format here, then decode only inside that
        // work's fresh VM after the controlled host-session switch.
        if (!guide::IsBoundedNativeState(ready.bytes) || ChapterOf(game, ready.scenario).empty() ||
            (current.game == game &&
             (!Decode(context, game, ready.bytes, scenario) || Utf8(scenario) != ready.scenario))) {
            Store::Get().CompleteRestore(game, false, "选择记录的原生存档结构或版本无效；当前剧情未改变");
            continue;
        }
        auto rollback = Capture(context, image);
        auto* state = Structure(Global(image, L"scObj"));
        const bool currentStory = state && !ChapterOf(current.game, Utf8(WString(*state, L"scenarioCall"))).empty();
        if (currentStory && rollback.empty()) {
            Store::Get().CompleteRestore(game, false, "无法保留当前剧情现场；未执行恢复");
            continue;
        }
        std::lock_guard<std::mutex> lock(transferMutex);
        if (transfer.armed) {
            Store::Get().CompleteRestore(game, false, "另一个恢复请求正在执行，请稍后重试");
            continue;
        }
        transfer.target = std::move(ready);
        transfer.originalGame = current.game;
        transfer.originalResourceHash = Store::Get().ResourceHash(current.game);
        transfer.rollback = std::move(rollback);
        transfer.restoringRollback = false;
        transfer.armed = true;
    }
}
} // namespace

std::string LatestGuideJson(const std::string& game) { return Store::Get().LatestJson(game); }
void BeginGuideSession(ECSContext& context, ECSExecutionImage& image) noexcept {
    try {
        current = Session{};
        current.game = Identify(image);
        if (current.game.empty() || study::launcher::ActiveCompatibilityProfile() != "sphere-2008") return;
        current.root = Root(context);
        const auto path = ScriptPath(context, current.game);
        if (current.root.empty() || path.empty()) {
            Store::Get().SetError(current.game, "无法确定原作资源或独立存档目录，分支恢复未启用"); return;
        }
        Store::Get().ConfigureResourceFile(current.game, current.root, path);
        Store::Get().ResetTraversal(current.game);
        // Both packaged/development layouts keep games/<game>/system/*.csx.
        // Resolve the other work only from this already selected layout.
        const std::string other = current.game == "yosuga" ? "haruka" : "yosuga";
        const auto sibling = fs::u8path(path).parent_path().parent_path().parent_path() / other / "system" /
            (other == "yosuga" ? "yosuga.csx" : "Haruka.csx");
        const auto siblingSave = fs::u8path(current.root).parent_path().parent_path() / other / "savedata";
        std::error_code error;
        if (fs::is_regular_file(sibling, error)) {
            Store::Get().ConfigureResourceFile(other, siblingSave.u8string(), sibling.u8string());
            Store::Get().SetDefaultSpecials(other, AlwaysSpecials(other));
        }
        Flags flags;
        if (ReadFlags(image, flags)) { PublishLiveProgress(current.game, flags); RefreshOverview(image, flags); }
    } catch (...) { Store::Get().SetError(current.game, "攻略进度初始化失败，原游戏可继续运行"); }
}
void PollGuideProgress(ECSContext& context, const std::vector<std::wstring>& chain) noexcept {
    try {
        if (current.game.empty() || !context.m_pcsxi || chain.empty()) return;
        auto& image = *context.m_pcsxi;
        auto* state = Structure(Global(image, L"scObj"));
        auto* adv = Structure(Global(image, L"adv"));
        if (!state || !adv) return;
        const auto scenario = Utf8(WString(*state, L"scenarioCall"));
        const bool title = chain.size() >= 2 && chain.back() == L"GetCommand" && chain[chain.size()-2] == L"Title";
        const auto caller = chain.size() >= 2 ? chain[chain.size()-2] : std::wstring();
        const bool choice = chain.back() == L"GetCommand" && (caller == L"startSelect" || caller == L"AdvScreen::startSelect");
        const bool hitret = chain.back() == L"GetCommand" && (caller == L"hitret" || caller == L"AdvScreen::hitret");
        const bool loading = state->GetMemberAsInt(L"isLoad", 0) != 0;
        if ((title && !current.inTitle) || (loading && !current.loading)) ClearTraversal();
        current.inTitle = title; current.loading = loading;
        if (!loading && (choice || hitret) && current.restorePending) {
            Store::Get().CompleteRestore(current.game, current.restoreSucceeded, current.restoreMessage);
            current.restorePending = false;
        }
        if (!title && !loading && !current.activeChoice.empty() && adv->GetMemberAsInt(L"fSelect", 0) == 0) {
            const int selected = adv->GetMemberAsInt(L"selectResult", 0);
            if (selected > 0 && !loading) Store::Get().Choice(current.game, current.activeChoice, selected);
            current.activeChoice.clear();
        }
        const auto now = Clock::now();
        if (now >= current.nextLive) {
            Flags flags;
            if (ReadFlags(image, flags)) {
                if (!current.haveFlags || flags.read != current.previousFlags.read || flags.cg != current.previousFlags.cg ||
                    flags.recollection != current.previousFlags.recollection || flags.gameClear != current.previousFlags.gameClear) {
                    PublishLiveProgress(current.game, flags); RefreshOverview(image, flags);
                    current.previousFlags = std::move(flags); current.haveFlags = true;
                }
            }
            current.nextLive = now + std::chrono::milliseconds(250);
        }
        const auto chapterId = ChapterOf(current.game, scenario);
        const auto* chapter = chapterId.empty() ? nullptr : Chapter(current.game, chapterId);
        if (!loading && (choice || hitret) && chapter) {
            const int ordinal = state->GetMemberAsInt(L"hitretCount", -1);
            const bool freshChoice = choice && (current.activeScenario != scenario || current.activeOrdinal != ordinal || current.activeChoice.empty());
            const bool freshChapter = hitret && scenario == chapter->entry && ordinal == 0 && !current.chapterSnapshots.count(chapterId);
            if (freshChoice || freshChapter) {
                Observation observation;
                observation.game = current.game; observation.scenario = scenario; observation.chapter = chapterId;
                observation.route = chapter->name; observation.ordinal = ordinal;
                observation.createdAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                observation.kind = choice ? "choice" : "chapter";
                observation.title = std::string(chapter->name) + (choice ? " · 选择" : " · 开始记录");
                if (choice) {
                    auto* options = Array(adv->GetMemberAs(L"selectItem"));
                    if (!options || !options->m_varArray.GetSize() || options->m_varArray.GetSize() > 16) return;
                    std::string signature;
                    for (unsigned i = 0; i < options->m_varArray.GetSize(); ++i) {
                        EWideString text;
                        auto* option = Entity(options->m_varArray.GetAt(i));
                        if (!option || option->OperateString(text)) return;
                        const auto value = Utf8(static_cast<const wchar_t*>(text));
                        if (value.size() > 8192) return;
                        observation.options.push_back({int(i+1), value, true, false});
                        signature += std::to_string(i+1) + "\t" + value + "\n";
                    }
                    const std::vector<std::uint8_t> bytes(signature.begin(), signature.end());
                    observation.nodeId = scenario + "-" + std::to_string(ordinal) + "-" + Store::HashBytes(bytes).substr(0,16);
                } else observation.nodeId = "chapter-" + chapterId;
                const auto began = Clock::now();
                observation.snapshot = Capture(context, image);
                if (observation.snapshot.empty()) {
                    Store::Get().SetError(current.game, "选择已到达，但无法创建有效原生检查点");
                }
                if (choice) {
                    current.activeChoice = observation.nodeId; current.activeScenario = scenario; current.activeOrdinal = ordinal;
                } else current.chapterSnapshots.insert(chapterId);
                Store::Get().Observe(std::move(observation));
                study::platform::LogPrint(study::platform::LogPriority::Info, "EntisGLS",
                    "Guide native capture game=%s scenario=%s kind=%s script_ms=%.3f disk=background",
                    current.game.c_str(), scenario.c_str(), choice ? "choice" : "chapter",
                    std::chrono::duration<double,std::milli>(Clock::now()-began).count());
            }
        }
        PollPrepared(context, image, title || hitret || choice);
    } catch (...) { Store::Get().SetError(current.game, "分支记录暂时不可用，当前游戏状态未改变"); }
}
bool GuideDepartureReady(std::string& game) noexcept {
    std::lock_guard<std::mutex> lock(transferMutex);
    if (!transfer.armed) return false;
    game = transfer.target.game; return true;
}
void RequestGuideRestore(const std::string& game, const std::string& nodeId, const std::string& checkpointId) {
    if (game != "yosuga" && game != "haruka") return;
    Store::Get().PrepareRestore(game, nodeId, checkpointId);
}
void RequestGuideEntry(const std::string& game, const std::string& entryId) {
    if (game != "yosuga" && game != "haruka") return;
    if (entryId.rfind("chapter:",0) == 0) {
        const auto id = entryId.substr(8);
        if (Chapter(game,id)) RequestGuideRestore(game, "chapter-" + id, {});
        return;
    }
    if (entryId.rfind("special:",0) != 0) return;
    const auto id = entryId.substr(8);
    size_t count; const auto* list = Specials(game,count);
    for (size_t i = 0; i < count; ++i) if (id == list[i].id) {
        // Match the original title gate exactly. A terminal read flag alone
        // is not evidence that the original gameClear operation completed.
        if (std::string(list[i].condition) != "always") {
            std::lock_guard<std::mutex> lock(permissionsMutex);
            if (!clearedGames[game]) {
                Store::Get().SetError(game, "此入口尚未解锁"); return;
            }
        }
        host::Request request;
        request.kind = host::Request::Kind::GuideEntry;
        request.game = game == "haruka" ? host::Game::Haruka : host::Game::Yosuga;
        request.command.assign(list[i].command, list[i].command + std::char_traits<char>::length(list[i].command));
        Store::Get().SetAction(game, "pending", "等待原作标题入口");
        host::Bridge::Get().Post(request);
        return;
    }
}
void GuideEntryDelivered(const std::string& game) noexcept {
    try { Store::Get().SetAction(game, "completed", "已从原作标题入口进入"); } catch (...) {}
}
void EndGuideTraversal(const std::string& game) noexcept {
    try {
        if (current.game == game && current.restorePending) {
            Store::Get().CompleteRestore(game, false, "恢复在原作输入等待点出现前被中止；普通存档未改写");
            current.restorePending = false;
        }
        Store::Get().ResetTraversal(game);
    } catch (...) {}
}
bool RestoreGuideSession(ECSContext& context, ECSExecutionImage& image, std::wstring& scenario) noexcept {
    try {
        Transfer pending;
        {
            std::lock_guard<std::mutex> lock(transferMutex);
            if (!transfer.armed || transfer.target.game != Identify(image)) return false;
            pending = std::move(transfer); transfer = Transfer{};
        }
        const bool resourceMatches = Store::ResourceFileMatches(ScriptPath(context, pending.target.game),
                                                                 pending.target.resourceHash);
        auto state = resourceMatches ? Decode(context, pending.target.game, pending.target.bytes, scenario) :
            std::unique_ptr<ECSObject>();
        if (state && !pending.target.scenario.empty() && Utf8(scenario) != pending.target.scenario) state.reset();
        auto* destination = Structure(Global(image, L"scObj"));
        bool okay = state && destination && !destination->Move(context, state.get());
        if (okay) { state.release(); okay = EnterLoad(context, image); }
        if (okay) {
            AwaitRestore(!pending.restoringRollback,
                pending.restoringRollback ? "恢复失败，已回到跳转前的实际剧情现场" : "已恢复到原作的实际输入等待点", pending.target);
            return true;
        }
        if (!pending.rollback.empty()) {
            std::wstring rollbackScenario;
            if (pending.originalGame == pending.target.game) {
                auto backup = Store::ResourceFileMatches(ScriptPath(context, pending.originalGame), pending.originalResourceHash) ?
                    Decode(context, pending.originalGame, pending.rollback, rollbackScenario) : std::unique_ptr<ECSObject>();
                if (backup && destination && !destination->Move(context, backup.get())) {
                    backup.release();
                    if (EnterLoad(context, image)) {
                        scenario = rollbackScenario;
                        AwaitRestore(false,"恢复失败，已回到跳转前的实际剧情现场", pending.target);
                        return true;
                    }
                }
            } else {
                Transfer back;
                back.target.game = pending.originalGame; back.target.bytes = std::move(pending.rollback);
                back.target.resourceHash = pending.originalResourceHash;
                back.target.ok = true; back.originalGame = pending.originalGame;
                back.armed = true; back.restoringRollback = true;
                std::lock_guard<std::mutex> lock(transferMutex); transfer = std::move(back);
            }
        }
        Store::Get().CompleteRestore(pending.target.game,false,"记录恢复失败；普通存档和攻略完成标志未改写");
    } catch (...) { Store::Get().CompleteRestore(Identify(image),false,"原生选择记录无法读取；普通存档未改写"); }
    return false;
}
void GuideObjectLoaded(ECSContext&, ECSObject* object) noexcept {
    try {
        auto* state = Structure(object);
        if (!current.game.empty() && state && !std::wcscmp(state->GetTypeName(), L"ScenarioObject"))
            ClearTraversal();
    } catch (...) {}
}
} // namespace entis::progress
