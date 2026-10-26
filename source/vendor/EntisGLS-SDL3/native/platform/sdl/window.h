#pragma once
#include <SDL3/SDL.h>
namespace SakuraGL { class SGLGenericWindow; }

namespace study::platform::sdl {
// Called from the SDL main thread. SDK windows and render contexts live there;
// the Cotopha script runs separately and dispatches platform work to this loop.
void ProcessWindowEvent(const SDL_Event& event);
void DrawWindows();
// Main-thread cleanup for exceptional closes; safe after SDK finalization.
void CollectRetiredWindows();
bool HasWindows();
// Called on the main thread with the app's current GL context, after a
// successful draw and before presentation. Pass nullptr to disable.
void SetFrameObserver(void (*observer)(SDL_Window*, void*), void* user);
void SetWindowDrawHandler(bool (*handler)(SakuraGL::SGLGenericWindow*));
// Draws the next frame even if the window is hidden behind others.
void RequestForcedFrame();
// Development driver only: feeds a key straight to the SDK window. SDL routes
// real keyboard events only to the application that owns keyboard focus, and an
// automated check must not bring the game to the front.
void InjectKeyEvent(int scancode, bool down);
// Called on the main thread after each game window and GL context are created.
void SetWindowCreatedHook(void (*hook)(SDL_Window*));
// Reuse one window for successive displays (one game after another).
void KeepWindowAcrossDisplays(bool keep);
// Main thread, at exit: destroys a window left parked by the last display.
void ReleaseParkedWindow();
}
