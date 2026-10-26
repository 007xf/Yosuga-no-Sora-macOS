#include "runtime/cotopha_port/game_progress.h"
#include "runtime/cotopha_port/game_progress_data.h"
#include <ctime>
#include <iomanip>
#include <iterator>
#include <locale>
#include <map>
#include <mutex>
#include <sstream>

namespace entis::progress {
namespace {
std::mutex snapshotsMutex;
std::map<std::string, std::string> snapshots;
bool Check(const std::vector<std::uint32_t>& words, unsigned flag) {
    return flag / 32 < words.size() && (words[flag / 32] & (std::uint32_t(1) << (flag % 32)));
}
template <size_t N> unsigned Count(const std::vector<std::uint32_t>& words, const unsigned (&ids)[N]) {
    unsigned found = 0;
    for (const auto id : ids) found += Check(words, id);
    return found;
}
template <size_t R, size_t C, size_t E, size_t D>
std::string Build(const std::string& game, const char* title, const Flags& flags, const char* event,
                  std::int64_t timestamp, const data::Route (&routes)[R],
                  const unsigned (&cg)[C], const unsigned (&recollect)[E],
                  const data::Range (&reads)[D], unsigned tracks) {
    unsigned completed = 0, total = 0;
    for (const auto range : reads) {
        total += range.last - range.first + 1;
        for (unsigned id = range.first; id <= range.last; ++id) completed += Check(flags.read, id);
    }
    std::ostringstream json;
    json.imbue(std::locale::classic());
    json << std::setprecision(12)
         << "{\"schema_version\":1,\"game\":\"" << game << "\",\"title\":\"" << title
         << "\",\"source\":\"" << (std::string(event) == "live" ? "runtime" : "system.dat")
         << "\",\"updated_at_unix\":" << timestamp
         << ",\"snapshot_event\":\"" << (std::string(event) == "live" ? "live" : std::string(event) == "save" ? "save" : "load")
         << "\",\"is_live\":" << (std::string(event) == "live" ? "true" : "false") << ",\"routes\":[";
    for (unsigned i = 0; i < R; ++i) {
        if (i) json << ',';
        json << "{\"id\":\"" << routes[i].id << "\",\"name\":\"" << routes[i].name
             << "\",\"read_flag\":" << routes[i].flag << ",\"complete\":"
             << (Check(flags.read, routes[i].flag) ? "true" : "false") << '}';
    }
    const bool musicOpen = game == "haruka" || flags.gameClear;
    json << "],\"cg\":{\"unlocked\":" << Count(flags.cg, cg) << ",\"total\":" << C
         << "},\"recollection\":{\"unlocked\":" << Count(flags.recollection, recollect) << ",\"total\":" << E
         << "},\"music\":{\"unlocked\":" << (musicOpen ? tracks : 0) << ",\"total\":" << tracks
         << ",\"note\":\"" << (game == "yosuga" ? "任一路线通关后开放全部曲目" : "原作音乐鉴赏默认全部开放")
         << "\"},\"read\":{\"completed\":" << completed << ",\"total\":" << total
         << ",\"percent\":" << (total ? completed * 100.0 / total : 0) << "}}";
    return json.str();
}
} // namespace

std::string BuildProgressJson(const std::string& game, const Flags& flags, const char* event, std::int64_t timestamp) {
    using namespace data;
    if (game == "yosuga") return Build(game, "缘之空", flags, event, timestamp,
        yosugaRoutes, yosugaCg, yosugaRecollection, yosugaRead, 21);
    if (game == "haruka") return Build(game, "悠之空", flags, event, timestamp,
        harukaRoutes, harukaCg, harukaRecollection, harukaRead, 22);
    return {};
}
std::string LatestProgressJson(const std::string& game) {
    std::lock_guard<std::mutex> lock(snapshotsMutex);
    const auto found = snapshots.find(game);
    return found == snapshots.end() ? std::string() : found->second;
}
void PublishLiveProgress(const std::string& game, const Flags& flags) {
    const auto json = BuildProgressJson(game, flags, "live", std::time(nullptr));
    std::lock_guard<std::mutex> lock(snapshotsMutex);
    snapshots[game] = json;
}
} // namespace entis::progress

#ifndef ENTISGLS_PROGRESS_MODEL_ONLY
#include "compatibility/sdk/legacy/gls.h"
#include "compatibility/games/compatibility_profiles.h"
#include "platform/log.h"
#include <cerrno>
#include <cstdio>
#include <filesystem>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#else
#include <unistd.h>
#endif

namespace entis::progress {
namespace {
ECSStructure* Structure(ECSObject* object) {
    return object ? ESLTypeCast<ECSStructure>(ECSObject::GetEntity(object)) : nullptr;
}
bool ReadWords(ECSStructure& root, const wchar_t* name, std::vector<std::uint32_t>& words, size_t expected) {
    auto* flag = Structure(root.GetMemberAs(name));
    auto* object = flag ? flag->GetMemberAs(L"flag") : nullptr;
    auto* array = object ? ESLTypeCast<ECSArray>(ECSObject::GetEntity(object)) : nullptr;
    // Both supplied games allocate exactly these capacities. Reject malformed
    // objects rather than silently treating missing words as unread progress.
    if (!array || array->m_varArray.GetSize() != expected) return false;
    for (unsigned i = 0; i < array->m_varArray.GetSize(); ++i) {
        auto* word = ESLTypeCast<ECSInteger>(ECSObject::GetEntity(array->m_varArray.GetAt(i)));
        if (!word) return false;
        words.push_back(static_cast<std::uint32_t>(word->GetValue()));
    }
    return true;
}
std::string SaveRoot(ECSContext& context) {
    auto* environment = context.GetEnvironment();
    auto* opener = environment ? environment->GetWritableFileOpener() : nullptr;
    SSystem::SString root;
    if (!opener || opener->DirectPathOf(root, L"")) return {};
    for (unsigned i = 0; i < 8 && SSystem::g_defURLOpener.FindScheme(root) >= 0; ++i) {
        SSystem::SString next;
        if (SSystem::g_defURLOpener.DirectPathOf(next, root) || next == root) return {};
        root = next;
    }
    if (SSystem::g_defURLOpener.FindScheme(root) >= 0) return {};
    return root.ToCharArray().GetConstArray();
}
bool AtomicWrite(const std::filesystem::path& root, const std::string& json) {
#if defined(_WIN32)
    // A Windows rename cannot replace an existing destination. Use the native
    // replace operation, with CREATE_NEW to avoid following a preexisting temp.
    static std::atomic<unsigned long long> sequence{0};
    std::filesystem::path staging;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        staging = root / (L".progress." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(sequence.fetch_add(1)) + L".tmp");
        file = CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    if (file == INVALID_HANDLE_VALUE) return false;
    size_t at = 0;
    bool okay = true;
    while (at < json.size()) {
        const size_t remaining = json.size() - at;
        const DWORD requested = static_cast<DWORD>(remaining < 0x100000 ? remaining : 0x100000);
        DWORD wrote = 0;
        if (!WriteFile(file, json.data() + at, requested, &wrote, nullptr) || !wrote) {
            okay = false; break;
        }
        at += wrote;
    }
    if (okay && !FlushFileBuffers(file)) okay = false;
    if (!CloseHandle(file)) okay = false;
    if (okay && !MoveFileExW(staging.c_str(), (root / L"progress.json").c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) okay = false;
    if (!okay) DeleteFileW(staging.c_str());
    return okay;
#else
    std::string staging = (root / ".progress.XXXXXX").string();
    const int fd = mkstemp(staging.data());
    if (fd < 0) return false;
    size_t at = 0;
    bool okay = true;
    while (at < json.size()) {
        const auto wrote = write(fd, json.data() + at, json.size() - at);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) { okay = false; break; }
        at += static_cast<size_t>(wrote);
    }
    if (okay && fsync(fd)) okay = false;
    if (close(fd)) okay = false;
    if (okay && std::rename(staging.c_str(), (root / "progress.json").c_str())) okay = false;
    if (!okay) std::remove(staging.c_str());
    return okay;
#endif
}
} // namespace

void SnapshotSystemObject(ECSContext& context, ECSObject* object, const wchar_t* filename, const char* event) noexcept {
    try {
        if (study::launcher::ActiveCompatibilityProfile() != "sphere-2008" || !filename) return;
        // Only the original global SystemObject file, never save slots/configs.
        std::wstring name(filename);
        if (name != L"system.dat") return;
        auto* root = Structure(object);
        if (!root || !context.m_pcsxi) return;
        const std::string game = context.m_pcsxi->GetFunctionAddress(L"sc00_Z000") ? "yosuga" :
            context.m_pcsxi->GetFunctionAddress(L"scKARAOKE") ? "haruka" : "";
        if (game.empty()) return;
        Flags flags;
        if (!ReadWords(*root, L"readFlag", flags.read, 1500) ||
            !ReadWords(*root, L"cgFlag", flags.cg, 128) ||
            !ReadWords(*root, L"recollectFlag", flags.recollection, 1)) return;
        flags.gameClear = root->GetMemberAsInt(L"gameClear", 0) != 0;
        const auto json = BuildProgressJson(game, flags, event, std::time(nullptr));
        const auto path = SaveRoot(context);
        const bool persisted = !path.empty() && AtomicWrite(std::filesystem::u8path(path), json);
        {
            std::lock_guard<std::mutex> lock(snapshotsMutex);
            snapshots[game] = json;
        }
        study::platform::LogPrint(study::platform::LogPriority::Info, "StudySteady",
            "Progress %s snapshot: game=%s persisted=%d", event, game.c_str(), persisted);
    } catch (...) {
        study::platform::LogPrint(study::platform::LogPriority::Warn, "StudySteady", "Progress snapshot could not be recorded");
    }
}
} // namespace entis::progress
#endif
