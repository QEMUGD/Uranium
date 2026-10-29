#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <algorithm>
#include <cmath>
#include <queue>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace uranium {
struct Cell { int x = 0, y = 0; };
struct SearchNode { int x, y; float g, f; int parent; };
struct Hitbox { float left, bottom, right, top; };

static bool solved = false;
static std::string solvedLevel;
static std::vector<Cell> solution;
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
        // This first executable controller is deliberately limited to normal
        // cube mode: it reacts to actual hazard objects instead of pretending
        // a free-space A* path is a sequence of valid game inputs.
        if (!m_layer->m_player1->isInNormalMode()) {
            solved = false;
            solution.clear();
            setStatus("Auto replay currently supports cube mode only");
            return;
        }
        size_t hazards = 0;
        for (unsigned int i = 0; i < m_layer->m_objects->count(); ++i) {
            auto* object = static_cast<GameObject*>(m_layer->m_objects->objectAtIndex(i));
            if (object && object->m_objectType == GameObjectType::Hazard) ++hazards;
        }
        if (!hazards) {
            solved = false;
            solution.clear();
            setStatus("No hazard objects found in this level");
            return;
        }
        solved = true;
        solvedLevel = levelName(m_layer);
        solution.assign(1, Cell{0, 0}); // marks this level as prepared for reactive replay
        if (m_replay) m_replay->setEnabled(true);
        char message[128];
        snprintf(message, sizeof(message), "Cube solver armed: %zu hazards", hazards);
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
        lastReplayHazard = nullptr;
        releaseReplayJump = false;
        replayCooldown = 0.f;
        autoClickClock = 0.f;
        setStatus("Replay started: reactive cube controller");
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
            uranium::replayCooldown = std::max(0.f, uranium::replayCooldown - dt);
            if (uranium::releaseReplayJump) {
                player->releaseButton(PlayerButton::Jump);
                uranium::releaseReplayJump = false;
            }

            // Reactive cube controller: choose the nearest upcoming hazard and
            // jump shortly before its hitbox reaches the player's horizontal
            // position. The game itself supplies collision/physics each frame.
            if (player->isInNormalMode() && player->m_isOnGround &&
                uranium::replayCooldown <= 0.f && m_objects) {
                GameObject* nearest = nullptr;
                float nearestDistance = 1.0e9f;
                const float playerX = player->getPositionX();
                // Pick the nearest spike-like hazard. Use the player's real
                // horizontal speed to estimate time-to-contact, not a fixed
                // 18-48 unit window that can be skipped at higher game speed.
                for (unsigned int i = 0; i < m_objects->count(); ++i) {
                    auto* object = static_cast<GameObject*>(m_objects->objectAtIndex(i));
                    if (!object || object->m_objectType != GameObjectType::Hazard) continue;
                    auto rect = object->getObjectRect();
                    const float distance = rect.getMinX() - playerX;
                    if (distance > 0.f && distance < nearestDistance) {
                        nearestDistance = distance;
                        nearest = object;
                    }
                }
                // Conservative trigger distance for a normal-speed cube.
                // Avoid relying on a velocity field that differs across SDK builds.
                // This remains an approximation; speed portals need a proper physics model.
                constexpr float triggerDistance = 60.f;
                if (nearest && nearest != uranium::lastReplayHazard &&
                    nearestDistance <= triggerDistance) {
                    player->pushButton(PlayerButton::Jump);
                    uranium::releaseReplayJump = true;
                    uranium::lastReplayHazard = nearest;
                    // Prevent retriggering while airborne, but permit the next
                    // spike shortly after landing.
                    uranium::replayCooldown = .04f;
                }
            }
            // Stop cleanly after completion; if the player dies, leave the
            // controller armed so it can continue after the game's respawn.
            if (m_hasCompletedLevel) uranium::replaying = false;
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
