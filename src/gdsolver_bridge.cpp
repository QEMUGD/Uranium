#include "gdsolver_bridge.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

using namespace geode::prelude;

namespace uranium::gdsolver {
std::string dataDirectory() {
    const auto path = Mod::get()->getSaveDir();
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return path.string();
}
} // namespace uranium::gdsolver

// gdsolver's live exporter is authored as part of namespace p1. It relies on
// the same shared declarations as the upstream mod: InputCmd and raw GD offsets.
#include "../third_party/gdsolver/src/mod/plan_edges.hpp"
#include "../third_party/gdsolver/src/mod/gd_offsets.hpp"

#define DATA_DIR (uranium::gdsolver::dataDirectory())
namespace p1 {
inline void writeResult(const std::string& line) {
    log::info("[Uranium/gdsolver] {}", line);
}
#include "../third_party/gdsolver/src/solver/solver.hpp"
} // namespace p1
#undef DATA_DIR

#include "../third_party/gdsolver/dp/src/dpcore.hpp"

namespace uranium::gdsolver {
namespace {

int groundedState(PlayerObject* player, int mode) {
    if (!player) return 0;
    // GD leaves m_isOnGround set while many flying modes are airborne. For those
    // modes, require the secondary contact flag and a settled vertical velocity.
    if (mode == 1 || mode == 3 || mode == 4 || mode == 7) {
        return player->m_isOnGround2 && std::fabs(static_cast<float>(player->m_yVelocity)) < 0.01f;
    }
    return player->m_isOnGround ? 1 : 0;
}

std::string startState(GJBaseGameLayer* layer) {
    auto* p1 = layer ? layer->m_player1 : nullptr;
    auto* p2 = layer ? layer->m_player2 : nullptr;
    if (!p1) return {};

    const bool dual = layer->m_gameState.m_isDualMode && p2;
    const int mode1 = modeIndex(p1);
    const int mode2 = dual ? modeIndex(p2) : -1;
    const int mini1 = p1->m_vehicleSize < 0.9f ? 1 : 0;
    const int mini2 = dual ? (p2->m_vehicleSize < 0.9f ? 1 : 0) : -1;

    // This matches dp's --start tuple (see dp/src/dp/cli.hpp). The UI opens
    // while paused, so the button is not held at the anchor.
    std::ostringstream out;
    out << std::setprecision(9)
        << 0 << ','
        << p1->getPositionX() << ','
        << p1->getPositionY() << ','
        << static_cast<float>(p1->m_yVelocity) << ','
        << mode1 << ','
        << groundedState(p1, mode1) << ','
        << 0 << ','
        << (p1->m_isUpsideDown ? 1 : 0) << ','
        << mini1 << ','
        << (dual ? 1 : 0) << ','
        << (dual ? p2->getPositionY() : 0.f) << ','
        << (dual ? static_cast<float>(p2->m_yVelocity) : 0.f) << ','
        << (dual && p2->m_isUpsideDown ? 1 : 0) << ','
        << (dual ? groundedState(p2, mode2) : 0) << ','
        << p1->m_playerSpeed << ','
        << 0 << ','
        << (p1->m_isDashing ? 1 : 0) << ','
        << 0 << ','
        << -1 << ','
        << 0 << ','
        << 0 << ','
        << 0 << ','
        << p1->getRotation() << ','
        << 0 << ','
        << (p1->m_maybeIsBoosted ? 1 : 0) << ','
        << mode2 << ','
        << mini2;
    return out.str();
}

void addIfExists(std::vector<std::string>& args, const std::string& flag,
                 const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return;
    args.push_back(flag);
    args.push_back(path.string());
}

} // namespace

int modeIndex(PlayerObject* player) {
    if (!player) return 0;
    if (player->m_isShip) return 1;
    if (player->m_isBall) return 2;
    if (player->m_isBird) return 3;
    if (player->m_isDart) return 4;
    if (player->m_isRobot) return 5;
    if (player->m_isSpider) return 6;
    if (player->m_isSwing) return 7;
    return 0;
}

std::string modeName(PlayerObject* player) {
    switch (modeIndex(player)) {
        case 1: return "Ship";
        case 2: return "Ball";
        case 3: return "UFO";
        case 4: return "Wave";
        case 5: return "Robot";
        case 6: return "Spider";
        case 7: return "Swing";
        default: return "Cube";
    }
}

bool isPlatformer(GJBaseGameLayer* layer) {
    return layer && layer->m_levelSettings && layer->m_levelSettings->m_platformerMode;
}

SolveJob prepareSolve(GJBaseGameLayer* layer) {
    SolveJob job;
    if (!layer || !layer->m_player1 || !layer->m_objects) {
        job.error = "No active level/player to export.";
        return job;
    }
    if (isPlatformer(layer)) {
        job.error = "Platformer mode is not supported by gdsolver (it uses left/right controls).";
        return job;
    }

    std::error_code ec;
    const std::filesystem::path dir(dataDirectory());
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        job.error = "Cannot create solver data folder: " + ec.message();
        return job;
    }

    // Use the upstream live exporter, including portals, pads, orbs, triggers,
    // group mappings, rotated hitboxes, and level settings.
    p1::solver::buildPois(layer, true);
    job.csv = p1::solver::g_objRectsText;
    p1::solver::forgetObjRectsText();
    if (job.csv.empty()) {
        job.error = "gdsolver exported an empty level.";
        return job;
    }

    const dpcore::LevelStats stats = dpcore::statsFromCsv(job.csv);
    if (!stats.unsupported.empty()) {
        job.error = "gdsolver rejected this level: " + stats.unsupported;
        return job;
    }

    job.level = layer->m_level ? std::string(layer->m_level->m_levelName.c_str()) : "unknown";
    job.planPath = (dir / "uranium_plan.txt").string();
    std::filesystem::remove(job.planPath, ec);

    const std::string start = startState(layer);
    if (start.empty()) {
        job.error = "Could not read the player's starting state.";
        return job;
    }

    job.args = {
        "--out", job.planPath,
        "--start", start,
        "--horizon", "36000",
        "--cap", "16000",
        "--threads", "4",
        "--goalgd"
    };
    addIfExists(job.args, "--triggers", dir / "triggers.txt");
    addIfExists(job.args, "--objgroups", dir / "objgroups.txt");
    addIfExists(job.args, "--obb", dir / "obb.txt");
    addIfExists(job.args, "--levelsettings", dir / "levelsettings.txt");
    addIfExists(job.args, "--forceids", dir / "forceblocks.txt");
    addIfExists(job.args, "--areas", dir / "areas.txt");
    addIfExists(job.args, "--rotgameplay", dir / "rotgameplay.txt");

    job.ok = true;
    return job;
}

SolveResult executeSolve(SolveJob job) {
    SolveResult result;
    if (!job.ok) {
        result.message = job.error.empty() ? "Solve job was not prepared." : job.error;
        return result;
    }

    const int rc = dpcore::solveInProcess(job.csv, job.args);
    const dpcore::SolveOutcome outcome = dpcore::outcome();
    result.complete = outcome.verdict == dpcore::OutcomeSolved;
    result.stopTick = outcome.deepT;

    std::ifstream plan(job.planPath);
    if (plan) {
        std::string line;
        while (std::getline(plan, line)) {
            long long tick = 0;
            int buttons = 0;
            if (std::sscanf(line.c_str(), "input=%lld,%d", &tick, &buttons) != 2) continue;
            if (tick < 0) continue;
            result.inputs.push_back({tick, static_cast<std::uint8_t>(buttons & 0x3)});
            result.stopTick = std::max(result.stopTick, tick);
        }
    }

    std::stable_sort(result.inputs.begin(), result.inputs.end(),
        [](const InputEvent& a, const InputEvent& b) { return a.tick < b.tick; });

    result.hasPlan = outcome.verdict == dpcore::OutcomeSolved
                  || outcome.verdict == dpcore::OutcomePartial;
    if (!result.hasPlan || (rc != 0 && rc != 1)) {
        result.hasPlan = false;
        if (!outcome.unsupported.empty()) {
            result.message = "gdsolver refused the level: " + outcome.unsupported;
        } else {
            result.message = "gdsolver found no route (solver code " + std::to_string(rc) + ").";
        }
        result.inputs.clear();
        return result;
    }

    if (result.complete) {
        result.message = "gdsolver route ready: " + std::to_string(result.inputs.size())
                       + " input changes; 240 Hz plan.";
    } else {
        result.message = "Partial route only (tick " + std::to_string(outcome.deepT)
                       + "); model did not reach the end.";
    }
    return result;
}

} // namespace uranium::gdsolver
