#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/ui/Popup.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {
struct GPTEvent {
    double timeSeconds = 0.0;
    int button = 1;
    bool pressed = true;
    float x = 0.f;
    float y = 0.f;
};
struct TrailPoint { float x = 0.f; float y = 0.f; double t = 0.0; };

std::vector<GPTEvent> g_events;
std::vector<TrailPoint> g_trail;
bool g_recording = false;
bool g_enabled = true;
bool g_playing = false;
bool g_trajectory = true;
bool g_pathfinder = false;
double g_recordTime = 0.0;
double g_playTime = 0.0;
size_t g_playIndex = 0;
float g_sampleTimer = 0.f;
float g_speedMultiplier = 1.0f;
constexpr double kPlaybackSpeed = 15.0;
std::vector<std::filesystem::path> g_macroFiles;
size_t g_selectedMacro = 0;

std::string safeName(std::string name) {
    for (auto& c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) c = '_';
    }
    if (name.empty()) name = "uranium";
    return name;
}
std::filesystem::path macroDir() {
    auto path = Mod::get()->getSaveDir() / "macros";
    std::filesystem::create_directories(path);
    return path;
}
std::string currentLevelName() {
    auto pl = PlayLayer::get();
    if (pl && pl->m_level && !pl->m_level->m_levelName.empty()) return pl->m_level->m_levelName;
    return "uranium";
}
bool saveMacro(std::string name) {
    std::ofstream out(macroDir() / (safeName(std::move(name)) + ".gpt"), std::ios::binary);
    if (!out) return false;
    out << "GPT 2\n# time_seconds button pressed(0|1) x y\n";
    out.precision(9);
    for (auto const& e : g_events)
        out << e.timeSeconds << ' ' << e.button << ' ' << (e.pressed ? 1 : 0) << ' ' << e.x << ' ' << e.y << '\n';
    out << "# trajectory x y time_seconds\n";
    for (auto const& p : g_trail) out << "#P " << p.x << ' ' << p.y << ' ' << p.t << '\n';
    return static_cast<bool>(out);
}
void refreshMacroFiles() {
    g_macroFiles.clear();
    for (auto const& entry : std::filesystem::directory_iterator(macroDir())) {
        if (entry.is_regular_file() && entry.path().extension() == ".gpt") g_macroFiles.push_back(entry.path());
    }
    std::sort(g_macroFiles.begin(), g_macroFiles.end());
    if (g_macroFiles.empty()) g_selectedMacro = 0;
    else if (g_selectedMacro >= g_macroFiles.size()) g_selectedMacro = g_macroFiles.size() - 1;
}
bool loadMacro(std::filesystem::path const& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string header; std::getline(in, header);
    if (header != "GPT 1" && header != "GPT 2") return false;
    std::vector<GPTEvent> parsed;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line); GPTEvent e; int pressed = 0;
        if (!(row >> e.timeSeconds >> e.button >> pressed)) return false;
        if (e.timeSeconds < 0 || e.button < 0 || e.button > 16 || (pressed != 0 && pressed != 1)) return false;
        row >> e.x >> e.y;
        e.pressed = pressed == 1; parsed.push_back(e);
    }
    g_events = std::move(parsed); g_playIndex = 0; g_playTime = 0; return !g_events.empty();
}
void stopRecordingAndSave(bool autoSave) {
    if (!g_recording) return;
    g_recording = false;
    if (autoSave && !g_events.empty()) {
        if (saveMacro(currentLevelName())) {
            Notification::create("Macro saved automatically", NotificationIcon::Success, 2.5f)->show();
        } else {
            Notification::create("Macro auto-save failed", NotificationIcon::Error, 3.0f)->show();
        }
    }
}
void startRecording() {
    g_events.clear(); g_trail.clear(); g_recordTime = 0.0; g_sampleTimer = 0.f;
    g_playing = false; g_recording = true;
}
void addEvent(PlayerObject* player, int button, bool pressed) {
    if (!g_recording || !g_enabled || !player) return;
    auto pl = PlayLayer::get();
    if (!pl || (player != pl->m_player1 && player != pl->m_player2)) return;
    g_events.push_back({g_recordTime, button, pressed, player->getPositionX(), player->getPositionY()});
}

class UraniumPopup : public Popup {
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_selectedLabel = nullptr;
    bool init() {
        refreshMacroFiles();
        if (!Popup::init(340.f, 300.f)) return false;
        this->setTitle("Uranium");
        auto menu = CCMenu::create(); menu->setPosition(CCPointZero);
        auto add = [&](char const* title, CCPoint pos, SEL_MenuHandler cb, char const* sprite = "GJ_button_01.png") {
            auto spr = ButtonSprite::create(title, "bigFont.fnt", sprite, 0.65f);
            spr->setScale(0.62f);
            auto item = CCMenuItemSpriteExtra::create(spr, this, cb);
            item->setPosition(pos); menu->addChild(item);
        };
        add(g_recording ? "Stop + Save" : "Record", {86.f, 238.f}, menu_selector(UraniumPopup::onRecord), g_recording ? "GJ_button_06.png" : "GJ_button_01.png");
        add("Play 15x", {254.f, 238.f}, menu_selector(UraniumPopup::onPlay), "GJ_button_02.png");
        add("Stop Playback", {86.f, 198.f}, menu_selector(UraniumPopup::onStop), "GJ_button_06.png");
        add(g_trajectory ? "Trail: ON" : "Trail: OFF", {254.f, 198.f}, menu_selector(UraniumPopup::onTrail), "GJ_button_04.png");
        add("< Macro", {86.f, 158.f}, menu_selector(UraniumPopup::onPrevious), "GJ_button_01.png");
        add("Macro >", {254.f, 158.f}, menu_selector(UraniumPopup::onNext), "GJ_button_01.png");
        add("Load Selected", {170.f, 119.f}, menu_selector(UraniumPopup::onLoadSelected), "GJ_button_02.png");
        add("Save Macro", {86.f, 79.f}, menu_selector(UraniumPopup::onSave), "GJ_button_01.png");
        add("Speedhack", {254.f, 79.f}, menu_selector(UraniumPopup::onSpeed), "GJ_button_05.png");
        this->addChild(menu);
        m_selectedLabel = CCLabelBMFont::create("No saved macros", "chatFont.fnt");
        m_selectedLabel->setScale(0.55f); m_selectedLabel->setPosition({170.f, 48.f}); this->addChild(m_selectedLabel);
        updateSelectedLabel();
        m_status = CCLabelBMFont::create("Ready", "chatFont.fnt");
        m_status->setScale(0.62f); m_status->setPosition({170.f, 23.f}); this->addChild(m_status);
        return true;
    }
    void status(std::string const& s) { if (m_status) m_status->setString(s.c_str()); }
    void updateSelectedLabel() {
        refreshMacroFiles();
        if (!m_selectedLabel) return;
        if (g_macroFiles.empty()) m_selectedLabel->setString("No saved macros");
        else {
            auto name = g_macroFiles[g_selectedMacro].stem().string();
            if (name.size() > 28) name = name.substr(0, 25) + "...";
            m_selectedLabel->setString(name.c_str());
        }
    }
    void onPrevious(CCObject*) {
        refreshMacroFiles();
        if (g_macroFiles.empty()) { status("No saved macros"); return; }
        g_selectedMacro = (g_selectedMacro + g_macroFiles.size() - 1) % g_macroFiles.size();
        updateSelectedLabel(); status("Selected previous macro");
    }
    void onNext(CCObject*) {
        refreshMacroFiles();
        if (g_macroFiles.empty()) { status("No saved macros"); return; }
        g_selectedMacro = (g_selectedMacro + 1) % g_macroFiles.size();
        updateSelectedLabel(); status("Selected next macro");
    }
    void onLoadSelected(CCObject*) {
        refreshMacroFiles();
        if (g_macroFiles.empty()) { status("No saved macros to load"); return; }
        g_recording = false; g_playing = false;
        if (loadMacro(g_macroFiles[g_selectedMacro])) status("Loaded selected macro");
        else status("Could not load selected macro");
    }
    void onSpeed(CCObject*) {
        if (g_speedMultiplier < 1.5f) g_speedMultiplier = 2.f;
        else if (g_speedMultiplier < 3.5f) g_speedMultiplier = 5.f;
        else g_speedMultiplier = 1.f;
        status(std::string("Game speed: ") + std::to_string(static_cast<int>(g_speedMultiplier)) + "x");
    }
    void onRecord(CCObject*) {
        if (g_recording) { stopRecordingAndSave(true); status("Stopped; saved if events existed"); }
        else { startRecording(); status("Recording inputs + trajectory"); }
    }
    void onPlay(CCObject*) {
        if (g_events.empty()) { status("No macro yet - record first"); return; }
        g_recording = false; g_playing = true; g_playIndex = 0; g_playTime = 0.0; status("Playback running at 15x");
    }
    void onStop(CCObject*) { g_playing = false; status("Playback stopped"); }
    void onTrail(CCObject*) { g_trajectory = !g_trajectory; status(g_trajectory ? "Trajectory enabled" : "Trajectory hidden"); }
    void onPathfinder(CCObject*) {
        g_pathfinder = !g_pathfinder;
        status(g_pathfinder ? "Hitbox scan is experimental" : "Pathfinder disabled");
        if (g_pathfinder) Notification::create("Hitbox mapping is experimental; it does not guarantee a 100% route", NotificationIcon::Warning, 3.5f)->show();
    }
    void onSave(CCObject*) {
        auto ok = saveMacro(currentLevelName());
        refreshMacroFiles(); updateSelectedLabel();
        status(ok ? "Saved in Geode/macros" : "Save failed");
    }
public:
    static UraniumPopup* create() { auto ret = new UraniumPopup(); if (ret->init()) { ret->autorelease(); return ret; } delete ret; return nullptr; }
};
}

class $modify(UraniumPlayerObject, PlayerObject) {
    bool pushButton(PlayerButton button) {
        auto result = PlayerObject::pushButton(button);
        addEvent(this, static_cast<int>(button), true);
        return result;
    }
    bool releaseButton(PlayerButton button) {
        auto result = PlayerObject::releaseButton(button);
        addEvent(this, static_cast<int>(button), false);
        return result;
    }
};

class $modify(UraniumPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        g_recording = false; g_playing = false; g_recordTime = 0; g_sampleTimer = 0;
        return true;
    }
    void update(float dt) {
        float gameDt = dt * g_speedMultiplier;
        PlayLayer::update(gameDt);
        if (g_recording && g_enabled) {
            g_recordTime += gameDt;
            g_sampleTimer += gameDt;
            if (g_trajectory && g_sampleTimer >= 0.025f) {
                g_sampleTimer = 0.f;
                if (m_player1) g_trail.push_back({m_player1->getPositionX(), m_player1->getPositionY(), g_recordTime});
            }
        }
        if (g_playing && g_enabled && m_player1) {
            g_playTime += static_cast<double>(gameDt) * kPlaybackSpeed;
            while (g_playIndex < g_events.size() && g_events[g_playIndex].timeSeconds <= g_playTime) {
                auto const& e = g_events[g_playIndex++];
                auto btn = static_cast<PlayerButton>(e.button);
                if (e.pressed) m_player1->pushButton(btn); else m_player1->releaseButton(btn);
            }
            if (g_playIndex >= g_events.size()) g_playing = false;
        }
    }
    void levelComplete() {
        g_playing = false;
        stopRecordingAndSave(true);
        PlayLayer::levelComplete();
    }
    void onQuit() {
        g_playing = false;
        stopRecordingAndSave(true);
        PlayLayer::onQuit();
    }
};

class $modify(UraniumPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto win = CCDirector::get()->getWinSize();
        auto item = CCMenuItemSpriteExtra::create(
            CCSprite::createWithSpriteFrameName("GJ_hammerIcon_001.png"), this,
            menu_selector(UraniumPauseLayer::onOpenGPT));
        if (!item) return;
        item->setID("gpt-uranium-open-button"_spr);
        item->setScale(0.55f); item->setPosition({win.width - 28.f, win.height * 0.50f});
        if (auto menu = this->getChildByID("right-button-menu")) { menu->addChild(item); menu->updateLayout(); }
        else this->addChild(item);
    }
    void onOpenGPT(CCObject*) { if (auto popup = UraniumPopup::create()) popup->show(); }
};
