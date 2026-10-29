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
static float replayCooldown = 0.f;
static int targetTPS = 240;
static bool tpsBypassEnabled = false;
static constexpr float CELL_SIZE = 12.f;
static constexpr float PLAYER_MARGIN = 12.f;
static float previousPlayerX = 0.f;
static bool hasPreviousPlayerX = false;

// Predict a normal-cube jump using the live player's jump-start velocity and
// gravity. The simulation uses quarter-frame steps like GD's fixed-step loop.
// Returns the number of frames to wait before jumping, or -1 if no safe arc
// was found for this hazard.
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

    // Try increasingly later takeoff points. A candidate is safe only if the
    // predicted player box avoids the hazard for the whole horizontal crossing.
    constexpr float substep = 0.25f; // quarter of a 60 Hz physics frame
    constexpr int maxDelayFrames = 48;
    constexpr int maxFlightFrames = 100;
    for (int delay = 0; delay <= maxDelayFrames; ++delay) {
        float y = 0.f;
        float vy = 0.f;
        bool jumping = false;
        bool safe = true;
        bool crossed = false;
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
                    // Continue checking horizontal travel after landing.
                }
            }
            const float x = px + speedPerFrame * frame;
            const float left = x - playerHalfWidth;
            const float right = x + playerHalfWidth;
            const float bottom = py + y - playerHalfHeight;
            const float top = py + y + playerHalfHeight;
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

// Build a coarse collision occupancy map from the actual objects loaded by GD.
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
    // Avoid unbounded maps for exceptionally long levels.
    maxX = std::min(maxX, minX + 12000.f);
    return boxes;
}

// A* searches free space through the hitbox map. This is a geometric route
// planner; it is not yet a full GD physics simulator (jump arcs, gravity,
// speed portals and mode changes still need to be modelled).
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
    // Target is the furthest mapped point, at the player's current height.
    Cell goal{width - 1, start.y};
    // Find a free starting cell nearby if the player's current cell overlaps a hitbox.
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
    // Octile distance matches the 8-way movement cost, so A* doesn't
    // overestimate diagonal routes and expands fewer irrelevant cells.
    auto heuristic = [&](int x, int y) {
        const int ax = std::abs(goal.x - x);
        const int ay = std::abs(goal.y - y);
        const int diagonal = std::min(ax, ay);
        const int straight = std::max(ax, ay) - diagonal;
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
    // Eight directions make the geometric route less blocky.
    constexpr int dx[] = {1,-1,0,0,1,1,-1,-1};
    constexpr int dy[] = {0,0,1,-1,1,-1,1,-1};
    int found = -1;
    size_t expansions = 0;
    while (!open.empty() && expansions++ < 350000) {
        int id = open.top().id; open.pop();
        auto const n = nodes[static_cast<size_t>(id)];
        // A cell can be queued more than once; discard obsolete queue entries.
        int currentBest = best[static_cast<size_t>(key(n.x, n.y))];
        if (currentBest != id) continue;
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
        awaitingChoice = false;
        this->onClose(nullptr);
    }
    void onSolve(CCObject*) {
        if (!m_layer || !m_layer->m_player1 || !m_layer->m_objects) {
            setStatus("No active level objects");
            return;
        }
        auto* player = m_layer->m_player1;
        if (!player->isInNormalMode()) {
            solved = false;
            solution.clear();
            plannedJumpFrames.clear();
            setStatus("DP pathfinder currently supports cube mode only");
            return;
        }

        // Reachability DP adapted from the approach described in gdsolver
        // (MIT; see THIRD_PARTY_NOTICES.md): keep reachable (height, vertical
        // velocity, grounded) states per tick, try both jump/no-jump inputs,
        // discard collisions, and backtrack a surviving input plan.
        struct DPNode {
            int yq;
            int vyq;
            bool grounded;
            int parent;
            bool jump;
        };
        constexpr float yQuantum = 2.f;
        constexpr float vyQuantum = 0.5f;
        constexpr int maxTicks = 12000;
        constexpr size_t maxStates = 450000;
        const float startX = player->getPositionX();
        const float baseY = player->getPositionY();
        const float gravity = std::max(0.05f, static_cast<float>(player->m_gravity) *
            static_cast<float>(player->m_gravityMod));
        const float jumpVelocity = static_cast<float>(player->m_yStart);
        if (!std::isfinite(gravity) || !std::isfinite(jumpVelocity) ||
            gravity <= 0.f || jumpVelocity <= 0.f) {
            setStatus("DP failed: invalid cube physics values");
            return;
        }

        std::vector<Hitbox> hazards;
        float farthestX = startX;
        for (unsigned int i = 0; i < m_layer->m_objects->count(); ++i) {
            auto* object = static_cast<GameObject*>(m_layer->m_objects->objectAtIndex(i));
            if (!object || object->m_objectType != GameObjectType::Hazard) continue;
            const auto r = object->getObjectRect();
            if (r.size.width <= 0.f || r.size.height <= 0.f || r.getMaxX() < startX) continue;
            hazards.push_back({r.getMinX(), r.getMinY(), r.getMaxX(), r.getMaxY()});
            farthestX = std::max(farthestX, r.getMaxX());
        }
        if (hazards.empty()) {
            setStatus("DP found no upcoming hazards");
            return;
        }

        // Estimate world speed from the most recent live samples; use a conservative
        // normal-speed fallback if Solve was opened before samples were collected.
        float speedPerFrame = 5.f;
        if (hasPreviousPlayerX) {
            const float dx = startX - previousPlayerX;
            if (std::isfinite(dx) && dx > 0.1f && dx < 40.f) speedPerFrame = dx;
        }
        const int horizon = std::clamp(static_cast<int>(std::ceil((farthestX - startX + 80.f) /
            speedPerFrame)), 1, maxTicks);

        auto collides = [&](float x, float y) {
            constexpr float halfW = 10.f;
            constexpr float halfH = 10.f;
            const float left = x - halfW, right = x + halfW;
            const float bottom = baseY + y - halfH, top = baseY + y + halfH;
            for (const auto& h : hazards) {
                if (h.right < left) continue;
                if (h.left > right) break;
                if (right > h.left && left < h.right && top > h.bottom && bottom < h.top)
                    return true;
            }
            return false;
        };
        auto keyOf = [](int yq, int vyq, bool grounded) -> uint64_t {
            const uint64_t a = static_cast<uint32_t>(yq);
            const uint64_t b = static_cast<uint32_t>(vyq);
            return (a << 33) ^ (b << 1) ^ static_cast<uint64_t>(grounded);
        };

        std::vector<DPNode> nodes;
        nodes.reserve(100000);
        nodes.push_back({0, 0, true, -1, false});
        std::vector<int> frontier{0};
        int goal = -1;
        int reachedTick = 0;

        for (int tick = 1; tick <= horizon && !frontier.empty(); ++tick) {
            const float x = startX + speedPerFrame * static_cast<float>(tick);
            std::vector<int> next;
            next.reserve(frontier.size() * 2);
            std::unordered_map<uint64_t, int> seen;
            seen.reserve(frontier.size() * 2);
            for (int parent : frontier) {
                const DPNode state = nodes[static_cast<size_t>(parent)];
                for (int action = 0; action <= 1; ++action) {
                    if (action && !state.grounded) continue;
                    float y = static_cast<float>(state.yq) * yQuantum;
                    float vy = static_cast<float>(state.vyq) * vyQuantum;
                    bool grounded = state.grounded;
                    if (action && grounded) {
                        vy = jumpVelocity;
                        grounded = false;
                    }
                    if (!grounded) {
                        vy -= gravity;
                        y += vy;
                        if (y <= 0.f && vy < 0.f) {
                            y = 0.f;
                            vy = 0.f;
                            grounded = true;
                        }
                    }
                    const int yq = static_cast<int>(std::lround(y / yQuantum));
                    const int vyq = static_cast<int>(std::lround(vy / vyQuantum));
                    y = static_cast<float>(yq) * yQuantum;
                    if (collides(x, y)) continue;
                    const uint64_t key = keyOf(yq, vyq, grounded);
                    if (seen.find(key) != seen.end()) continue;
                    if (nodes.size() >= maxStates) {
                        setStatus("DP state cap reached; level needs a larger search");
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
            if (x >= farthestX + 20.f && !frontier.empty()) {
                goal = frontier.front();
                break;
            }
        }
        if (goal < 0) {
            solved = false;
            plannedJumpFrames.clear();
            solution.clear();
            char message[128];
            snprintf(message, sizeof(message), "DP found no route (%d ticks, %zu states)",
                reachedTick, nodes.size());
            setStatus(message);
            return;
        }

        plannedJumpFrames.clear();
        for (int at = goal; at > 0; at = nodes[static_cast<size_t>(at)].parent) {
            const auto& n = nodes[static_cast<size_t>(at)];
            if (n.jump) plannedJumpFrames.push_back(
                static_cast<int>(std::lround((startX + speedPerFrame * static_cast<float>(reachedTick)) / speedPerFrame)));
        }
        // Backtrack with an explicit tick counter; node parents are always from the previous layer.
        plannedJumpFrames.clear();
        {
            int at = goal;
            int tick = reachedTick;
            while (at > 0) {
                const auto& n = nodes[static_cast<size_t>(at)];
                if (n.jump) plannedJumpFrames.push_back(tick);
                at = n.parent;
                --tick;
            }
        }
        std::reverse(plannedJumpFrames.begin(), plannedJumpFrames.end());
        solved = !plannedJumpFrames.empty();
        solvedLevel = levelName(m_layer);
        solution.assign(1, Cell{0, 0});
        if (m_replay) m_replay->setEnabled(solved);
        char message[128];
        snprintf(message, sizeof(message), "DP route found: %zu jumps, %zu states",
            plannedJumpFrames.size(), nodes.size());
        setStatus(message);
    }
    void onReplay(CCObject*) {
        if (!solved || solution.empty() || solvedLevel != levelName(m_layer)) {
            setStatus("Replay locked: solve this level first");
            return;
        }
        if (!m_layer || !m_layer->m_player1 || !m_layer->m_player1->isInNormalMode()) {
            setStatus("Replay supports normal cube mode only");
            return;
        }
        replaying = true;
        replayClock = 0.f;
        replayNode = 0;
        replayFrame = 0;
        replayFrameAccumulator = 0.f;
        lastReplayHazard = nullptr;
        releaseReplayJump = false;
        replayCooldown = 0.f;
        autoClickClock = 0.f;
        setStatus("Replay started: DP jump plan");
        this->onClose(nullptr);
        // This popup is opened from the pause menu. Closing the popup alone
        // leaves the game paused, so resume the underlying PauseLayer too.
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
        if (!Popup::init(360.f, 260.f)) return false;
        m_layer = layer;
        m_pauseLayer = pauseLayer;
        setTitle("Uranium Pathfinder");
        auto menu = CCMenu::create();
        menu->setPosition(CCPointZero);
        addButton(menu, "Replay", {65.f, 112.f}, menu_selector(LevelChoicePopup::onReplay),
                  solved && solvedLevel == levelName(layer));
        addButton(menu, "Solve", {180.f, 112.f}, menu_selector(LevelChoicePopup::onSolve));
        addButton(menu, "Normal", {295.f, 112.f}, menu_selector(LevelChoicePopup::onNormal));
        addButton(menu, "17 CPS Cap", {75.f, 62.f}, menu_selector(LevelChoicePopup::onAutoClick));
        addButton(menu, "TPS -", {180.f, 62.f}, menu_selector(LevelChoicePopup::onTPSDown));
        addButton(menu, "TPS +", {285.f, 62.f}, menu_selector(LevelChoicePopup::onTPSUp));
        addChild(menu);
        m_status = CCLabelBMFont::create("Choose how to enter this level", "chatFont.fnt");
        m_status->setScale(.42f);
        m_status->setPosition({180.f, 35.f});
        addChild(m_status);
        char tpsText[80]; snprintf(tpsText, sizeof(tpsText), "TPS target: %d (bypass not active)", targetTPS);
        m_tpsStatus = CCLabelBMFont::create(tpsText, "chatFont.fnt");
        m_tpsStatus->setScale(.40f);
        m_tpsStatus->setPosition({180.f, 14.f});
        addChild(m_tpsStatus);
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
        PlayLayer::update(dt);
        if (!m_player1 || m_isPaused || m_hasCompletedLevel) return;

        if (uranium::replaying) {
            auto* player = m_player1;
            // Execute the DP's per-frame input plan. Each planned jump is a one-frame
            // press; release on the following update to avoid holding jump.
            if (uranium::releaseReplayJump) {
                player->releaseButton(PlayerButton::Jump);
                uranium::releaseReplayJump = false;
            }
            uranium::replayFrameAccumulator += std::max(0.f, dt) * 60.f;
            int steps = static_cast<int>(uranium::replayFrameAccumulator);
            uranium::replayFrameAccumulator -= static_cast<float>(steps);
            for (int i = 0; i < steps; ++i, ++uranium::replayFrame) {
                if (uranium::replayNode < uranium::plannedJumpFrames.size() &&
                    uranium::plannedJumpFrames[uranium::replayNode] == uranium::replayFrame) {
                    player->pushButton(PlayerButton::Jump);
                    uranium::releaseReplayJump = true;
                    ++uranium::replayNode;
                }
            }
            if (m_hasCompletedLevel) {
                uranium::replaying = false;
                player->releaseButton(PlayerButton::Jump);
            }
            return;
        }

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
