#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <sakuragl/sakuragl.h>
#include "launcher/runtime_session.h"
#include "runtime/cotopha_port/host_bridge.h"
#include "runtime/cotopha_port/branch_guide_store.h"
#include "launcher/game_config.h"
#include "compatibility/games/known_game.h"
#include "extensions/emote/psb/psb_key_resolver.h"
#include "extensions/emote/psb/psb_key_runtime.h"
#include "launcher/psb_key_settings.h"
#include "launcher/psb_key_dialog.h"
#include "io/save_directory.h"
#include "io/game_files.h"
#include "runtime/cotopha_port/legacy_window_draw.h"
#include "platform/sdl/system.h"
#include "platform/sdl/game_file_metrics.h"
#include "platform/sdl/window.h"
#include "platform/sdl/sdl_sound_player.h"
#include "platform/sdl/sdk_image_codec.h"
#include "platform/sdl/image_codec.h"
#if defined(SDL_PLATFORM_ANDROID)
#include "platform/android/android_game_files.h"
#endif
#if defined(SDL_PLATFORM_IOS)
#include "../ios/ios_launcher.h"
#include "../../tests/integration/ios/ios_presentation_smoke.h"
#endif
#include "platform/gl.h"
#if defined(ENTISGLS_MAC_SHELL)
#include "apps/macos/mac_shell.h"
#endif
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <vector>
#include <stdexcept>

SakuraGL::SGLError sglStaticInitialize() { return SakuraGL::sglErrSuccess; }
SakuraGL::SGLError sglStaticFinalize() { return SakuraGL::sglErrSuccess; }

namespace {
bool cliMode = false;
#if defined(SDL_PLATFORM_IOS)
std::atomic<bool> iosBackground{false};
bool IOSLifecycleEvent(void*, SDL_Event* event) {
    if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND || event->type == SDL_EVENT_DID_ENTER_BACKGROUND) {
        iosBackground.store(true, std::memory_order_release);
        // iOS can suspend us before the next event-poll iteration. Finish any
        // submitted GL commands while the app is still allowed to use GLES.
        if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND && SDL_GL_GetCurrentContext()) glFinish();
    } else if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
        iosBackground.store(false, std::memory_order_release);
    }
    return true;
}
#endif
namespace fs = std::filesystem;
std::string ReadAsset(const std::string& root, const char* relative) {
    const auto path = root.empty() ? std::string(relative) : (fs::path(root) / relative).string();
    size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (!data) throw std::runtime_error("Cannot read launcher compatibility asset: " + path);
    std::string text(static_cast<const char*>(data), size);
    SDL_free(data);
    return text;
}

struct FrameCapture {
    std::string path;
    Uint64 notBefore = 0;
    bool attempted = false, saved = false;
};
struct ScheduledInput {
    double seconds = 0;
    float x = 0, y = 0;
    SDL_Keycode key = 0;
    SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
    Uint64 releaseAt = 0;
    SDL_WindowID window = 0;
    bool pressed = false, released = false;
};
void InjectScheduledInput(std::vector<ScheduledInput>& inputs, Uint64 started) {
    const auto now = SDL_GetTicks();
    for (auto& input : inputs) {
        SDL_Event event{};
        if (!input.pressed && now - started >= input.seconds * 1000) {
            auto* window = SDL_GL_GetCurrentWindow();
            if (!window) continue;
            input.window = SDL_GetWindowID(window);
            if (input.key) {
                event.type = SDL_EVENT_KEY_DOWN;
                event.key.windowID = input.window; event.key.key = input.key;
                event.key.scancode = input.scancode; event.key.down = true;
            } else {
                int width = 0, height = 0;
                SDL_GetWindowSize(window, &width, &height);
                // Keep SDL's queried cursor state consistent with the queued
                // diagnostic events, including native hover/enter processing.
                SDL_WarpMouseInWindow(window, input.x * width, input.y * height);
                SDL_Log("Scheduled mouse target: window=%u size=%dx%d point=%.1f,%.1f at=%llu ms",
                    unsigned(input.window), width, height, double(input.x * width),
                    double(input.y * height), static_cast<unsigned long long>(now - started));
                event.type = SDL_EVENT_MOUSE_MOTION; event.motion.windowID = input.window;
                event.motion.x = input.x * width; event.motion.y = input.y * height;
                SDL_PushEvent(&event);
                event = {};
                event.type = SDL_EVENT_MOUSE_BUTTON_DOWN; event.button.windowID = input.window;
                event.button.button = SDL_BUTTON_LEFT; event.button.down = true; event.button.clicks = 1;
                event.button.x = input.x * width; event.button.y = input.y * height;
            }
            if (!SDL_PushEvent(&event)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Scheduled input failed: %s", SDL_GetError());
                input.pressed = input.released = true; AbortLegacyGame(); return;
            }
            input.pressed = true; input.releaseAt = now + 80;
            SDL_Log("Injected scheduled SDL %s event", input.key ? "key" : "mouse");
        } else if (input.pressed && !input.released && now >= input.releaseAt) {
            if (input.key) {
                event.type = SDL_EVENT_KEY_UP;
                event.key.windowID = input.window; event.key.key = input.key;
                event.key.scancode = input.scancode;
            } else {
                int width = 0, height = 0;
                if (auto* window = SDL_GetWindowFromID(input.window)) SDL_GetWindowSize(window, &width, &height);
                event.type = SDL_EVENT_MOUSE_BUTTON_UP; event.button.windowID = input.window;
                event.button.button = SDL_BUTTON_LEFT; event.button.clicks = 1;
                event.button.x = input.x * width; event.button.y = input.y * height;
            }
            if (!SDL_PushEvent(&event)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Scheduled input failed: %s", SDL_GetError());
                input.released = true; AbortLegacyGame(); return;
            }
            input.released = true;
        }
    }
}
void CaptureFrame(SDL_Window* window, void* userdata) {
    auto& capture = *static_cast<FrameCapture*>(userdata);
    if (capture.path.empty() || capture.attempted || SDL_GetTicks() < capture.notBefore) return;
    capture.attempted = true;
    studysteady::platform::RgbaImage image;
    if (!SDL_GetWindowSizeInPixels(window, &image.width, &image.height)) return;
    image.pixels.resize(size_t(image.width) * image.height * 4);
    GLint framebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    const auto drawable = SDL_GetNumberProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_UIKIT_OPENGL_FRAMEBUFFER_NUMBER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(drawable));
    glReadPixels(0, 0, image.width, image.height, GL_RGBA, GL_UNSIGNED_BYTE, image.pixels.data());
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    const auto stride = size_t(image.width) * 4;
    for (int y = 0; y < image.height / 2; ++y)
        std::swap_ranges(image.pixels.begin() + y * stride,
            image.pixels.begin() + (y + 1) * stride,
            image.pixels.begin() + (image.height - 1 - y) * stride);
    auto* file = SDL_IOFromFile(capture.path.c_str(), "wb");
    if (file) {
        capture.saved = studysteady::platform::EncodeImage(file, image,
            studysteady::platform::ImageEncoding::png);
        if (!SDL_CloseIO(file)) capture.saved = false;
    }
    SDL_Log("Frame capture %s: %s", capture.saved ? "saved" : "failed", capture.path.c_str());
}
// Development driver: when ENTISGLS_DRIVER names a file, each appended line is
// a command for the running game. Coordinates are in the game's logical
// display (for example 800x600):
//   click X Y | rclick X Y | move X Y | wheel N | key NAME | shot PATH | quit
double driverLogicalW = 800, driverLogicalH = 600;
double DriverLogicalWidth() { return driverLogicalW; }
double DriverLogicalHeight() { return driverLogicalH; }
void SetDriverLogicalSize(const std::string& xml) {
    const auto display = xml.find("<display");
    if (display == std::string::npos) return;
    auto attribute = [&](const char* name) -> double {
        const auto at = xml.find(std::string(" ") + name + "=\"", display);
        return at == std::string::npos ? 0 : std::atof(xml.c_str() + at + std::strlen(name) + 3);
    };
    if (attribute("width") > 0 && attribute("height") > 0) {
        driverLogicalW = attribute("width"); driverLogicalH = attribute("height");
    }
}
struct DevDriver {
    std::string path;
    std::uintmax_t consumed = 0;
    bool quit = false;
    // Events are spaced like a person's input: scripts poll every ~33 ms.
    std::vector<std::pair<Uint64, SDL_Event>> pending;
    // A keyboard tap needs its release later, but keyboard events are handed to
    // the SDK window directly: SDL would drop them while the app has no focus.
    struct PendingKey { Uint64 at; SDL_Scancode code; bool down; };
    std::vector<PendingKey> pendingKeys;
    Uint64 busyUntil = 0;
};
SDL_Keycode DriverKey(const std::string& name, SDL_Scancode& scancode) {
    struct Entry { const char* name; SDL_Keycode key; SDL_Scancode code; };
    static const Entry keys[] = {
        {"enter", SDLK_RETURN, SDL_SCANCODE_RETURN}, {"escape", SDLK_ESCAPE, SDL_SCANCODE_ESCAPE},
        {"space", SDLK_SPACE, SDL_SCANCODE_SPACE}, {"up", SDLK_UP, SDL_SCANCODE_UP},
        {"down", SDLK_DOWN, SDL_SCANCODE_DOWN}, {"left", SDLK_LEFT, SDL_SCANCODE_LEFT},
        {"right", SDLK_RIGHT, SDL_SCANCODE_RIGHT}, {"ctrl", SDLK_LCTRL, SDL_SCANCODE_LCTRL},
        {"tab", SDLK_TAB, SDL_SCANCODE_TAB}, {"backspace", SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE},
        {"pageup", SDLK_PAGEUP, SDL_SCANCODE_PAGEUP}, {"pagedown", SDLK_PAGEDOWN, SDL_SCANCODE_PAGEDOWN},
    };
    for (const auto& entry : keys)
        if (name == entry.name) { scancode = entry.code; return entry.key; }
    if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0]))) {
        const char c = char(std::tolower(static_cast<unsigned char>(name[0])));
        scancode = SDL_GetScancodeFromKey(SDL_Keycode(c), nullptr);
        return SDL_Keycode(c);
    }
    scancode = SDL_SCANCODE_UNKNOWN;
    return 0;
}
void RunDriverCommand(DevDriver& driver, const std::string& line, FrameCapture& capture) {
    char verb[16]{}; char text[1024]{};
    double x = 0, y = 0;
    if (std::sscanf(line.c_str(), "%15s", verb) != 1) return;
    auto* window = SDL_GL_GetCurrentWindow();
    const std::string command = verb;
    if (command == "quit") { driver.quit = true; return; }
    if (command == "wait" && std::sscanf(line.c_str(), "%*s %lf", &x) == 1) {
        driver.busyUntil = SDL_GetTicks() + Uint64(x * 1000); return;
    }
    if (command == "hub") {
        // hub <yosuga|haruka> <Title command ID>: what a click on the unified title sends.
        char game[32]{}, id[128]{};
        if (std::sscanf(line.c_str(), "%*s %31s %127s", game, id) == 2) {
            entis::host::Request request;
            request.game = std::string(game) == "haruka" ? entis::host::Game::Haruka : entis::host::Game::Yosuga;
            request.command.assign(id, id + std::strlen(id));
            request.kind = entis::host::Bridge::Get().ActiveGame() == request.game
                ? entis::host::Request::Kind::TitleCommand : entis::host::Request::Kind::SwitchGame;
            entis::host::Bridge::Get().Post(request);
        }
        return;
    }
#if defined(ENTISGLS_MAC_SHELL)
    if (command == "shell" && std::sscanf(line.c_str(), "%*s %1023[^\n]", text) == 1) {
        entis::mac::HandleShellCommand(text);
        return;
    }
    if (command == "uishot" && std::sscanf(line.c_str(), "%*s %1023[^\n]", text) == 1) {
        entis::mac::SaveOverlaySnapshot(text);
        return;
    }
    if (command == "menu" && std::sscanf(line.c_str(), "%*s %1023[^\n]", text) == 1) {
        entis::mac::DumpMenus(text);
        return;
    }
#endif
    if (command == "game" && std::sscanf(line.c_str(), "%*s %1023s", text) == 1) {
        entis::host::Request request;
        request.kind = entis::host::Request::Kind::GameCommand;
        request.command.assign(text, text + std::strlen(text));
        entis::host::Bridge::Get().Post(request);
        return;
    }
    if (command == "state" && std::sscanf(line.c_str(), "%*s %1023[^\n]", text) == 1 && window) {
        int w = 0, h = 0, pw = 0, ph = 0;
        SDL_GetWindowSize(window, &w, &h);
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        std::ofstream out(text);
        out << "{\"game\":\"" << (entis::host::Bridge::Get().ActiveGame() == entis::host::Game::Haruka ? "haruka" : "yosuga")
            << "\",\"width\":" << w << ",\"height\":" << h << ",\"pixelWidth\":" << pw
            << ",\"pixelHeight\":" << ph << ",\"fullscreen\":"
            << ((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) ? "true" : "false") << "}\n";
        return;
    }
    if (command == "shot" && std::sscanf(line.c_str(), "%*s %1023[^\n]", text) == 1) {
        capture.path = text; capture.attempted = false; capture.notBefore = 0;
        study::platform::sdl::RequestForcedFrame();
        return;
    }
    if (!window) return;
    const SDL_WindowID id = SDL_GetWindowID(window);
    int width = 0, height = 0;
    SDL_GetWindowSize(window, &width, &height);
    // The logical display keeps its aspect ratio inside the window.
    const double logicalW = DriverLogicalWidth(), logicalH = DriverLogicalHeight();
    const double scale = std::min(width / logicalW, height / logicalH);
    const double offsetX = (width - logicalW * scale) / 2, offsetY = (height - logicalH * scale) / 2;
    auto point = [&](double lx, double ly, float& px, float& py) {
        px = float(offsetX + lx * scale); py = float(offsetY + ly * scale);
    };
    SDL_Event event{};
    if ((command == "click" || command == "rclick" || command == "move") &&
        std::sscanf(line.c_str(), "%*s %lf %lf", &x, &y) == 2) {
        float px, py; point(x, y, px, py);
        SDL_WarpMouseInWindow(window, px, py);
        event.type = SDL_EVENT_MOUSE_MOTION; event.motion.windowID = id; event.motion.x = px; event.motion.y = py;
        SDL_PushEvent(&event);
        const Uint64 now = SDL_GetTicks();
        if (command == "move") { driver.busyUntil = now + 100; return; }
        const Uint8 button = command == "click" ? SDL_BUTTON_LEFT : SDL_BUTTON_RIGHT;
        event = {}; event.type = SDL_EVENT_MOUSE_BUTTON_DOWN; event.button.windowID = id;
        event.button.button = button; event.button.down = true; event.button.clicks = 1;
        event.button.x = px; event.button.y = py;
        driver.pending.push_back({now + 120, event});
        event.type = SDL_EVENT_MOUSE_BUTTON_UP; event.button.down = false;
        driver.pending.push_back({now + 220, event});
        driver.busyUntil = now + 300;
        return;
    }
    if (command == "wheel" && std::sscanf(line.c_str(), "%*s %lf", &y) == 1) {
        event.type = SDL_EVENT_MOUSE_WHEEL; event.wheel.windowID = id; event.wheel.y = float(y);
        event.wheel.integer_y = int(y);
        SDL_PushEvent(&event);
        return;
    }
    if ((command == "key" || command == "keydown" || command == "keyup") &&
        std::sscanf(line.c_str(), "%*s %1023s", text) == 1) {
        SDL_Scancode scancode;
        if (!DriverKey(text, scancode)) { SDL_Log("Driver: unknown key %s", text); return; }
        if (command == "keyup") { study::platform::sdl::InjectKeyEvent(int(scancode), false); return; }
        study::platform::sdl::InjectKeyEvent(int(scancode), true);
        if (command == "keydown") return;
        const Uint64 now = SDL_GetTicks();
        driver.pendingKeys.push_back({now + 100, scancode, false});
        driver.busyUntil = now + 180;
        return;
    }
    SDL_Log("Driver: unknown command: %s", line.c_str());
}
void PollDriver(DevDriver& driver, FrameCapture& capture) {
    if (driver.path.empty()) return;
    const Uint64 now = SDL_GetTicks();
    for (auto it = driver.pending.begin(); it != driver.pending.end();) {
        if (now >= it->first) { SDL_PushEvent(&it->second); it = driver.pending.erase(it); }
        else ++it;
    }
    for (auto it = driver.pendingKeys.begin(); it != driver.pendingKeys.end();) {
        if (now >= it->at) {
            study::platform::sdl::InjectKeyEvent(int(it->code), it->down);
            it = driver.pendingKeys.erase(it);
        } else ++it;
    }
    if (now < driver.busyUntil) return;
    // Wait for the previous screenshot before running the next command.
    if (!capture.path.empty() && !capture.attempted) { study::platform::sdl::RequestForcedFrame(); return; }
    std::ifstream in(driver.path, std::ios::binary);
    if (!in) return;
    in.seekg(0, std::ios::end);
    const auto size = std::uintmax_t(in.tellg());
    if (size <= driver.consumed) return;
    in.seekg(std::streamoff(driver.consumed));
    std::string line;
    if (!std::getline(in, line) || in.eof()) return;  // only complete lines
    driver.consumed += line.size() + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    SDL_Log("Driver: %s", line.c_str());
    RunDriverCommand(driver, line, capture);
}

#if !defined(SDL_PLATFORM_ANDROID) && !defined(SDL_PLATFORM_IOS)
bool ChooseGameDirectory(std::string& path) {
    struct Selection { std::string path; std::atomic<bool> done{false}; } selection;
    SDL_ShowOpenFolderDialog([](void* userdata, const char* const* files, int) {
        auto& choice = *static_cast<Selection*>(userdata);
        if (files && files[0]) choice.path = files[0];
        choice.done.store(true, std::memory_order_release);
    }, &selection, nullptr, nullptr, false);
    while (!selection.done.load(std::memory_order_acquire)) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {}
        SDL_Delay(10);
    }
    if (selection.path.empty()) return false;
    path = selection.path;
    return true;
}
bool ChooseLibraryGame(const fs::path& library, const fs::path& legacy, std::string& path) {
    std::vector<std::string> games;
    auto add = [&](const std::string& value) {
        if (!value.empty() && value.find('\n') == std::string::npos && fs::is_directory(value) &&
            std::find(games.begin(), games.end(), value) == games.end()) games.push_back(value);
    };
    std::ifstream input(library); std::string value;
    while (std::getline(input, value)) add(value);
    if (games.empty()) { std::ifstream previous(legacy / "game-directory.txt"); if (std::getline(previous, value)) add(value); }
    if (!games.empty()) {
        std::vector<SDL_MessageBoxButtonData> buttons;
        // Display paths as well as names so equally named directories are distinguishable.
        for (size_t i = 0; i < games.size(); ++i) buttons.push_back({0, int(i + 1), games[i].c_str()});
        buttons.push_back({SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Choose another game folder"});
        buttons.push_back({SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, -1, "Cancel"});
        SDL_MessageBoxData box{SDL_MESSAGEBOX_INFORMATION, nullptr, "EntisGLS Launcher",
            "Choose a game (traditional Cotopha CSX runtime)", int(buttons.size()), buttons.data(), nullptr};
        int chosen = -1;
        if (!SDL_ShowMessageBox(&box, &chosen)) throw std::runtime_error(SDL_GetError());
        if (chosen < 0) return false;
        if (chosen > 0) { path = games.at(size_t(chosen - 1)); return true; }
    }
    if (!ChooseGameDirectory(path)) return false;
    // A selection is remembered only after the native configuration is validated.
    return true;
}
void RememberGame(const fs::path& library, const std::string& path) {
    if (path.find('\n') != std::string::npos || path.find('\r') != std::string::npos) return;
    std::ifstream input(library); std::string existing;
    while (std::getline(input, existing)) if (existing == path) return;
    std::ofstream output(library, std::ios::app); output << path << '\n';
}
#endif
}

// The merged Yosuga no Sora + Haruka na Sora app: hubRoot holds yosuga.xml,
// haruka.xml and both game folders. One game runs at a time under the app's
// unified title (entis::host::Bridge); the window stays open across switches.
static int RunHubApplication(const fs::path& hubRoot, const fs::path& dataRoot) {
    using namespace study::platform::sdl;
    using entis::host::Bridge;
    using entis::host::Game;
    auto readText = [](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("Cannot read " + path.u8string());
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    entis::launcher::GameLaunchConfig configs[2] = {
        entis::launcher::NormalizeGameConfig(hubRoot, readText(hubRoot / "yosuga.xml"), "yosuga.xml"),
        entis::launcher::NormalizeGameConfig(hubRoot, readText(hubRoot / "haruka.xml"), "haruka.xml")};
    SystemPaths paths;
    paths.gameRoot = hubRoot.string();
    paths.storageRoot = paths.localRoot = dataRoot.string();
    paths.assetsRoot = (fs::path(SDL_GetBasePath()) / "assets").string();
    for (const auto& config : configs) {
        for (const auto& warning : config.warnings) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", warning.c_str());
        fs::create_directories(dataRoot / config.saveFolder / "savedata");
    }
    if (!ConfigureSystemPaths(paths)) return 2;
    SakuraGL::Initialize();
    SakuraGL::RegisterSDLSoundPlayer();
    SakuraGL::RegisterSDLImageDecoder();
    SetWindowDrawHandler(&StudySteadyDrawWindow);
#if defined(ENTISGLS_MAC_SHELL)
    entis::mac::ConfigureDataRoot(dataRoot.string().c_str());
    Bridge::Get().SetStartupLogoEnabled(entis::mac::StartupLogoEnabled());
    SetWindowCreatedHook(&entis::mac::AttachToWindow);
    Bridge::Get().Enable(&entis::mac::OnHostEvent);
#else
    Bridge::Get().Enable([](const entis::host::Event&) {});
#endif
    entis::launcher::SetGamePsbKeyResolver(std::make_shared<entis::launcher::PsbKeyResolver>(
        paths.gameRoot, paths.localRoot, std::nullopt));
    KeepWindowAcrossDisplays(true);
    std::atomic<bool> finished{false};
    std::thread runtime([&] {
        Game game = Game::Yosuga;
        // A bounded test entry point; production always boots Yosuga.
        if (SDL_getenv("ENTISGLS_DRIVER") && SDL_getenv("ENTISGLS_TEST_SAVEDIR")) {
            if (const char* initial = SDL_getenv("ENTISGLS_TEST_GAME"); initial && std::string(initial) == "haruka")
                game = Game::Haruka;
        }
        while (game != Game::None) {
            const auto& config = configs[game == Game::Haruka ? 1 : 0];
            SDL_Log("Hub: starting %s", config.title.c_str());
            const int result = RunLegacyGame(L"", &config);
            if (result) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Hub: %s ended with %d: %s",
                                     config.title.c_str(), result, LegacyGameLastError().c_str());
            game = Bridge::Get().NextSession();
        }
        SSystem::SThread::ReleaseLocalStorage();
        finished.store(true, std::memory_order_release);
    });
    FrameCapture capture;
    SetFrameObserver(CaptureFrame, &capture);
    DevDriver driver;
    SetDriverLogicalSize(configs[0].normalizedXml);
    if (const char* driverPath = SDL_getenv("ENTISGLS_DRIVER")) {
        driver.path = driverPath;
        std::error_code ignored;
        driver.consumed = fs::exists(driver.path, ignored) ? fs::file_size(driver.path, ignored) : 0;
    }
    bool quitting = false;
    Uint64 quitStarted = 0;
    bool forcedQuit = false;
    auto requestQuit = [&] {
        if (quitting) return;
        quitting = true;
        quitStarted = SDL_GetTicks();
        Bridge::Get().Post({entis::host::Request::Kind::Quit});
        SDL_Log("Hub: requested graceful exit");
    };
    while (!finished.load(std::memory_order_acquire)) {
        PollDriver(driver, capture);
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                requestQuit();
                continue;  // Avoid the game's separate exit-confirmation dialog.
            }
#if defined(ENTISGLS_MAC_SHELL)
            // Cocoa queues SDL keys before AppKit handles menus/local monitors.
            // Those native handlers own these downs; keep ups and legacy Ctrl.
            if (event.type == SDL_EVENT_KEY_DOWN) {
                SDL_Window* gameWindow = SDL_GL_GetCurrentWindow();
                const auto mods = event.key.mod;
                if (gameWindow && event.key.windowID == SDL_GetWindowID(gameWindow) &&
                    ((mods & SDL_KMOD_GUI) ||
                     (!(mods & (SDL_KMOD_GUI | SDL_KMOD_CTRL | SDL_KMOD_ALT)) &&
                      (event.key.key == SDLK_A || event.key.key == SDLK_H)))) continue;
            }
#endif
            ProcessWindowEvent(event);
        }
        if (driver.quit || Bridge::Get().QuitRequested()) requestQuit();
        if (quitting && !forcedQuit && SDL_GetTicks() - quitStarted >= 2000) {
            forcedQuit = true;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Hub: graceful exit timed out; interrupting script");
            AbortLegacyGame();
        }
        DrawWindows();
        SDL_Delay(1);
    }
    runtime.join();
    // All queued guide writes must finish before a confirmed data clear, and
    // before exiting with a newly recorded choice still pending on disk.
    entis::guide::Store::Get().Flush();
#if defined(ENTISGLS_MAC_SHELL)
    entis::mac::ClearSavedDataAfterExit();
#endif
    KeepWindowAcrossDisplays(false);
    ReleaseParkedWindow();
    entis::launcher::SetGamePsbKeyResolver(nullptr);
    SetFrameObserver(nullptr, nullptr);
    SetWindowDrawHandler(nullptr);
    SakuraGL::Finalize();
    CollectRetiredWindows();
    return 0;
}

static int RunApplication(int argc, char** argv) {
    using namespace study::platform::sdl;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    // The packaged merged app carries both games in Resources/games.
    fs::path hubRoot;
    if (const char* hub = SDL_getenv("ENTISGLS_HUB_ROOT")) hubRoot = hub;
    else if (const char* base = SDL_GetBasePath(); base && fs::exists(fs::path(base) / "games" / "yosuga.xml"))
        hubRoot = fs::path(base) / "games";
    if (!hubRoot.empty()) {
        const char* preference = SDL_GetPrefPath("Sphere", "YosugaNoSora");
        fs::path data = preference ? fs::path(preference) : hubRoot / "appdata";
        SDL_free(const_cast<char*>(preference));
        if (const char* testSaves = SDL_getenv("ENTISGLS_TEST_SAVEDIR"); testSaves && *testSaves) {
            data = fs::absolute(testSaves);
            SDL_Log("Hub: isolated test data directory: %s", data.string().c_str());
        }
        const int result = RunHubApplication(hubRoot, data);
        SDL_Quit();
        return result;
    }
    InstallStdoutLogging();
#if defined(SDL_PLATFORM_IOS)
    if (cliMode) {
        std::fputs("IOS_DIAGNOSTIC_SDL_READY\n", stderr);
        std::fflush(stderr);
    }
    iosBackground.store(false, std::memory_order_release);
    if (!SDL_AddEventWatch(IOSLifecycleEvent, nullptr)) throw std::runtime_error(SDL_GetError());
#endif
    SystemPaths paths;
    const char* preference = SDL_GetPrefPath("EntisGLS", "Launcher");
    if (!preference) { SDL_Quit(); return 1; }
    paths.localRoot = preference;
    SDL_free(const_cast<char*>(preference));
    paths.storageRoot = paths.localRoot;
#if defined(SDL_PLATFORM_ANDROID)
    paths.storageRoot = SDL_GetAndroidExternalStoragePath();
    paths.localRoot = SDL_GetAndroidInternalStoragePath();
    paths.gameRoot = paths.storageRoot + "/game";
#else
    paths.assetsRoot = (std::filesystem::path(SDL_GetBasePath()) / "assets").string();
    paths.gameRoot = (std::filesystem::path(SDL_GetBasePath()) / "game").string();
#if defined(SDL_PLATFORM_IOS)
    const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    if (!documents) throw std::runtime_error(SDL_GetError());
    paths.storageRoot = documents;
#endif
#endif
    std::wstring runtimeArguments;
    double exitAfter = 0;
    FrameCapture capture;
    std::vector<ScheduledInput> scheduledInputs;
    double captureAfter = 10;
    bool explicitGameDirectory = false, explicitLocalDirectory = false;
    bool inspectOnly = false;
    std::string explicitConfig;
    std::optional<uint32_t> commandLineKey;
    std::optional<std::string> saveKey;
    bool configureGame = false;
#if defined(SDL_PLATFORM_IOS)
    bool librarySmoke = false, presentationSmoke = false;
#endif
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--legacy-local-data") { /* Accepted for older launch scripts; save location is always game/savedata. */ }
#if defined(SDL_PLATFORM_IOS)
        else if (argument == "--library-smoke") { librarySmoke = true; cliMode = true; }
        else if (argument == "--ios-presentation-smoke") { presentationSmoke = true; cliMode = true; }
#endif
        else if (argument == "--inspect-game") { inspectOnly = true; cliMode = true; }
        else if (argument == "--trace-file-io") SetGameFileMetricsEnabled(true);
        else if (argument == "--configure-game") configureGame = true;
        else if (i + 1 < argc && argument == "--psb-key") {
            commandLineKey = entis::launcher::ParsePsbKey(argv[++i]);
            if (!commandLineKey) throw std::runtime_error("--psb-key requires an unsigned integer");
        } else if (i + 1 < argc && argument == "--save-psb-key") { saveKey = argv[++i]; cliMode = true; }
        else if (i + 1 < argc && argument == "--config") explicitConfig = argv[++i];
        else if (argument == "--self-test" || argument == "--window-probe" ||
            argument == "--emote-probe" || argument == "--image-export-probe" ||
            argument == "--opening-probe") {
            runtimeArguments.assign(argument.begin(), argument.end()); cliMode = true;
        } else if (i + 1 < argc && argument == "--exit-after") {
            exitAfter = std::stod(argv[++i]); cliMode = true;
        } else if (i + 1 < argc && argument == "--capture-frame") {
            capture.path = std::filesystem::absolute(argv[++i]).string();
        } else if (i + 1 < argc && argument == "--capture-after") {
            captureAfter = std::stod(argv[++i]);
        } else if (i + 1 < argc && argument == "--click-at") {
            ScheduledInput input;
            if (std::sscanf(argv[++i], "%lf,%f,%f", &input.seconds, &input.x, &input.y) != 3 ||
                input.seconds < 0 || input.x < 0 || input.x > 1 || input.y < 0 || input.y > 1)
                throw std::runtime_error("--click-at expects seconds,x,y with normalized coordinates");
            scheduledInputs.push_back(input);
        } else if (i + 1 < argc && argument == "--key-at") {
            ScheduledInput input; char key[32]{};
            if (std::sscanf(argv[++i], "%lf,%31s", &input.seconds, key) != 2 || input.seconds < 0)
                throw std::runtime_error("--key-at expects seconds,enter|escape|space");
            const std::string name = key;
            if (name == "enter") { input.key = SDLK_RETURN; input.scancode = SDL_SCANCODE_RETURN; }
            else if (name == "escape") { input.key = SDLK_ESCAPE; input.scancode = SDL_SCANCODE_ESCAPE; }
            else if (name == "space") { input.key = SDLK_SPACE; input.scancode = SDL_SCANCODE_SPACE; }
            else throw std::runtime_error("Unknown scheduled key");
            scheduledInputs.push_back(input);
        } else if (i + 1 < argc && (argument == "--game-dir" || argument == "--assets-dir" ||
                   argument == "--storage-dir" || argument == "--local-dir")) {
            const auto path = std::filesystem::absolute(argv[++i]).lexically_normal().string();
            if (argument == "--game-dir") { paths.gameRoot = path; explicitGameDirectory = true; }
            else if (argument == "--assets-dir") paths.assetsRoot = path;
            else if (argument == "--storage-dir") paths.storageRoot = path;
            else { paths.localRoot = path; explicitLocalDirectory = true; }
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Unknown/incomplete argument: %s", argv[i]);
            SDL_Quit(); return 2;
        }
    }
#if defined(SDL_PLATFORM_IOS)
    if (presentationSmoke) {
        const int result = RunIOSPresentationSmoke();
        SDL_Quit();
        return result;
    }
#endif
    const fs::path applicationData = paths.localRoot;
    fs::create_directories(applicationData);
    fs::path legacyData;
#if defined(SDL_PLATFORM_IOS)
    if (!explicitGameDirectory && !ChooseIOSLibraryGame(paths.storageRoot, paths.gameRoot,
            librarySmoke ? (exitAfter > 0 ? exitAfter : 3) : 0)) {
        SDL_Quit(); return 0;
    }
#endif
#if !defined(SDL_PLATFORM_ANDROID) && !defined(SDL_PLATFORM_IOS)
    if (char* old = SDL_GetPrefPath("StudySteady", "StudySteady")) { legacyData = old; SDL_free(old); }
    const auto library = applicationData / "game-library.txt";
    if (!explicitGameDirectory && !ChooseLibraryGame(library, legacyData, paths.gameRoot)) {
        SDL_Quit(); return 0;
    }
#endif
#if defined(SDL_PLATFORM_ANDROID)
    ConfigureAndroidGameFiles(paths.gameRoot);
    struct GameFilesSession { ~GameFilesSession() { entis::io::SetBackend({}, {}); } } gameFilesSession;
#endif
    const bool knownStudySteady = study::launcher::IsKnownStudySteady(paths.gameRoot);
    entis::launcher::GameLaunchConfig game;
    try { game = entis::launcher::DiscoverGame(paths.gameRoot, explicitConfig); }
    catch (const entis::launcher::ConfigError&) {
        // NOA-only imports made by old APKs have no Windows executable/config.
        // Never replace a user's invalid XML or explicit selection with a template.
        bool hasConfiguration = !explicitConfig.empty() || entis::io::Stat(fs::path(paths.gameRoot) / "entis-launcher.xml").kind != entis::io::FileInfo::Kind::Missing ||
            entis::io::Stat(fs::path(paths.gameRoot) / "cotopha.xml").kind != entis::io::FileInfo::Kind::Missing;
        if (entis::io::Stat(paths.gameRoot).kind == entis::io::FileInfo::Kind::Directory) for (const auto& entry : entis::io::List(paths.gameRoot)) {
            auto extension = fs::u8path(entry.name).extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (extension == ".exe") hasConfiguration = true;
        }
        if (!knownStudySteady || hasConfiguration) throw;
        game = entis::launcher::NormalizeGameConfig(paths.gameRoot,
            ReadAsset(paths.assetsRoot, "compatibility/study-steady-r18.xml"), "verified study-steady-r18 compatibility template");
    }
    if (knownStudySteady && game.compatibilityProfile.empty()) game.compatibilityProfile = "study-steady-r18";
    if (knownStudySteady && game.compatibilityProfile == "study-steady-r18" && !game.explicitGameId) game.gameId = "study-steady-r18";
    paths.localRoot = (applicationData / "games" / game.gameId).string();
    SDL_Log("Game: %s; id=%s; entry=%s; profile=%s; source=%s; saves=%s/savedata",
        game.title.c_str(), game.gameId.c_str(), game.entryScript.c_str(), game.compatibilityProfile.c_str(),
        game.configSource.c_str(), (game.savesInAppData ? paths.localRoot : paths.gameRoot).c_str());
    for (const auto& warning : game.warnings) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", warning.c_str());
#if !defined(SDL_PLATFORM_ANDROID) && !defined(SDL_PLATFORM_IOS)
    if (!explicitGameDirectory) RememberGame(library, paths.gameRoot);
#endif
    if (inspectOnly) { std::puts(game.normalizedXml.c_str()); SDL_Quit(); return 0; }
    if (saveKey) {
        entis::launcher::WritePsbKeyOverride(paths.localRoot,
            entis::launcher::ParsePsbKey(*saveKey == "auto" ? "" : *saveKey));
        SDL_Log("Per-game PSB override %s", *saveKey == "auto" ? "cleared" : "saved");
        SDL_Quit(); return 0;
    }
#if !defined(SDL_PLATFORM_ANDROID) && !defined(SDL_PLATFORM_IOS)
    if (configureGame) { entis::launcher::ShowPsbKeySettings(paths.localRoot, game.title); SDL_Quit(); return 0; }
    if (!explicitGameDirectory && !cliMode) {
        for (;;) {
            const SDL_MessageBoxButtonData choices[] = {
                {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Launch"},
                {0, 2, "PSB settings"}, {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"}};
            SDL_MessageBoxData box{SDL_MESSAGEBOX_INFORMATION, nullptr, "EntisGLS Launcher", game.title.c_str(), 3, choices, nullptr};
            int selected = 0;
            if (!SDL_ShowMessageBox(&box, &selected)) throw std::runtime_error(SDL_GetError());
            if (selected == 0) { SDL_Quit(); return 0; }
            if (selected == 1) break;
            entis::launcher::ShowPsbKeySettings(paths.localRoot, game.title);
        }
    }
#endif
    auto selectedKey = commandLineKey ? commandLineKey : entis::launcher::ReadPsbKeyOverride(paths.localRoot);
    if (!selectedKey) selectedKey = game.psbKey;
    entis::launcher::SetGamePsbKeyResolver(std::make_shared<entis::launcher::PsbKeyResolver>(paths.gameRoot, paths.localRoot, selectedKey));
    SDL_Log("PSB parameter mode: %s", selectedKey ? "explicit override" : "automatic driver discovery");
    fs::create_directories(paths.localRoot);
    if (game.savesInAppData) fs::create_directories(fs::path(paths.localRoot) / "savedata");
    else entis::launcher::PrepareSaveDirectory(paths.gameRoot);

    if (!ConfigureSystemPaths(paths)) { SDL_Quit(); return 2; }
    SakuraGL::Initialize();
    SakuraGL::RegisterSDLSoundPlayer();
    SakuraGL::RegisterSDLImageDecoder();
    SetWindowDrawHandler(&StudySteadyDrawWindow);
#if defined(ENTISGLS_MAC_SHELL)
    SetWindowCreatedHook(&entis::mac::AttachToWindow);
#endif
    std::atomic<bool> finished{false};
    int result = 1;
    std::string runtimeError;
    std::thread runtime([&] {
        try { result = RunLegacyGame(runtimeArguments.c_str(), &game); runtimeError = LegacyGameLastError(); }
        catch (const std::exception& error) {
            runtimeError = error.what();
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Runtime exception: %s", error.what());
        }
        SSystem::SThread::ReleaseLocalStorage();
        finished.store(true, std::memory_order_release);
    });
    const Uint64 started = SDL_GetTicks();
    capture.notBefore = started + Uint64(std::max(0.0, captureAfter) * 1000);
    SetFrameObserver(CaptureFrame, &capture);
    DevDriver driver;
    SetDriverLogicalSize(game.normalizedXml);
    if (const char* driverPath = SDL_getenv("ENTISGLS_DRIVER")) {
        driver.path = driverPath;
        std::error_code ignored;
        driver.consumed = fs::exists(driver.path, ignored) ? fs::file_size(driver.path, ignored) : 0;
    }
    bool aborting = false;
    bool psbFailure = false;
    while (!finished.load(std::memory_order_acquire)) {
        InjectScheduledInput(scheduledInputs, started);
        PollDriver(driver, capture);
        if (driver.quit && !aborting) { AbortLegacyGame(); aborting = true; }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                AbortLegacyGame(); aborting = true;
            }
            ProcessWindowEvent(event);
        }
        // Keep servicing GL destruction/queued calls even after closing a window.
#if defined(SDL_PLATFORM_IOS)
        // UIKit forbids any GLES work after entering the background, including
        // queued uploads and deletion tasks. Resume servicing them on return.
        if (!iosBackground.load(std::memory_order_acquire)) DrawWindows();
#else
        DrawWindows();
#endif
        if (!psbFailure && entis::launcher::HasGamePsbKeyError()) {
            psbFailure = true;
            AbortLegacyGame();
        }
        if (!aborting && exitAfter > 0 && SDL_GetTicks() - started >= exitAfter * 1000) {
            AbortLegacyGame(); aborting = true;
        }
        SDL_Delay(1);
    }
    runtime.join();
    if (entis::launcher::HasGamePsbKeyError()) {
        psbFailure = true;
        runtimeError = entis::launcher::GamePsbKeyError();
        result = 40;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", runtimeError.c_str());
    }
    entis::launcher::SetGamePsbKeyResolver(nullptr);
    SetFrameObserver(nullptr, nullptr);
    SetWindowDrawHandler(nullptr);
    SakuraGL::Finalize();
    CollectRetiredWindows();
    if (result && (!aborting || psbFailure) && !cliMode) {
        if (runtimeError.empty()) runtimeError = "Runtime returned error " + std::to_string(result);
        const std::string message = game.title + "\n" + runtimeError + (psbFailure
            ? "\nAdd the original E-mote driver to this game's folder, or set its PSB key in the launcher game settings."
            : "\nThis launcher supports the traditional Cotopha CSX runtime. A game may require additional native adapters or plugins.");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "EntisGLS Launcher", message.c_str(), nullptr);
    }
    SDL_Quit();
    return result;
}

int main(int argc, char** argv) {
#if defined(SDL_PLATFORM_IOS)
    // Identify smoke launches before SDL initialization. A startup failure must
    // retain its exit code rather than entering the interactive library retry
    // loop, and this marker distinguishes app startup from simctl attachment.
    for (int i = 1; i < argc; ++i) {
        if (SDL_strcmp(argv[i], "--library-smoke") == 0 ||
            SDL_strcmp(argv[i], "--ios-presentation-smoke") == 0) {
            cliMode = true;
            std::fputs("IOS_DIAGNOSTIC_MAIN_ENTERED\n", stderr);
            std::fflush(stderr);
            break;
        }
    }
    // A failed import/configuration must leave the library usable, and an
    // ordinary game exit returns to it. Diagnostic launches retain exit codes.
    for (;;) {
        try {
            const int result = RunApplication(argc, argv);
            if (cliMode) return result;
        } catch (const std::exception& error) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "EntisGLS Launcher: %s", error.what());
            if (SDL_WasInit(SDL_INIT_VIDEO) && !cliMode)
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "EntisGLS Launcher", error.what(), nullptr);
            SDL_Quit();
            if (cliMode) return 1;
        }
    }
#else
    try { return RunApplication(argc, argv); }
    catch (const std::exception& error) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "EntisGLS Launcher: %s", error.what());
        if (SDL_WasInit(SDL_INIT_VIDEO) && !cliMode)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "EntisGLS Launcher", error.what(), nullptr);
        SDL_Quit();
        return 1;
    }
#endif
}
