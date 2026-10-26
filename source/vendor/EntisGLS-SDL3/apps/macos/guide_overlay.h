#pragma once
// The in-game 攻略进度 page: drawn inside the game window over the 4:3 picture
// in the games' own 空澄 interface style (it is not a separate macOS window).
// Opened by the title's ID_GUIDE button, the app menu and the settings data
// tab. While it is open it owns mouse and keyboard input. Main thread only.
#import <AppKit/AppKit.h>

namespace entis::mac::guide {
// game: @"yosuga", @"haruka" or nil for the running work.
void Show(NSWindow* window, NSString* game);
void Close();
bool Visible();
// Re-reads the live records (bridge events, periodic refresh).
void Refresh();
void SetDataRoot(NSString* path);
// Development driver verbs (progress-*, close-progress). Returns true when handled.
bool HandleCommand(NSWindow* window, NSString* command);
}
