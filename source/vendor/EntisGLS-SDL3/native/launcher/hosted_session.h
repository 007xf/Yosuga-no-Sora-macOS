#pragma once
// Runs a Sphere 2008 game (Yosuga no Sora / Haruka na Sora) under the merged
// app's unified title: Initialize(), then a native version of the game's
// Starting() that skips the boot logos and returns to the shell's title.
#include <cstdint>

class ECSContext;
class ECSExecutionImage;
enum ESLError : intptr_t;

namespace entis::host {
enum class Game;
// Returns the error that ended the session (eslErrSuccess when the shell
// asked to switch games or quit).
ESLError RunHostedSession(ECSContext& context, ECSExecutionImage& image, Game game);
}
