#pragma once

#include <Geode/Geode.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace uranium::gdsolver {

struct InputEvent {
    long long tick = 0;
    std::uint8_t buttons = 0; // bit 0: player 1 jump, bit 1: player 2 jump
};

struct SolveJob {
    bool ok = false;
    std::string error;
    std::string level;
    std::string csv;
    std::string planPath;
    std::vector<std::string> args;
};

struct SolveResult {
    bool hasPlan = false;
    bool complete = false;
    long long stopTick = -1;
    std::string message;
    std::vector<InputEvent> inputs;
};

int modeIndex(PlayerObject* player);
std::string modeName(PlayerObject* player);
bool isPlatformer(GJBaseGameLayer* layer);
std::string dataDirectory();
SolveJob prepareSolve(GJBaseGameLayer* layer);
SolveResult executeSolve(SolveJob job);

} // namespace uranium::gdsolver
