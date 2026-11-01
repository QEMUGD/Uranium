#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef URANIUM_HAS_GDSOLVER
#define URANIUM_HAS_GDSOLVER 0
#endif

#if URANIUM_HAS_GDSOLVER
#include "gdsolver_bridge.hpp"
#endif

using namespace geode::prelude;

namespace uranium {

#if URANIUM_HAS_GDSOLVER
using gdsolver::InputEvent;
using gdsolver::SolveResult;

static std::vector<InputEvent> replayInputs;
static long long replayStopTick = -1;
static std::size_t replayEventIndex = 0;
static long long replayTick = 0;
static float replayAccumulator = 0.f;
static std::uint8_t replayHeldMask = 0;
static bool solved = false;
static bool replaying = false;
static bool solveRunning = false;
static std::string solvedLevel;

static std::mutex solveMutex;
static std::atomic<bool> solveFinished{false};
static SolveResult completedSolve;

static constexpr float kSolverTickSeconds = 1.f / 240.f;
static constexpr int kMaxCatchupTicksPerRender = 96;

static std::string levelName(PlayLayer* layer) {
    if (!layer || !layer->m_level) return "unknown";
    return std::string(layer->m_level->m_levelName.c_str());
}

static void releaseReplayInputs(PlayLayer* layer) {
    if (layer && layer->m_player1) layer->m_player1->releaseButton(PlayerButton::Jump);
    if (layer && layer->m_player2) layer->m_player2->releaseButton(PlayerButton::Jump);
    replayHeldMask = 0;
}

static bool applyReplayMask(PlayLayer* layer, std::uint8_t nextMask) {
    if (!layer || !layer->m_player1) return false;
    if ((nextMask & 2u) && !layer->m_player2) return false;

    const std::uint8_t changed = static_cast<std::uint8_t>(replayHeldMask ^ nextMask);
    if (changed & 1u) {
        if (nextMask & 1u) layer->m_player1->pushButton(PlayerButton::Jump);
        else layer->m_player1->releaseButton(PlayerButton::Jump);
    }
    if (changed & 2u) {
        if (nextMask & 2u) layer->m_player2->pushButton(PlayerButton::Jump);
        else layer->m_player2->releaseButton(PlayerButton::Jump);
    }
    replayHeldMask = nextMask;
    return true;
}

static bool applyEventsThrough(PlayLayer* layer, long long tick) {
    while (replayEventIndex < replayInputs.size() &&
           replayInputs[replayEventIndex].tick <= tick) {
        if (!applyReplayMask(layer, replayInputs[replayEventIndex].buttons)) return false;
        ++replayEventIndex;
    }
    return true;
}

static void stopReplay(PlayLayer* layer, const char* reason, bool notify) {
    replaying = false;
    replayAccumulator = 0.f;
    releaseReplayInputs(layer);
    if (notify && reason && *reason) {
        Notification::create(reason, NotificationIcon::Info)->show();
    }
}

static void startSolveWorker(gdsolver::SolveJob job) {
    solveFinished.store(false, std::memory_order_release);
    solveRunning = true;
    std::thread([job = std::move(job)]() mutable {
        SolveResult result;
        try {
            result = gdsolver::executeSolve(std::move(job));
        } catch (const std::exception& e) {
            result.message = std::string("Solver exception: ") + e.what();
        } catch (...) {
            result.message = "Solver stopped after an unknown error.";
        }
        {
            std::lock_guard<std::mutex> lock(solveMutex);
            completedSolve = std::move(result);
        }
        solveFinished.store(true, std::memory_order_release);
    }).detach();
}

static bool takeSolveResult(SolveResult& result) {
    if (!solveFinished.exchange(false, std::memory_order_acq_rel)) return false;
    std::lock_guard<std::mutex> lock(solveMutex);
    result = std::move(completedSolve);
    completedSolve = SolveResult{};
    solveRunning = false;
    return true;
}

#endif // URANIUM_HAS_GDSOLVER

static bool autoClick17 = false;
static float autoClickClock = 0.f;

class LevelChoicePopup final : public Popup {
    PlayLayer* m_layer = nullptr;
    PauseLayer* m_pauseLayer = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_modeStatus = nullptr;
    CCMenuItemSpriteExtra* m_replay = nullptr;

    void setStatus(const std::string& text) {
        if (m_status) m_status->setString(text.c_str());
    }

    void onAutoClick(CCObject*) {
        autoClick17 = !autoClick17;
        setStatus(autoClick17 ? "17 CPS cap: ON" : "17 CPS cap: OFF");
    }

    void onContinue(CCObject*) {
#if URANIUM_HAS_GDSOLVER
        if (replaying) stopReplay(m_layer, "Replay stopped.", false);
#endif
        this->onClose(nullptr);
    }

    void onSolve(CCObject*) {
#if URANIUM_HAS_GDSOLVER
        if (!m_layer || !m_layer->m_player1 || !m_layer->m_objects) {
            setStatus("No active level/player to export.");
            return;
        }
        if (gdsolver::isPlatformer(m_layer)) {
            setStatus("Platformer controls are not supported by gdsolver yet.");
            return;
        }
        if (solveRunning) {
            setStatus("Solver is already working...");
            return;
        }

        // Export live GD objects on the game thread. Only the plain CSV/argument
        // snapshot crosses to the worker; no GameObject or layer pointer is used there.
        auto job = gdsolver::prepareSolve(m_layer);
        if (!job.ok) {
            setStatus(job.error);
            return;
        }

        solved = false;
        replayInputs.clear();
        replayStopTick = -1;
        if (m_replay) m_replay->setEnabled(false);
        setStatus("Level exported. Running 240 Hz solver...");
        startSolveWorker(std::move(job));
#else
        setStatus("gdsolver is unavailable on Android 32-bit.");
#endif
    }

    void onReplay(CCObject*) {
#if URANIUM_HAS_GDSOLVER
        if (!solved || solvedLevel != levelName(m_layer)) {
            setStatus("Replay locked: solve this level first.");
            return;
        }
        if (!m_layer || !m_layer->m_player1) {
            setStatus("No active player.");
            return;
        }
        if (gdsolver::isPlatformer(m_layer)) {
            setStatus("Platformer replay is not supported.");
            return;
        }
        if (std::any_of(replayInputs.begin(), replayInputs.end(),
                        [](const InputEvent& e) { return (e.buttons & 2u) != 0; }) &&
            !m_layer->m_player2) {
            setStatus("This route needs player 2, but the level has no second player.");
            return;
        }

        replayEventIndex = 0;
        replayTick = 0;
        replayAccumulator = 0.f;
        replayHeldMask = 0;
        releaseReplayInputs(m_layer);
        replaying = true;
        autoClick17 = false;
        autoClickClock = 0.f;
        setStatus("Starting exact-tick replay for " + gdsolver::modeName(m_layer->m_player1) + ".");
        this->onClose(nullptr);
        if (m_pauseLayer) m_pauseLayer->onResume(nullptr);
#else
        setStatus("Replay requires the gdsolver build (Windows/Android 64-bit).");
#endif
    }

    void addButton(CCMenu* menu, char const* label, CCPoint pos,
                   SEL_MenuHandler callback, bool enabled = true) {
        auto sprite = ButtonSprite::create(label, "bigFont.fnt", "GJ_button_01.png", .48f);
        auto item = CCMenuItemSpriteExtra::create(sprite, this, callback);
        item->setPosition(pos);
        item->setEnabled(enabled);
        menu->addChild(item);
        if (std::string(label) == "Replay") m_replay = item;
    }

    bool init(PlayLayer* layer, PauseLayer* pauseLayer) {
        if (!Popup::init(400.f, 300.f)) return false;
        m_layer = layer;
        m_pauseLayer = pauseLayer;
        setTitle("Uranium Pathfinder");

        auto menu = CCMenu::create();
        menu->setPosition(CCPointZero);
#if URANIUM_HAS_GDSOLVER
        addButton(menu, "Replay", {78.f, 126.f}, menu_selector(LevelChoicePopup::onReplay),
                  solved && solvedLevel == levelName(layer));
#else
        addButton(menu, "Replay", {78.f, 126.f}, menu_selector(LevelChoicePopup::onReplay), false);
#endif
        addButton(menu, "Solve", {200.f, 126.f}, menu_selector(LevelChoicePopup::onSolve));
        addButton(menu, "Continue", {322.f, 126.f}, menu_selector(LevelChoicePopup::onContinue));
        addButton(menu, "17 CPS", {92.f, 72.f}, menu_selector(LevelChoicePopup::onAutoClick));
        addChild(menu);

#if URANIUM_HAS_GDSOLVER
        const std::string mode = layer && layer->m_player1 ? gdsolver::modeName(layer->m_player1) : "Unknown";
        m_modeStatus = CCLabelBMFont::create(("Mode: " + mode + "  |  Solver: gdsolver 240 Hz").c_str(), "chatFont.fnt");
#else
        m_modeStatus = CCLabelBMFont::create("Solver unavailable on Android 32-bit", "chatFont.fnt");
#endif
        m_modeStatus->setScale(.43f);
        m_modeStatus->setPosition({200.f, 44.f});
        addChild(m_modeStatus);

        m_status = CCLabelBMFont::create("Solve the active level to create a route", "chatFont.fnt");
        m_status->setScale(.43f);
        m_status->setPosition({200.f, 20.f});
        addChild(m_status);

        auto win = CCDirector::get()->getWinSize();
        const float fit = std::min((win.width - 24.f) / 400.f, (win.height - 24.f) / 300.f);
        this->setScale(std::clamp(fit, 0.68f, 1.f));
        this->schedule(schedule_selector(LevelChoicePopup::updatePoll), 0.1f);
        return true;
    }

public:
    static LevelChoicePopup* create(PlayLayer* layer, PauseLayer* pauseLayer) {
        auto p = new LevelChoicePopup();
        if (p && p->init(layer, pauseLayer)) {
            p->autorelease();
            return p;
        }
        CC_SAFE_DELETE(p);
        return nullptr;
    }

    void updatePoll(float) { pollSolver(); }

    void pollSolver() {
#if URANIUM_HAS_GDSOLVER
        if (!solveRunning) return;
        SolveResult result;
        if (!takeSolveResult(result)) return;

        if (result.complete && result.hasPlan && result.level == levelName(m_layer)) {
            replayInputs = std::move(result.inputs);
            replayStopTick = result.stopTick;
            solved = true;
            solvedLevel = result.level;
            if (m_replay) m_replay->setEnabled(true);
            setStatus(result.message);
        } else {
            solved = false;
            replayInputs.clear();
            if (m_replay) m_replay->setEnabled(false);
            if (result.complete && result.level != levelName(m_layer)) {
                setStatus("Solver finished for a different level; solve this level again.");
            } else {
                setStatus(result.message.empty() ? "gdsolver did not prove a complete route." : result.message);
            }
        }
#endif
    }
};

} // namespace uranium

class $modify(UraniumPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto menu = CCMenu::create();
        menu->setID("uranium-pause-menu");
        menu->setPosition(CCPointZero);
        auto sprite = ButtonSprite::create("Uranium", "bigFont.fnt", "GJ_button_04.png", .6f);
        auto item = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(UraniumPauseLayer::onUranium));
        item->setPosition({0.f, 0.f});
        menu->addChild(item);
        menu->setPosition({CCDirector::get()->getWinSize().width - 65.f, 45.f});
        this->addChild(menu);
    }

    void onUranium(CCObject*) {
        auto layer = PlayLayer::get();
        if (auto popup = uranium::LevelChoicePopup::create(layer, this)) popup->show();
        else Notification::create("Uranium could not open the pathfinder menu", NotificationIcon::Warning)->show();
    }
};

class $modify(UraniumPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
#if URANIUM_HAS_GDSOLVER
        uranium::replaying = false;
        uranium::solved = false;
        uranium::replayInputs.clear();
        uranium::replayEventIndex = 0;
        uranium::replayTick = 0;
        uranium::replayAccumulator = 0.f;
        uranium::replayHeldMask = 0;
        uranium::replayStopTick = -1;
#endif
        return true;
    }

    void update(float dt) {
#if URANIUM_HAS_GDSOLVER
        if (uranium::replaying && !m_isPaused && !m_hasCompletedLevel && m_player1) {
            const float safeDt = std::clamp(dt, 0.f, 0.5f);
            uranium::replayAccumulator += safeDt;
            int simulated = 0;

            while (uranium::replayAccumulator + 1e-7f >= uranium::kSolverTickSeconds &&
                   simulated < uranium::kMaxCatchupTicksPerRender && uranium::replaying) {
                uranium::replayAccumulator -= uranium::kSolverTickSeconds;
                ++simulated;

                if (!uranium::applyEventsThrough(this, uranium::replayTick)) {
                    uranium::stopReplay(this, "Replay stopped: missing second player for dual input.", true);
                    break;
                }
                ++uranium::replayTick;
                if (!uranium::applyEventsThrough(this, uranium::replayTick)) {
                    uranium::stopReplay(this, "Replay stopped: missing second player for dual input.", true);
                    break;
                }

                // Step the real PlayLayer at the solver's fixed 240 Hz cadence. This
                // preserves individual press/release edges even on 30/60 FPS displays.
                PlayLayer::update(uranium::kSolverTickSeconds);

                if (m_hasCompletedLevel) {
                    uranium::stopReplay(this, "Uranium replay completed the level.", true);
                    break;
                }
                if (!m_player1 || m_player1->m_isDead) {
                    uranium::stopReplay(this, "Replay ended: the game rejected the predicted route.", true);
                    break;
                }
                if (uranium::replayStopTick >= 0 &&
                    uranium::replayTick > uranium::replayStopTick + 240) {
                    uranium::stopReplay(this, "Replay plan ended without a level completion.", true);
                    break;
                }
            }
            return;
        }
#endif

        PlayLayer::update(dt);

        if (!m_player1 || m_isPaused || m_hasCompletedLevel) return;

#if URANIUM_HAS_GDSOLVER
        if (uranium::replaying) return;
#endif
        if (!uranium::autoClick17) return;

        uranium::autoClickClock += dt;
        constexpr float interval = 1.f / 17.f;
        if (uranium::autoClickClock >= interval) {
            uranium::autoClickClock = std::fmod(uranium::autoClickClock, interval);
            m_player1->pushButton(PlayerButton::Jump);
            m_player1->releaseButton(PlayerButton::Jump);
        }
    }
};
