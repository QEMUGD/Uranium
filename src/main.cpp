#include <cctype>
#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <queue>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace uranium {
struct MacroEvent { double time = 0.0; int button = 1; bool down = false; };
struct Rect { float left = 0.f, bottom = 0.f, right = 0.f, top = 0.f; };
struct Cell { int x = 0, y = 0; };
struct Node { int x = 0, y = 0; float cost = 0.f, estimate = 0.f; int parent = -1; };

static std::vector<MacroEvent> events;
static std::vector<std::filesystem::path> macroFiles;
static size_t selectedMacro = 0, playbackIndex = 0;
static bool recording = false, playing = false;
static float speedMultiplier = 1.f;
static double recordClock = 0.0, playbackClock = 0.0;
static std::string lastLevel = "level";
static std::vector<Rect> observedHitboxes;

static std::filesystem::path macroDirectory() {
    auto path = Mod::get()->getSaveDir() / "macros";
    std::filesystem::create_directories(path);
    return path;
}
static std::string fileSafe(std::string s) {
    for (char& c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) c = '_';
    return s.empty() ? "level" : s;
}
static void refreshMacros() {
    macroFiles.clear();
    for (auto const& entry : std::filesystem::directory_iterator(macroDirectory()))
        if (entry.is_regular_file() && entry.path().extension() == ".gpt") macroFiles.push_back(entry.path());
    std::sort(macroFiles.begin(), macroFiles.end());
    if (macroFiles.empty()) selectedMacro = 0;
    else selectedMacro = std::min(selectedMacro, macroFiles.size() - 1);
}
static bool saveMacro(std::string name) {
    std::ofstream out(macroDirectory() / (fileSafe(std::move(name)) + ".gpt"));
    if (!out) return false;
    out << "URANIUM_MACRO 1\n";
    out.precision(9);
    for (auto const& e : events) out << e.time << ' ' << e.button << ' ' << (e.down ? 1 : 0) << '\n';
    return static_cast<bool>(out);
}
static bool loadMacro(std::filesystem::path const& path) {
    std::ifstream in(path);
    std::string header;
    if (!std::getline(in, header) || header != "URANIUM_MACRO 1") return false;
    std::vector<MacroEvent> loaded;
    MacroEvent e; int down = 0;
    while (in >> e.time >> e.button >> down) {
        if (e.time < 0.0 || e.button < 0 || e.button > 16 || (down != 0 && down != 1)) return false;
        e.down = down == 1; loaded.push_back(e);
    }
    if (loaded.empty()) return false;
    events = std::move(loaded); playbackIndex = 0; playbackClock = 0.0;
    return true;
}
static void stopAndSaveOnCompletion() {
    playing = false;
    if (recording) {
        recording = false;
        if (!events.empty() && saveMacro(lastLevel))
            Notification::create("Uranium macro saved", NotificationIcon::Success)->show();
    }
}

// A* grid search primitive for a future collision-aware solver. The grid is not
// yet populated from live game hitboxes and is not a substitute for GD physics.
static std::vector<Cell> findGridPath(int width, int height, Cell start, Cell goal, std::vector<bool> const& blocked) {
    if (width <= 0 || height <= 0 || blocked.size() != static_cast<size_t>(width * height)) return {};
    auto valid = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < width && y < height && !blocked[static_cast<size_t>(y * width + x)];
    };
    if (!valid(start.x, start.y) || !valid(goal.x, goal.y)) return {};
    auto h = [&](int x, int y) { return static_cast<float>(std::abs(goal.x-x) + std::abs(goal.y-y)); };
    auto key = [&](int x, int y) { return y * width + x; };
    struct Entry { float score; int id; };
    auto cmp = [](Entry const& a, Entry const& b) { return a.score > b.score; };
    std::priority_queue<Entry, std::vector<Entry>, decltype(cmp)> open(cmp);
    std::vector<Node> nodes;
    std::vector<int> best(static_cast<size_t>(width * height), -1);
    nodes.push_back({start.x, start.y, 0.f, h(start.x,start.y), -1});
    best[key(start.x,start.y)] = 0; open.push({nodes[0].estimate, 0});
    constexpr int dx[] = {1,-1,0,0}, dy[] = {0,0,1,-1};
    int found = -1;
    while (!open.empty()) {
        int id = open.top().id; open.pop();
        Node n = nodes[id];
        if (n.x == goal.x && n.y == goal.y) { found = id; break; }
        for (int i=0;i<4;++i) {
            int x=n.x+dx[i], y=n.y+dy[i];
            if (!valid(x,y)) continue;
            float cost=n.cost+1.f; int k=key(x,y), old=best[k];
            if (old>=0 && nodes[old].cost<=cost) continue;
            int next=static_cast<int>(nodes.size());
            nodes.push_back({x,y,cost,cost+h(x,y),id}); best[k]=next; open.push({nodes[next].estimate,next});
        }
    }
    std::vector<Cell> path;
    for (int at=found;at>=0;at=nodes[at].parent) path.push_back({nodes[at].x,nodes[at].y});
    std::reverse(path.begin(),path.end());
    return path;
}

class ToolsPopup final : public Popup {
    CCLabelBMFont* statusLabel = nullptr;
    CCLabelBMFont* macroLabel = nullptr;
    void status(char const* s) { if (statusLabel) statusLabel->setString(s); }
    void updateMacroLabel() {
        refreshMacros();
        if (!macroLabel) return;
        if (macroFiles.empty()) macroLabel->setString("No saved macros");
        else {
            auto name=macroFiles[selectedMacro].stem().string();
            if (name.size()>24) name=name.substr(0,21)+"...";
            macroLabel->setString(name.c_str());
        }
    }
    void button(CCMenu* menu, char const* text, CCPoint pos, SEL_MenuHandler cb) {
        auto sprite=ButtonSprite::create(text,"bigFont.fnt","GJ_button_01.png",0.7f);
        auto item=CCMenuItemSpriteExtra::create(sprite,this,cb); item->setPosition(pos); menu->addChild(item);
    }
    void onRecord(CCObject*) {
        if (recording) {
            recording=false;
            status(!events.empty() && saveMacro(lastLevel) ? "Recording stopped and saved" : "Stopped; nothing saved");
            updateMacroLabel();
        } else { events.clear(); recordClock=0.0; recording=true; playing=false; status("Recording inputs"); }
    }
    void onLoad(CCObject*) {
        refreshMacros();
        if (macroFiles.empty()) { status("No macro to load"); return; }
        recording=false; playing=false;
        status(loadMacro(macroFiles[selectedMacro]) ? "Macro loaded" : "Macro load failed");
    }
    void onPlay(CCObject*) {
        if (events.empty()) { status("Load or record a macro first"); return; }
        recording=false; playing=true; playbackClock=0.0; playbackIndex=0; status("Replay started");
    }
    void onStop(CCObject*) { playing=false; status("Stopped"); }
    void onPrev(CCObject*) {
        refreshMacros(); if (macroFiles.empty()) { status("No saved macros"); return; }
        selectedMacro=(selectedMacro+macroFiles.size()-1)%macroFiles.size(); updateMacroLabel(); status("Previous macro selected");
    }
    void onNext(CCObject*) {
        refreshMacros(); if (macroFiles.empty()) { status("No saved macros"); return; }
        selectedMacro=(selectedMacro+1)%macroFiles.size(); updateMacroLabel(); status("Next macro selected");
    }
    void onSpeed(CCObject*) {
        speedMultiplier = speedMultiplier < 1.5f ? 2.f : speedMultiplier < 3.5f ? 5.f : 1.f;
        status(speedMultiplier == 1.f ? "Normal speed" : "Experimental speed multiplier");
    }
    void onSolve(CCObject*) {
        status("Auto-solver not implemented yet");
        Notification::create("Physics-accurate solve and retry loop are not implemented yet", NotificationIcon::Warning)->show();
    }
    bool init() {
        if (!Popup::init(350.f,300.f)) return false;
        setTitle("Uranium");
        auto menu=CCMenu::create(); menu->setPosition(CCPointZero);
        button(menu,"Record / Save",{85.f,230.f},menu_selector(ToolsPopup::onRecord));
        button(menu,"Load",{265.f,230.f},menu_selector(ToolsPopup::onLoad));
        button(menu,"Replay",{85.f,188.f},menu_selector(ToolsPopup::onPlay));
        button(menu,"Stop",{265.f,188.f},menu_selector(ToolsPopup::onStop));
        button(menu,"< Macro",{85.f,146.f},menu_selector(ToolsPopup::onPrev));
        button(menu,"Macro >",{265.f,146.f},menu_selector(ToolsPopup::onNext));
        button(menu,"Solve",{85.f,104.f},menu_selector(ToolsPopup::onSolve));
        button(menu,"Speed",{265.f,104.f},menu_selector(ToolsPopup::onSpeed));
        addChild(menu);
        macroLabel=CCLabelBMFont::create("No saved macros","chatFont.fnt"); macroLabel->setScale(.6f); macroLabel->setPosition({175.f,66.f}); addChild(macroLabel);
        statusLabel=CCLabelBMFont::create("Clean-room prototype","chatFont.fnt"); statusLabel->setScale(.6f); statusLabel->setPosition({175.f,38.f}); addChild(statusLabel);
        updateMacroLabel(); return true;
    }
public:
    static ToolsPopup* create() { auto p=new ToolsPopup(); if(p->init()){p->autorelease();return p;} delete p; return nullptr; }
};
} // namespace uranium

class $modify(UraniumPlayer, PlayerObject) {
    bool pushButton(PlayerButton button) {
        bool result=PlayerObject::pushButton(button);
        if (uranium::recording) uranium::events.push_back({uranium::recordClock,static_cast<int>(button),true});
        return result;
    }
    bool releaseButton(PlayerButton button) {
        bool result=PlayerObject::releaseButton(button);
        if (uranium::recording) uranium::events.push_back({uranium::recordClock,static_cast<int>(button),false});
        return result;
    }
};
class $modify(UraniumPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if(!PlayLayer::init(level,useReplay,dontCreateObjects)) return false;
        uranium::recording=false; uranium::playing=false; uranium::recordClock=0.0;
        uranium::lastLevel = level ? std::string(level->m_levelName.c_str()) : "level";
        uranium::observedHitboxes.clear(); return true;
    }
    void update(float dt) {
        float scaled=dt*uranium::speedMultiplier;
        PlayLayer::update(scaled);
        if(uranium::recording) uranium::recordClock+=scaled;
        if(uranium::playing && m_player1) {
            uranium::playbackClock+=scaled;
            while(uranium::playbackIndex<uranium::events.size() &&
                  uranium::events[uranium::playbackIndex].time<=uranium::playbackClock) {
                auto const& e=uranium::events[uranium::playbackIndex++];
                auto btn=static_cast<PlayerButton>(e.button);
                if(e.down) m_player1->pushButton(btn); else m_player1->releaseButton(btn);
            }
            if(uranium::playbackIndex>=uranium::events.size()) uranium::playing=false;
        }
    }
    void levelComplete() { uranium::stopAndSaveOnCompletion(); PlayLayer::levelComplete(); }
    void onQuit() {
        uranium::playing=false;
        if(uranium::recording) { uranium::recording=false; if(!uranium::events.empty()) uranium::saveMacro(uranium::lastLevel); }
        PlayLayer::onQuit();
    }
};
class $modify(UraniumPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto win=CCDirector::get()->getWinSize();
        auto sprite=CCSprite::createWithSpriteFrameName("GJ_hammerIcon_001.png");
        if(!sprite) return;
        auto item=CCMenuItemSpriteExtra::create(sprite,this,menu_selector(UraniumPauseLayer::onOpenUranium));
        item->setID("uranium-open-tools"); item->setScale(.55f); item->setPosition({win.width-28.f,win.height*.5f});
        if(auto menu=getChildByID("right-button-menu")) { menu->addChild(item); menu->updateLayout(); }
        else addChild(item);
    }
    void onOpenUranium(CCObject*) { if(auto popup=uranium::ToolsPopup::create()) popup->show(); }
};
