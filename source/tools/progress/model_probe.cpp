#include "runtime/cotopha_port/game_progress.h"
#include <iostream>

int main() {
    std::string game;
    unsigned clear, length;
    if (!(std::cin >> game >> clear)) return 2;
    entis::progress::Flags flags;
    flags.gameClear = clear != 0;
    for (auto* words : {&flags.read, &flags.cg, &flags.recollection}) {
        if (!(std::cin >> length) || length > 1500) return 2;
        words->resize(length);
        for (auto& word : *words) if (!(std::cin >> word)) return 2;
    }
    const auto json = entis::progress::BuildProgressJson(game, flags, "save", 12345);
    if (json.empty()) return 3;
    std::cout << json << '\n';
}
