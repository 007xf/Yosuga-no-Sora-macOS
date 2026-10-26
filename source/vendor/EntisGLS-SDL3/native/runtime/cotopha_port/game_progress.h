#pragma once
#include <cstdint>
#include <string>
#include <vector>

class ECSContext;
class ECSObject;
class ECSExecutionImage;

namespace entis::progress {
struct Flags {
    std::vector<std::uint32_t> read, cg, recollection;
    bool gameClear = false;
};
// Pure calculation, also used by tests against the independent Python reader.
std::string BuildProgressJson(const std::string& game, const Flags& flags,
                              const char* event, std::int64_t timestamp);
// Script thread only. Successful system.dat load/save; never calls script code.
// Best-effort sidecar write does not alter the success of the actual game save.
void SnapshotSystemObject(ECSContext&, ECSObject*, const wchar_t* filename, const char* event) noexcept;
// Main thread safe; empty when no successful snapshot exists in this process.
std::string LatestProgressJson(const std::string& game);
void PublishLiveProgress(const std::string& game, const Flags& flags);
// Reached-only branch catalogue and native checkpoint availability (thread safe).
std::string LatestGuideJson(const std::string& game);
// Called by the host after Initialize, before showing a title or restoring.
void BeginGuideSession(ECSContext&, ECSExecutionImage&) noexcept;
// Uses only a prevalidated native ScenarioObject, then original EnterLoad.
// True means the caller should run the normal ScenarioLoop(scenario) read path.
bool RestoreGuideSession(ECSContext&, ECSExecutionImage&, std::wstring& scenario) noexcept;
// Safe script wait hooks, never destructive restore inside the native call.
void PollGuideProgress(ECSContext&, const std::vector<std::wstring>& callChain) noexcept;
// Actual File.LoadObject path only, not speculative checkpoint validation.
void GuideObjectLoaded(ECSContext&, ECSObject*) noexcept;
bool GuideDepartureReady(std::string& game) noexcept;
void RequestGuideRestore(const std::string& game, const std::string& nodeId,
                         const std::string& checkpointId);
void RequestGuideEntry(const std::string& game, const std::string& entryId);
void GuideEntryDelivered(const std::string& game) noexcept;
void EndGuideTraversal(const std::string& game) noexcept;
} // namespace entis::progress
