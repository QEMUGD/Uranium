#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <algorithm>
#include <cmath>
#include <queue>
#include <string>
#include <unordered_map>
#include <cstdint>
#include <vector>

using namespace geode::prelude;

namespace uranium {
struct Cell { int x = 0, y = 0; };
struct SearchNode { int x, y; float g, f; int parent; };
struct Hitbox { float left, bottom, right, top; };

static bool solved = false;
static std::string solvedLevel;
static std::vector<Cell> solution;
static std::vector<int> plannedJumpFrames;
static int replayFrame = 0;
static float replayFrameAccumulator = 0.f;
static bool replaying = false;
static bool awaitingChoice = false;
static size_t replayNode = 0;
static float replayClock = 0.f;
static bool autoClick17 = false;
static float autoClickClock = 0.f;
static GameObject* lastReplayHazard = nullptr;
static bool releaseReplayJump = false;
// Presses whose planned tick passed during a slow rendered frame are queued, not merged
// into one click. GD only observes a button edge once per PlayLayer update.
static int replayPendingPresses = 0;
static float replayCooldown = 0.f;
static int targetTPS = 240;
static bool tpsBypassEnabled = false;
static constexpr float CELL_SIZE = 12.f;
static constexpr float PLAYER_MARGIN = 12.f;
static float previousPlayerX = 0.f;
static bool hasPreviousPlayerX = false;
static float estimatedSpeedPerFrame = 5.f;
static int speedSamples = 0;

static int predictJumpDelay(PlayerObject* player, GameObject* hazard, float speedPerFrame) {
    if (!player || !hazard || speedPerFrame <= 0.01f) return -1;
    const auto obstacle = hazard->getObjectRect();
    const float px = player->getPositionX();
    const float py = player->getPositionY();
    const float playerHalfWidth = 10.f;
    const float playerHalfHeight = 10.f;
    const float gravity = std::max(0.05f, static_cast<float>(player->m_gravity) *
        static_cast<float>(player->m_gravityMod));
    const float jumpVelocity = static_cast<float>(player->m_yStart);
    if (!std::isfinite(gravity) || !std::isfinite(jumpVelocity) ||
        gravity <= 0.f || jumpVelocity <= 0.f) return -1;
    constexpr float substep = 0.25f;
    constexpr int maxDelayFrames = 48;
    constexpr int maxFlightFrames = 100;
    for (int delay = 0; delay <= maxDelayFrames; ++delay) {
        float y = 0.f, vy = 0.f;
        bool jumping = false, safe = true, crossed = false;
        for (int step = 0; step < maxFlightFrames * 4; ++step) {
            const float frame = static_cast<float>(step) * substep;
            if (!jumping && frame >= static_cast<float>(delay)) {
                jumping = true;
                vy = jumpVelocity;
            }
            if (jumping) {
                vy -= gravity * substep;
                y += vy * substep;
                if (y <= 0.f && frame > static_cast<float>(delay) + 1.f) {
                    y = 0.f;
                    vy = 0.f;
                }
            }
            const float x = px + speedPerFrame * frame;
            const float left = x - playerHalfWidth, right = x + playerHalfWidth;
            const float bottom = py + y - playerHalfHeight, top = py + y + playerHalfHeight;
            if (right >= obstacle.getMinX() && left <= obstacle.getMaxX()) {
                if (top > obstacle.getMinY() && bottom < obstacle.getMaxY()) {
                    safe = false;
                    break;
                }
                if (left > obstacle.getMaxX()) { crossed = true; break; }
            }
            if (left > obstacle.getMaxX()) { crossed = true; break; }
        }
        if (safe && crossed) return delay;
    }
    return -1;
}

static std::string levelName(PlayLayer* layer) {
    if (!layer || !layer->m_level) return "unknown";
    return std::string(layer->m_level->m_levelName.c_str());
}

static std::vector<Hitbox> collectHitboxes(PlayLayer* layer, float& minX, float& maxX, float& minY, float& maxY) {
    std::vector<Hitbox> boxes;
    minX = layer->m_player1 ? layer->m_player1->getPositionX() : 0.f;
    maxX = minX + 500.f;
    minY = 0.f;
    maxY = 360.f;
    if (!layer->m_objects) return boxes;
    for (unsigned int i = 0; i < layer->m_objects->count(); ++i) {
        auto* object = static_cast<GameObject*>(layer->m_objects->objectAtIndex(i));
        if (!object) continue;
        auto r = object->getObjectRect();
        if (r.size.width <= 0.f || r.size.height <= 0.f) continue;
        boxes.push_back({r.getMinX(), r.getMinY(), r.getMaxX(), r.getMaxY()});
        maxX = std::max(maxX, r.getMaxX());
        minY = std::min(minY, r.getMinY() - 48.f);
        maxY = std::max(maxY, r.getMaxY() + 48.f);
    }
    maxX = std::min(maxX, minX + 12000.f);
    return boxes;
}

static std::vector<Cell> findRoute(PlayLayer* layer, size_t& boxCount) {
    float minX, maxX, minY, maxY;
    auto boxes = collectHitboxes(layer, minX, maxX, minY, maxY);
    boxCount = boxes.size();
    if (maxX <= minX) return {};
    const int width = std::clamp(static_cast<int>((maxX - minX) / CELL_SIZE) + 1, 2, 1000);
    const int height = std::clamp(static_cast<int>((maxY - minY) / CELL_SIZE) + 1, 2, 100);
    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<unsigned char> blocked(count, 0);
    auto key = [width](int x, int y) { return y * width + x; };
    auto valid = [&](int x, int y) { return x >= 0 && y >= 0 && x < width && y < height; };
    auto toCellX = [&](float x) { return std::clamp(static_cast<int>((x - minX) / CELL_SIZE), 0, width - 1); };
    auto toCellY = [&](float y) { return std::clamp(static_cast<int>((y - minY) / CELL_SIZE), 0, height - 1); };
    for (auto const& b : boxes) {
        int x0 = toCellX(b.left - PLAYER_MARGIN), x1 = toCellX(b.right + PLAYER_MARGIN);
        int y0 = toCellY(b.bottom - PLAYER_MARGIN), y1 = toCellY(b.top + PLAYER_MARGIN);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) blocked[static_cast<size_t>(key(x,y))] = 1;
    }
    auto* player = layer->m_player1;
    if (!player) return {};
    Cell start{toCellX(player->getPositionX()), toCellY(player->getPositionY())};
    Cell goal{width - 1, start.y};
    auto nearestFree = [&](Cell c) {
        for (int radius = 0; radius < 8; ++radius)
            for (int dy = -radius; dy <= radius; ++dy)
                for (int dx = -radius; dx <= radius; ++dx) {
                    int x = c.x + dx, y = c.y + dy;
                    if (valid(x,y) && !blocked[static_cast<size_t>(key(x,y))]) return Cell{x,y};
                }
        return c;
    };
    start = nearestFree(start);
    for (int y = 0; y < height; ++y) {
        if (!blocked[static_cast<size_t>(key(goal.x,y))]) { goal.y = y; break; }
    }
    if (!valid(start.x,start.y) || !valid(goal.x,goal.y) ||
        blocked[static_cast<size_t>(key(start.x,start.y))] ||
        blocked[static_cast<size_t>(key(goal.x,goal.y))]) return {};
    auto heuristic = [&](int x, int y) {
        const int ax = std::abs(goal.x - x), ay = std::abs(goal.y - y);
        const int diagonal = std::min(ax, ay), straight = std::max(ax, ay) - diagonal;
        return static_cast<float>(diagonal) * 1.41421356f + static_cast<float>(straight);
    };
    struct Entry { float f; int id; };
    auto cmp = [](Entry const& a, Entry const& b) { return a.f > b.f; };
    std::priority_queue<Entry, std::vector<Entry>, decltype(cmp)> open(cmp);
    std::vector<SearchNode> nodes;
    nodes.reserve(std::min<size_t>(count, 65536));
    std::vector<int> best(count, -1);
    nodes.push_back({start.x,start.y,0.f,heuristic(start.x,start.y),-1});
    best[static_cast<size_t>(key(start.x,start.y))] = 0;
    open.push({nodes[0].f,0});
    constexpr int dx[] = {1,-1,0,0,1,1,-1,-1};
    constexpr int dy[] = {0,0,1,-1,1,-1,1,-1};
    int found = -1;
    size_t expansions = 0;
    while (!open.empty() && expansions++ < 350000) {
        int id = open.top().id; open.pop();
        auto const n = nodes[static_cast<size_t>(id)];
        if (best[static_cast<size_t>(key(n.x, n.y))] != id) continue;
        if (n.x == goal.x && n.y == goal.y) { found = id; break; }
        for (int i = 0; i < 8; ++i) {
            int x = n.x + dx[i], y = n.y + dy[i];
            if (!valid(x,y) || blocked[static_cast<size_t>(key(x,y))]) continue;
            if (dx[i] && dy[i] &&
                (blocked[static_cast<size_t>(key(n.x+dx[i],n.y))] ||
                 blocked[static_cast<size_t>(key(n.x,n.y+dy[i]))])) continue;
            float g = n.g + ((dx[i] && dy[i]) ? 1.41421356f : 1.f);
            int k = key(x,y), old = best[static_cast<size_t>(k)];
            if (old >= 0 && nodes[static_cast<size_t>(old)].g <= g) continue;
            int next = static_cast<int>(nodes.size());
            nodes.push_back({x,y,g,g+heuristic(x,y),id});
            best[static_cast<size_t>(k)] = next;
            open.push({nodes[static_cast<size_t>(next)].f,next});
        }
    }
    std::vector<Cell> path;
    for (int at = found; at >= 0; at = nodes[static_cast<size_t>(at)].parent)
        path.push_back({nodes[static_cast<size_t>(at)].x,nodes[static_cast<size_t>(at)].y});
    std::reverse(path.begin(),path.end());
    return path;
}

class LevelChoicePopup final : public Popup {
    PlayLayer* m_layer = nullptr;
    PauseLayer* m_pauseLayer = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCMenuItemSpriteExtra* m_replay = nullptr;
    CCLabelBMFont* m_tpsStatus = nullptr;
    void setStatus(char const* text) { if (m_status) m_status->setString(text); }
    void onAutoClick(CCObject*) {
        autoClick17 = !autoClick17;
        setStatus(autoClick17 ? "17 CPS cap: ON (max 17 clicks/s)" : "17 CPS cap: OFF (uncapped)");
    }
    void onTPSDown(CCObject*) {
        targetTPS = std::max(5, targetTPS - 5);
        if (m_tpsStatus) { char text[80]; snprintf(text, sizeof(text), "TPS target: %d (bypass not active)", targetTPS); m_tpsStatus->setString(text); }
    }
    void onTPSUp(CCObject*) {
        targetTPS = std::min(5000, targetTPS + 5);
        if (m_tpsStatus) { char text[80]; snprintf(text, sizeof(text), "TPS target: %d (bypass not active)", targetTPS); m_tpsStatus->setString(text); }
    }
    void onNormal(CCObject*) {
        replaying = false;
        lastReplayHazard = nullptr;
        releaseReplayJump = false;
        replayPendingPresses = 0;
        awaitingChoice = false;
        this->onClose(nullptr);
    }
    void onSolve(CCObject*) {
        solved = false;
        solution.clear();
        plannedJumpFrames.clear();
        if (m_replay) m_replay->setEnabled(false);
        if (!m_layer || !m_layer->m_player1 || !m_layer->m_objects) {
            setStatus("No active level objects");
            return;
        }
        auto* player = m_layer->m_player1;
        if (!player->isInNormalMode()) {
            setStatus("This solver build is cube-only; other modes need gdsolver state export");
            return;
        }

        struct DPNode { int yq; int vyq; bool grounded; int parent; bool jump; };
        constexpr float yQuantum = 1.f, vyQuantum = 0.25f;
        constexpr int maxTicks = 12000;
        // Finer states improve timing; cap memory use on mobile.
        constexpr size_t maxStates = 1200000;
        constexpr float playerHalfWidth = 10.f, playerHalfHeight = 10.f;
        const float startX = player->getPositionX();
        const float baseY = player->getPositionY();
        const float gravity = std::max(0.05f, static_cast<float>(player->m_gravity) *
            static_cast<float>(player->m_gravityMod));
        const float jumpVelocity = static_cast<float>(player->m_yStart);
        if (!std::isfinite(gravity) || !std::isfinite(jumpVelocity) ||
            gravity <= 0.f || jumpVelocity <= 0.f) {
            setStatus("Solve failed: invalid cube physics");
            return;
        }

        std::vector<Hitbox> hazards;
        float farthestX = startX;
        for (unsigned int i = 0; i < m_layer->m_objects->count(); ++i) {
            auto* object = static_cast<GameObject*>(m_layer->m_objects->objectAtIndex(i));
            if (!object || object->m_objectType != GameObjectType::Hazard) continue;
            const auto r = object->getObjectRect();
            if (r.size.width <= 0.f || r.size.height <= 0.f) continue;
            // Ignore hazards whose whole collision box is already behind the player's
            // front edge. Including them makes every first-step state look dead when
            // Solve is pressed after the player has reached/passed the opening spike.
            if (r.getMinX() <= startX + playerHalfWidth + 2.f) continue;
            hazards.push_back({r.getMinX(), r.getMinY(), r.getMaxX(), r.getMaxY()});
            farthestX = std::max(farthestX, r.getMaxX());
        }
        if (hazards.empty()) {
            setStatus("No upcoming hazards in this section");
            return;
        }
        std::sort(hazards.begin(), hazards.end(), [](const Hitbox& a, const Hitbox& b) {
            return a.left < b.left;
        });

        // Use measured player motion where available, with a normal-speed fallback.
        const float speedPerFrame = std::clamp(estimatedSpeedPerFrame, 2.f, 12.f);
        const int horizon = std::clamp(static_cast<int>(std::ceil(
            (farthestX - startX + 80.f) / speedPerFrame)), 1, maxTicks);
        auto collides = [&](float x, float y) {
            const float left = x - playerHalfWidth, right = x + playerHalfWidth;
            const float bottom = baseY + y - playerHalfHeight, top = baseY + y + playerHalfHeight;
            for (const auto& h : hazards) {
                if (h.right <= left) continue;
                if (h.left >= right) break;
                if (right > h.left && left < h.right && top > h.bottom && bottom < h.top)
                    return true;
            }
            return false;
        };
        auto keyOf = [](int yq, int vyq, bool grounded) -> uint64_t {
            return (static_cast<uint64_t>(static_cast<uint32_t>(yq)) << 33) ^
                (static_cast<uint64_t>(static_cast<uint32_t>(vyq)) << 1) ^
                static_cast<uint64_t>(grounded);
        };

        std::vector<DPNode> nodes;
        nodes.reserve(100000);
        nodes.push_back({0, 0, true, -1, false});
        std::vector<int> frontier{0};
        int goal = -1, reachedTick = 0;
        for (int tick = 1; tick <= horizon && !frontier.empty(); ++tick) {
            const float x = startX + speedPerFrame * static_cast<float>(tick);
            std::vector<int> next;
            std::unordered_map<uint64_t, int> seen;
            seen.reserve(frontier.size() * 2 + 1);
            for (int parent : frontier) {
                const DPNode state = nodes[static_cast<size_t>(parent)];
                for (int action = 0; action <= 1; ++action) {
                    if (action && !state.grounded) continue;
                    float y = static_cast<float>(state.yq) * yQuantum;
                    float vy = static_cast<float>(state.vyq) * vyQuantum;
                    bool grounded = state.grounded;
                    if (action && grounded) { vy = jumpVelocity; grounded = false; }
                    if (!grounded) {
                        vy -= gravity;
                        y += vy;
                        if (y <= 0.f && vy < 0.f) { y = 0.f; vy = 0.f; grounded = true; }
                    }
                    const int yq = static_cast<int>(std::lround(y / yQuantum));
                    const int vyq = static_cast<int>(std::lround(vy / vyQuantum));
                    y = static_cast<float>(yq) * yQuantum;
                    if (collides(x, y)) continue;
                    const uint64_t key = keyOf(yq, vyq, grounded);
                    if (seen.find(key) != seen.end()) continue;
                    if (nodes.size() >= maxStates) {
                        setStatus("Search state limit reached; try a shorter section");
                        return;
                    }
                    const int id = static_cast<int>(nodes.size());
                    nodes.push_back({yq, vyq, grounded, parent, action != 0});
                    seen.emplace(key, id);
                    next.push_back(id);
                }
            }
            frontier = std::move(next);
            reachedTick = tick;
            if (x >= farthestX + playerHalfWidth + 2.f && !frontier.empty()) {
                goal = frontier.front();
                break;
            }
        }
        if (goal < 0) {
            char message[128];
            snprintf(message, sizeof(message), "No route after %d ticks (%zu states)", reachedTick, nodes.size());
            setStatus(message);
            return;
        }
        plannedJumpFrames.clear();
        int at = goal, tick = reachedTick;
        while (at > 0) {
            const auto& n = nodes[static_cast<size_t>(at)];
            if (n.jump) plannedJumpFrames.push_back(tick);
            at = n.parent;
            --tick;
        }
        std::reverse(plannedJumpFrames.begin(), plannedJumpFrames.end());
        solved = true;
        solvedLevel = levelName(m_layer);
        solution.assign(1, Cell{0, 0});
        if (m_replay) m_replay->setEnabled(true);
        char message[128];
        snprintf(message, sizeof(message), "Route: %zu jumps, %zu states, %.2f u/frame",
            plannedJumpFrames.size(), nodes.size(), speedPerFrame);
        setStatus(message);
    }
    void onReplay(CCObject*) {
        if (!solved || solution.empty() || solvedLevel != levelName(m_layer)) {
            setStatus("Replay locked: solve this level first");
            return;
        }
        if (!m_layer || !m_layer->m_player1 || !m_layer->m_player1->isInNormalMode()) {
            setStatus("No compatible cube route is available for this mode");
            return;
        }
        replaying = true;
        replayClock = 0.f;
        replayNode = 0;
        replayFrame = 0;
        replayFrameAccumulator = 0.f;
        lastReplayHazard = nullptr;
        releaseReplayJump = false;
        replayPendingPresses = 0;
        replayCooldown = 0.f;
        autoClickClock = 0.f;
        setStatus("Replay started: DP jump plan");
        this->onClose(nullptr);
        if (m_pauseLayer) m_pauseLayer->onResume(nullptr);
    }
    void addButton(CCMenu* menu, char const* label, CCPoint pos, SEL_MenuHandler callback, bool enabled = true) {
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
        addButton(menu, "Replay", {78.f, 126.f}, menu_selector(LevelChoicePopup::onReplay),
                  solved && solvedLevel == levelName(layer));
        addButton(menu, "Solve", {200.f, 126.f}, menu_selector(LevelChoicePopup::onSolve));
        addButton(menu, "Continue", {322.f, 126.f}, menu_selector(LevelChoicePopup::onNormal));
        addButton(menu, "17 CPS", {92.f, 72.f}, menu_selector(LevelChoicePopup::onAutoClick));
        addButton(menu, "TPS -", {200.f, 72.f}, menu_selector(LevelChoicePopup::onTPSDown));
        addButton(menu, "TPS +", {308.f, 72.f}, menu_selector(LevelChoicePopup::onTPSUp));
        addChild(menu);
        m_status = CCLabelBMFont::create("Choose how to enter this level", "chatFont.fnt");
        m_status->setScale(.46f);
        m_status->setPosition({200.f, 38.f});
        addChild(m_status);
        char tpsText[80]; snprintf(tpsText, sizeof(tpsText), "TPS target: %d (bypass not active)", targetTPS);
        m_tpsStatus = CCLabelBMFont::create(tpsText, "chatFont.fnt");
        m_tpsStatus->setScale(.43f);
        m_tpsStatus->setPosition({200.f, 17.f});
        addChild(m_tpsStatus);
        // Scale the complete popup to fit both landscape phones and desktop windows.
        auto win = CCDirector::get()->getWinSize();
        float fit = std::min((win.width - 24.f) / 400.f, (win.height - 24.f) / 300.f);
        this->setScale(std::clamp(fit, 0.68f, 1.f));
        return true;
    }
public:
    static LevelChoicePopup* create(PlayLayer* layer, PauseLayer* pauseLayer) {
        auto p = new LevelChoicePopup();
        if (p && p->init(layer, pauseLayer)) { p->autorelease(); return p; }
        CC_SAFE_DELETE(p);
        return nullptr;
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
        uranium::awaitingChoice = false;
        return true;
    }
    void update(float dt) {
        // Replay input is edge-triggered. Advance the plan clock from elapsed time,
        // but queue every due press and emit at most one press edge per game update.
        // The previous implementation collapsed several due jumps into one press.
        if (m_player1 && !m_isPaused && !m_hasCompletedLevel && uranium::replaying) {
            auto* player = m_player1;
            if (uranium::releaseReplayJump) {
                player->releaseButton(PlayerButton::Jump);
                uranium::releaseReplayJump = false;
            }

            const float safeDt = std::clamp(dt, 0.f, 0.4f);
            uranium::replayFrameAccumulator += safeDt * 60.f;
            uranium::replayFrameAccumulator = std::min(uranium::replayFrameAccumulator, 24.f);
            int ticks = 0;
            while (uranium::replayFrameAccumulator >= 1.f && ticks++ < 24) {
                uranium::replayFrameAccumulator -= 1.f;
                ++uranium::replayFrame;
                while (uranium::replayNode < uranium::plannedJumpFrames.size() &&
                       uranium::plannedJumpFrames[uranium::replayNode] <= uranium::replayFrame) {
                    ++uranium::replayPendingPresses;
                    ++uranium::replayNode;
                }
            }

            if (!uranium::releaseReplayJump && uranium::replayPendingPresses > 0) {
                player->pushButton(PlayerButton::Jump);
                --uranium::replayPendingPresses;
                uranium::releaseReplayJump = true;
            }
        }

        PlayLayer::update(dt);

        if (!m_player1 || m_isPaused || m_hasCompletedLevel) {
            if (m_hasCompletedLevel) {
                uranium::replaying = false;
                uranium::replayPendingPresses = 0;
                uranium::releaseReplayJump = false;
                if (m_player1) m_player1->releaseButton(PlayerButton::Jump);
            }
            return;
        }

        // Calibrate world speed from live player movement instead of assuming a fixed rate.
        const float x = m_player1->getPositionX();
        const float frameScale = std::clamp(std::max(0.f, dt) * 60.f, 0.25f, 4.f);
        if (uranium::hasPreviousPlayerX) {
            const float dx = x - uranium::previousPlayerX;
            const float sample = dx / frameScale;
            if (std::isfinite(sample) && sample > 0.25f && sample < 20.f) {
                const int oldSamples = std::min(uranium::speedSamples, 15);
                uranium::estimatedSpeedPerFrame =
                    (uranium::estimatedSpeedPerFrame * static_cast<float>(oldSamples) + sample) /
                    static_cast<float>(oldSamples + 1);
                ++uranium::speedSamples;
            }
        }
        uranium::previousPlayerX = x;
        uranium::hasPreviousPlayerX = true;

        if (uranium::replaying || !uranium::autoClick17) return;
        uranium::autoClickClock += dt;
        constexpr float interval = 1.f / 17.f;
        if (uranium::autoClickClock >= interval) {
            uranium::autoClickClock = std::fmod(uranium::autoClickClock, interval);
            m_player1->pushButton(PlayerButton::Jump);
            m_player1->releaseButton(PlayerButton::Jump);
        }
    }
};
