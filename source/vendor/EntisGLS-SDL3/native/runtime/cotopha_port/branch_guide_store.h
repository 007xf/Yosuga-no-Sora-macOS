#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace entis::guide {
struct Option {
    int index = 0; // Original one-based option number.
    std::string text;
    bool available = true, chosen = false;
};
struct Observation {
    std::string game, nodeId, kind = "choice", scenario, title, chapter, route;
    int ordinal = 0;
    std::vector<Option> options;
    std::vector<std::uint8_t> snapshot; // Already copied native object bytes.
    std::int64_t createdAt = 0;
};
struct PreparedRestore {
    std::string game, nodeId, checkpointId, scenario, resourceHash, error;
    bool ok = false;
    std::vector<std::uint8_t> bytes;
};

class Store {
public:
    Store();
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    static Store& Get();
    // Only enqueue work: no disk access, hashes or JSON parsing on caller.
    void Configure(std::string game, std::string saveRoot, std::string resourceHash);
    void Configure(std::string game, std::string saveRoot, std::vector<std::uint8_t> resourceBytes);
    void ConfigureResourceFile(std::string game, std::string saveRoot, std::string scriptPath);
    void Observe(Observation observation);
    void Choice(std::string game, std::string nodeId, int option);
    void ResetTraversal(std::string game);
    std::string LatestJson(const std::string& game) const;
    void PrepareRestore(std::string game, std::string nodeId, std::string checkpointId = {});
    bool TakePreparedRestore(const std::string& game, PreparedRestore& result);
    void CompleteRestore(std::string game, bool success, std::string message = {});
    void SetOverview(std::string game, std::string chaptersJson, std::string endingsJson, std::string specialsJson);
    // Before a game's SystemObject has been loaded, only its unconditional
    // original menu entries are known; don't replace a live overview.
    void SetDefaultSpecials(std::string game, std::string specialsJson);
    void SetError(std::string game, std::string message);
    void SetAction(std::string game, std::string state, std::string message,
                   std::string nodeId = {}, std::string checkpointId = {});
    // Tests/shutdown: wait for already queued work; never needed per choice.
    void Flush();
    static std::string HashBytes(const std::vector<std::uint8_t>& bytes);
    static bool ResourceFileMatches(const std::string& path, const std::string& expectedHash) noexcept;
    std::string ResourceHash(const std::string& game) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace entis::guide
