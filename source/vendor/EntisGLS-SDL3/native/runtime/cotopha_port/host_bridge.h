#pragma once
// Bridge between a hosted game session (the Cotopha script thread) and the
// app shell (the main thread) for the merged Yosuga no Sora / Haruka na Sora
// app. The shell shows its own unified title; each game's original Title()
// keeps running underneath, and the shell drives it by delivering the same
// command IDs its buttons would have produced.
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

class ECSContext;

namespace entis::host {

enum class Game { None, Yosuga, Haruka };

struct Request {
    enum class Kind { TitleCommand, GameCommand, GuideEntry, SwitchGame, Quit } kind = Kind::TitleCommand;
    Game game = Game::None;
    std::wstring command;  // a Title() command ID such as L"ID_START"
};

struct Event {
    enum class Kind {
        SessionStarted,  // a game's runtime is up (before its title)
        TitleIdle,       // the game waits in its own title menu loop
        LeftTitle,       // a delivered command took the game out of its title
        SessionEnded,
        GuideRequested,  // the title's 攻略进度 button (ID_GUIDE) was pressed
    } kind = Kind::SessionStarted;
    Game game = Game::None;
    std::wstring menuPage;  // the title menu form page, e.g. ID_PAGE_MENU_FULL
};

class Bridge {
public:
    static Bridge& Get();

    bool Enabled() const { return enabled_; }
    void Enable(std::function<void(const Event&)> listener);

    // ---- main thread
    void Post(const Request& request);
    Game ActiveGame() const;
    void SetStartupLogoEnabled(bool enabled);
    bool StartupLogoEnabled() const;
    bool QuitRequested() const;
    // True while a game switch or quit is unwinding the script on purpose;
    // GetCommand's error is then expected instead of a script failure.
    bool DepartureRequested() const;

    // ---- script thread
    void BeginSession(Game game);
    void EndSession();
    void EnteringTitle();
    void TitleMenuPage(const std::wstring& page);
    // Called from the native Window.GetCommand. Returns true and sets
    // command when the shell's pending command should be delivered now.
    // Sets leave when the session must end (game switch or quit).
    bool PollCommand(ECSContext& context, std::wstring& command, bool& leave);
    // A command a title button produced. Returns true when the app handles it
    // (ハルカナソラ on Yosuga's title, 結束 on Haruka's, 攻略进度 on both)
    // instead of the game.
    bool FilterCommand(ECSContext& context, const std::wstring& id);
    // After Title() returned: whether the session should end instead of
    // dispatching its return value, and which game should run next.
    bool TakeLeave(Game& next, bool& quit);
    // After a session ended: the game to run next (None to quit the app).
    Game NextSession();

private:
    void Emit(const Event& event);
    static std::vector<std::wstring> CallChain(ECSContext& context);

    bool enabled_ = false;
    std::function<void(const Event&)> listener_;
    mutable std::mutex mutex_;
    Game active_ = Game::None;
    bool inTitle_ = false, idleReported_ = false;
    std::wstring menuPage_;
    std::vector<std::wstring> pending_;
    bool guideEntryPending_ = false;
    std::vector<std::wstring> gameCommands_;
    bool startupLogoEnabled_ = true;
    Game switchTo_ = Game::None;
    bool quit_ = false;
    bool restart_ = false;
};

const wchar_t* GameName(Game game);

}  // namespace entis::host
