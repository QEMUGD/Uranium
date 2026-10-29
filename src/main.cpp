#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
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
static size_t replayNode = 0;
static float replayClock = 0.f;
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
    for (auto* object : layer->m_objects) {
        if (!object || !object->m_isCollidable) continue;
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
    auto heuristic = [&](int x, int y) {
        return static_cast<float>(std::abs(goal.x-x) + std::abs(goal.y-y));
    };
    struct Entry { float f; int id; };
    auto cmp = [](Entry const& a, Entry const& b) { return a.f > b.f; };
    std::priority_queue<Entry, std::vector<Entry>, decltype(cmp)> open(cmp);
    std::vector<SearchNode> nodes;
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
        auto n = nodes[static_cast<size_t>(id)];
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
    CCLabelBMFont* m_status = nullptr;
    CCMenuItemSpriteExtra* m_replay = nullptr;
    void setStatus(char const* text) { if (m_status) m_status->setString(text); }
    void onNormal(CCObject*) {
        replaying = false;
        this->onClose(nullptr);
    }
    void onSolve(CCObject*) {
        if (!m_layer) { setStatus("No active level"); return; }
        setStatus("Mapping hitboxes and searching...");
        size_t hitboxCount = 0;
        auto route = findRoute(m_layer, hitboxCount);
        if (route.empty()) {
            solved = false;
            solution.clear();
            setStatus("No geometric route found");
            Notification::create("Pathfinder could not find a route in this collision map", NotificationIcon::Warning)->show();
            return;
        }
        solution = std::move(route);
        solved = true;
        solvedLevel = levelName(m_layer);
        if (m_replay) m_replay->setEnabled(true);
        char message[128];
        snprintf(message, sizeof(message), "Route found: %zu points / %zu hitboxes", solution.size(), hitboxCount);
        setStatus(message);
        Notification::create("Geometric route found (physics validation pending)", NotificationIcon::Success)->show();
    }
    void onReplay(CCObject*) {
        if (!solved || solution.empty() || solvedLevel != levelName(m_layer)) {
            setStatus("Replay locked: solve this level first");
            return;
        }
        // Route-following control is intentionally guarded as experimental:
        // the geometric route is not yet a validated input sequence.
        setStatus("Route ready; physics-based execution is not validated yet");
        Notification::create("Route found, but automated execution needs the physics solver", NotificationIcon::Warning)->show();
    }
    void addButton(CCMenu* menu, char const* label, CCPoint pos, SEL_MenuHandler callback, bool enabled = true) {
        auto sprite = ButtonSprite::create(label, "bigFont.fnt", "GJ_button_01.png", .7f);
        auto item = CCMenuItemSpriteExtra::create(sprite, this, callback);
        item->setPosition(pos);
        item->setEnabled(enabled);
        menu->addChild(item);
        if (std::string(label) == "Replay") m_replay = item;
    }
    bool init(PlayLayer* layer) {
        if (!Popup::init(340.f, 235.f)) return false;
        m_layer = layer;
        setTitle("Uranium Pathfinder");
        auto menu = CCMenu::create();
        menu->setPosition(CCPointZero);
        addButton(menu, "Replay", {75.f, 120.f}, menu_selector(LevelChoicePopup::onReplay),
                  solved && solvedLevel == levelName(layer));
        addButton(menu, "Solve", {170.f, 120.f}, menu_selector(LevelChoicePopup::onSolve));
        addButton(menu, "Normal", {265.f, 120.f}, menu_selector(LevelChoicePopup::onNormal));
        addChild(menu);
        m_status = CCLabelBMFont::create("Choose how to enter this level", "chatFont.fnt");
        m_status->setScale(.62f);
        m_status->setPosition({170.f, 70.f});
        addChild(m_status);
        return true;
    }
public:
    static LevelChoicePopup* create(PlayLayer* layer) {
        auto p = new LevelChoicePopup();
        if (p && p->init(layer)) { p->autorelease(); return p; }
        CC_SAFE_DELETE(p);
        return nullptr;
    }
};
} // namespace uranium

class $modify(UraniumPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        uranium::replaying = false;
        auto current = uranium::levelName(this);
        // Show the choice screen on entry, before the player begins actively playing.
        // The level scene is initialized underneath the modal popup.
        if (current != "unknown") {
            if (auto popup = uranium::LevelChoicePopup::create(this)) popup->show();
        }
        return true;
    }
};
