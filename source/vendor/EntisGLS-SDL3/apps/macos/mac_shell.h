#pragma once
// Native macOS shell around the game window: the merged app's unified title,
// menus and panels.
struct SDL_Window;
namespace entis::host { struct Event; }

namespace entis::mac {
// Called on the main thread right after the game window and its GL context exist.
void AttachToWindow(SDL_Window* window);
// Bridge events; may arrive on the script thread.
void OnHostEvent(const entis::host::Event& event);
// The hub resolves this path, including an isolated data root during testing.
void ConfigureDataRoot(const char* path);
// A persisted preference, read before the hosted startup sequence.
bool StartupLogoEnabled();
// Main-thread shell actions, also available to the development driver:
// settings[-shortcuts|-data], progress[-yosuga|-haruka], close-settings,
// close-progress, fullscreen, windowed,
// toggle-fullscreen, about; game command IDs are forwarded to the script thread.
// Guide diagnostics: progress-flow/overview/special, progress-select <node>,
// progress-entry <entry>, progress-checkpoint <checkpoint>, progress-restore
// (or progress-enter), progress-cancel-confirm. With an explicit driver,
// progress-dump <absolute path beside that driver>, progress-zoom <scale>,
// progress-fit are available; progress-confirm also requires isolated saves.
void HandleShellCommand(const char* command);
// Only call after the game runtime has joined: normal postprocessing must finish
// before clearing the data that the user confirmed in both native alerts.
bool ClearSavedDataAfterExit();
// Development aid: writes the native overlay layers (not the GL picture) to a PNG.
// If a shell panel is visible, captures that panel's content instead.
void SaveOverlaySnapshot(const char* path);
// Development aid: writes the actual application menu tree for verification.
void DumpMenus(const char* path);
}
