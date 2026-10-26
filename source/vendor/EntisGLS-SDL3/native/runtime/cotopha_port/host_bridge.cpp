#include "compatibility/sdk/legacy/gls.h"
#include "runtime/cotopha_port/host_bridge.h"
#include "runtime/cotopha_port/game_progress.h"
#include "platform/log.h"

namespace entis::host {

Bridge& Bridge::Get() {
    static Bridge bridge;
    return bridge;
}

void Bridge::Enable(std::function<void(const Event&)> listener) {
    std::lock_guard<std::mutex> lock(mutex_);
    listener_ = std::move(listener);
    enabled_ = true;
}

const wchar_t* GameName(Game game) {
    switch (game) {
        case Game::Yosuga: return L"yosuga";
        case Game::Haruka: return L"haruka";
        default: return L"none";
    }
}

void Bridge::Emit(const Event& event) {
    std::function<void(const Event&)> listener;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        listener = listener_;
    }
    if (listener) listener(event);
}

void Bridge::Post(const Request& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (request.kind) {
        case Request::Kind::TitleCommand: pending_.push_back(request.command); break;
        case Request::Kind::GameCommand:
            // One action per menu/key gesture. It is consumed or discarded at
            // the next script poll, so it cannot leak into a later dialog.
            if (active_ != Game::None && (request.game == Game::None || request.game == active_) &&
                gameCommands_.empty()) gameCommands_.push_back(request.command);
            break;
        case Request::Kind::GuideEntry:
            if (request.game == Game::None || request.command.empty()) break;
            pending_.clear();
            gameCommands_.clear();
            pending_.push_back(request.command);
            guideEntryPending_ = true;
            // A guide entry is a whitelisted original Title action. Leave an
            // ADV through the host's existing unwind path before entering it.
            if (request.game != active_ || !inTitle_) {
                switchTo_ = request.game;
                restart_ = true;
            }
            break;
        case Request::Kind::SwitchGame:
            switchTo_ = request.game;
            guideEntryPending_ = false;
            pending_.clear();
            gameCommands_.clear();
            if (!request.command.empty()) pending_.push_back(request.command);
            break;
        case Request::Kind::Quit: quit_ = true; break;
    }
}

void Bridge::SetStartupLogoEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    startupLogoEnabled_ = enabled;
}

bool Bridge::StartupLogoEnabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return startupLogoEnabled_;
}

bool Bridge::QuitRequested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return quit_;
}

bool Bridge::DepartureRequested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return quit_ || restart_ || switchTo_ != Game::None;
}

Game Bridge::ActiveGame() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}

void Bridge::BeginSession(Game game) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = game;
        inTitle_ = idleReported_ = false;
        gameCommands_.clear();
        menuPage_.clear();
        if (switchTo_ == game) { switchTo_ = Game::None; restart_ = false; }
    }
    Emit({Event::Kind::SessionStarted, game, {}});
}

void Bridge::EndSession() {
    Game game;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        game = active_;
        active_ = Game::None;
        inTitle_ = idleReported_ = false;
    }
    Emit({Event::Kind::SessionEnded, game, {}});
    if (game != Game::None) entis::progress::EndGuideTraversal(game == Game::Haruka ? "haruka" : "yosuga");
}

void Bridge::EnteringTitle() {
    std::lock_guard<std::mutex> lock(mutex_);
    inTitle_ = true;
    idleReported_ = false;
}

void Bridge::TitleMenuPage(const std::wstring& page) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (inTitle_) menuPage_ = page;
}

std::vector<std::wstring> Bridge::CallChain(ECSContext& context) {
    std::vector<std::wstring> chain;
    for (unsigned i = 0; i < context.m_stack.m_block.GetSize(); ++i) {
        auto* block = context.m_stack.m_block.GetAt(i);
        if (block && block->m_pwstrName)
            chain.emplace_back(static_cast<const wchar_t*>(*block->m_pwstrName));
    }
    return chain;
}

bool Bridge::PollCommand(ECSContext& context, std::wstring& command, bool& leave) {
    leave = false;
    if (!enabled_) return false;
    const auto chain = CallChain(context);
    entis::progress::PollGuideProgress(context, chain);
    std::string guideGame;
    const bool guideRestore = entis::progress::GuideDepartureReady(guideGame);
    // The title menu loop calls the script's GetCommand() directly from
    // Title(); dialogs it opens (config, load) have their own loops.
    const bool titleLoop = chain.size() >= 2 && chain[chain.size() - 1] == L"GetCommand" &&
                           chain[chain.size() - 2] == L"Title";
    const auto caller = chain.size() >= 2 ? chain[chain.size() - 2] : std::wstring{};
    const bool adventureLoop = !chain.empty() && chain.back() == L"GetCommand" &&
        (caller == L"hitret" || caller == L"AdvScreen::hitret" ||
         caller == L"startSelect" || caller == L"AdvScreen::startSelect");
    Event event;
    bool emit = false;
    bool guideEntryDelivered = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (guideRestore) {
            switchTo_ = guideGame == "haruka" ? Game::Haruka : Game::Yosuga;
            restart_ = true;
        }
        if (quit_ || restart_ || (switchTo_ != Game::None && switchTo_ != active_)) {
            leave = true;
            return false;
        }
        if (!gameCommands_.empty()) {
            const auto action = gameCommands_.front();
            gameCommands_.clear();
            if (adventureLoop || (titleLoop && action == L"ID_CONFIG")) command = action;
        }
        if (!titleLoop) {
            if (idleReported_) {
                idleReported_ = false;
                event = {Event::Kind::LeftTitle, active_, menuPage_};
                emit = true;
            }
        } else if (command.empty() && !pending_.empty()) {
            command = pending_.front();
            pending_.erase(pending_.begin());
            guideEntryDelivered = guideEntryPending_;
            guideEntryPending_ = false;
        } else if (!idleReported_) {
            idleReported_ = true;
            event = {Event::Kind::TitleIdle, active_, menuPage_};
            emit = true;
        }
    }
    if (emit) Emit(event);
    if (guideEntryDelivered)
        entis::progress::GuideEntryDelivered(ActiveGame() == Game::Haruka ? "haruka" : "yosuga");
    if (!command.empty())
        study::platform::LogPrint(study::platform::LogPriority::Info, "EntisGLS",
            "Hub delivered %ls to %ls", command.c_str(), GameName(ActiveGame()));
    return !command.empty();
}

bool Bridge::FilterCommand(ECSContext& context, const std::wstring& id) {
    if (!enabled_) return false;
    const auto chain = CallChain(context);
    const bool titleLoop = chain.size() >= 2 && chain[chain.size() - 1] == L"GetCommand" &&
                           chain[chain.size() - 2] == L"Title";
    if (!titleLoop) return false;
    Game game;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        game = active_;
        if (id == L"ID_GUIDE") {}
        else if (active_ == Game::Yosuga && id == L"ID_HARUKA") switchTo_ = Game::Haruka;
        else if (active_ == Game::Haruka && id == L"ID_EXITGAME") switchTo_ = Game::Yosuga;
        else return false;
    }
    study::platform::LogPrint(study::platform::LogPriority::Info, "EntisGLS",
                              "Hub: %ls requested from the title", id.c_str());
    // The shell shows the page on the main thread; the title keeps polling.
    if (id == L"ID_GUIDE") Emit({Event::Kind::GuideRequested, game, {}});
    return true;
}

bool Bridge::TakeLeave(Game& next, bool& quit) {
    bool leaving;
    Game game;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit = quit_;
        next = switchTo_;
        leaving = quit_ || restart_ || (switchTo_ != Game::None && switchTo_ != active_);
        inTitle_ = idleReported_ = false;
        game = active_;
    }
    // Title() has returned; whatever the game does next, the shell's title
    // is no longer what the player should see.
    if (!leaving) Emit({Event::Kind::LeftTitle, game, {}});
    return leaving;
}

Game Bridge::NextSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (quit_) return Game::None;
    return switchTo_;
}

}  // namespace entis::host
