#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/Notification.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Macro data
// ---------------------------------------------------------------------------

struct MacroInput {
    int frame;   // physics tick (m_gameState.m_currentProgress)
    int button;  // 1 = jump, 2 = left, 3 = right
    bool player1;
    bool down;
};

enum class Mode { Off, Record, Play };

namespace macro {
    Mode mode = Mode::Off;
    std::string name = "";
    std::vector<MacroInput> inputs;
    size_t playIndex = 0;
    bool injecting = false; // true while *we* call handleButton during playback

    std::filesystem::path dir() {
        auto d = Mod::get()->getSaveDir() / "macros";
        (void)file::createDirectoryAll(d);
        return d;
    }

    std::string sanitize(std::string s) {
        std::string out;
        for (char c : s) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' || c == '(' || c == ')') {
                out += c;
            }
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        while (!out.empty() && out.front() == ' ') out.erase(out.begin());
        return out;
    }

    Result<> save(std::string const& rawName) {
        auto fileName = sanitize(rawName);
        if (fileName.empty()) return Err("Give the macro a name first");
        if (inputs.empty()) return Err("There's nothing recorded yet");

        auto arr = matjson::Value::array();
        for (auto const& in : inputs) {
            auto row = matjson::Value::array();
            row.push(in.frame);
            row.push(in.button);
            row.push(in.player1);
            row.push(in.down);
            arr.push(row);
        }
        auto obj = matjson::makeObject({
            {"name", fileName},
            {"tps", 240},
            {"inputs", arr},
        });
        GEODE_UNWRAP(file::writeStringSafe(dir() / (fileName + ".json"), obj.dump(matjson::NO_INDENTATION)));
        name = fileName;
        return Ok();
    }

    Result<> load(std::filesystem::path const& path) {
        GEODE_UNWRAP_INTO(auto json, file::readJson(path));
        std::vector<MacroInput> loaded;
        if (auto arr = json["inputs"].asArray()) {
            for (auto& row : arr.unwrap()) {
                MacroInput in;
                in.frame = static_cast<int>(row[0].asInt().unwrapOr(0));
                in.button = static_cast<int>(row[1].asInt().unwrapOr(1));
                in.player1 = row[2].asBool().unwrapOr(true);
                in.down = row[3].asBool().unwrapOr(false);
                loaded.push_back(in);
            }
        } else {
            return Err("Macro file is missing its inputs");
        }
        std::stable_sort(loaded.begin(), loaded.end(), [](auto& a, auto& b) { return a.frame < b.frame; });
        inputs = std::move(loaded);
        name = path.stem().string();
        playIndex = 0;
        return Ok();
    }

    std::vector<std::filesystem::path> list() {
        std::vector<std::filesystem::path> out;
        if (auto res = file::readDirectory(dir())) {
            for (auto& p : res.unwrap()) {
                if (p.extension() == ".json") out.push_back(p);
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    char const* modeName() {
        switch (mode) {
            case Mode::Record: return "Recording";
            case Mode::Play: return "Playing";
            default: return "Off";
        }
    }
}

// ---------------------------------------------------------------------------
// Recording + playback hooks
// ---------------------------------------------------------------------------

class $modify(MacroGameLayer, GJBaseGameLayer) {
    bool isRealLevel() {
        return PlayLayer::get() == static_cast<GJBaseGameLayer*>(this);
    }

    int currentFrame() {
        return static_cast<int>(m_gameState.m_currentProgress);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!isRealLevel() || macro::injecting) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        if (macro::mode == Mode::Play) {
            // The macro is in control; ignore the real player's clicks.
            return;
        }

        if (macro::mode == Mode::Record) {
            macro::inputs.push_back({ currentFrame(), button, isPlayer1, down });
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (isRealLevel() && macro::mode == Mode::Play) {
            int frame = currentFrame();
            auto& inputs = macro::inputs;
            while (macro::playIndex < inputs.size() && inputs[macro::playIndex].frame <= frame) {
                auto const& in = inputs[macro::playIndex++];
                macro::injecting = true;
                GJBaseGameLayer::handleButton(in.down, in.button, in.player1);
                macro::injecting = false;
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(MacroPlayLayer, PlayLayer) {
    struct Fields {
        bool originalTestMode = false;
        CCLabelBMFont* statusLabel = nullptr;
        Mode shownMode = Mode::Off;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        m_fields->originalTestMode = m_isTestMode;

        auto win = CCDirector::get()->getWinSize();
        auto label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setScale(0.35f);
        label->setOpacity(160);
        label->setAnchorPoint({ 0.f, 0.f });
        label->setPosition({ 5.f, 5.f });
        label->setID("status-label"_spr);
        this->addChild(label, 1000);
        m_fields->statusLabel = label;
        this->updateMacroStatus();
        this->schedule(schedule_selector(MacroPlayLayer::tickStatus), 0.1f);

        macro::playIndex = 0;
        return true;
    }

    void tickStatus(float) {
        this->updateMacroStatus();
    }

    void updateMacroStatus() {
        // Playing a macro counts as a safe-mode attempt: no progress, no completions saved.
        m_isTestMode = macro::mode == Mode::Play ? true : m_fields->originalTestMode;

        auto label = m_fields->statusLabel;
        if (!label) return;
        switch (macro::mode) {
            case Mode::Record:
                label->setString(fmt::format("REC  {} inputs", macro::inputs.size()).c_str());
                label->setColor({ 255, 90, 90 });
                label->setVisible(true);
                break;
            case Mode::Play:
                label->setString(fmt::format("PLAY  {}", macro::name.empty() ? "macro" : macro::name).c_str());
                label->setColor({ 120, 255, 120 });
                label->setVisible(true);
                break;
            default:
                label->setVisible(false);
                break;
        }
    }

    void resetLevel() {
        this->updateMacroStatus();
        PlayLayer::resetLevel();

        int frame = static_cast<int>(m_gameState.m_currentProgress);

        if (macro::mode == Mode::Record) {
            auto& inputs = macro::inputs;

            // Drop everything that happened after the point we respawned at.
            std::erase_if(inputs, [&](MacroInput const& in) { return in.frame >= frame; });

            // Anything that was still held at that point gets released, so playback
            // doesn't keep holding a button the player let go of while dead.
            std::array<bool, 8> held{};
            for (auto const& in : inputs) {
                int idx = (in.player1 ? 0 : 4) + (in.button & 3);
                held[idx] = in.down;
            }
            for (int i = 0; i < 8; i++) {
                if (held[i]) {
                    inputs.push_back({ frame, i & 3, i < 4, false });
                }
            }
        }

        if (macro::mode == Mode::Play) {
            auto& inputs = macro::inputs;
            macro::playIndex = std::lower_bound(
                inputs.begin(), inputs.end(), frame,
                [](MacroInput const& in, int f) { return in.frame < f; }
            ) - inputs.begin();
        }
    }
};

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

class MacroPopup : public Popup {
protected:
    CCLabelBMFont* m_status = nullptr;
    TextInput* m_nameInput = nullptr;
    ScrollLayer* m_list = nullptr;
    PauseLayer* m_pauseLayer = nullptr;
    std::array<CCMenuItemSpriteExtra*, 3> m_modeBtns{};

    static constexpr float W = 380.f;
    static constexpr float H = 270.f;

    bool init(PauseLayer* pause) {
        if (!Popup::init(W, H)) return false;
        m_pauseLayer = pause;
        this->setTitle("Macros Special");

        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        m_mainLayer->addChild(menu);

        m_status = CCLabelBMFont::create("", "chatFont.fnt");
        m_status->setPosition({ W / 2, H - 50.f });
        m_status->setScale(0.8f);
        m_mainLayer->addChild(m_status);

        // Mode buttons
        char const* labels[] = { "Record", "Play", "Off" };
        for (int i = 0; i < 3; i++) {
            auto spr = ButtonSprite::create(labels[i], 70, true, "goldFont.fnt", "GJ_button_01.png", 28.f, 0.7f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(MacroPopup::onMode));
            btn->setTag(i);
            btn->setPosition({ W / 2 + (i - 1) * 105.f, H - 85.f });
            menu->addChild(btn);
            m_modeBtns[i] = btn;
        }

        // Name + save
        m_nameInput = TextInput::create(220.f, "Macro name");
        m_nameInput->setCommonFilter(CommonFilter::Any);
        m_nameInput->setMaxCharCount(40);
        m_nameInput->setPosition({ 140.f, H - 125.f });
        m_nameInput->setString(macro::name);
        m_mainLayer->addChild(m_nameInput);

        auto saveSpr = ButtonSprite::create("Save", 60, true, "goldFont.fnt", "GJ_button_02.png", 28.f, 0.7f);
        auto saveBtn = CCMenuItemSpriteExtra::create(saveSpr, this, menu_selector(MacroPopup::onSave));
        saveBtn->setPosition({ 310.f, H - 125.f });
        menu->addChild(saveBtn);

        // Saved macro list
        auto listBg = CCScale9Sprite::create("square02b_001.png", { 0, 0, 80, 80 });
        listBg->setColor({ 0, 0, 0 });
        listBg->setOpacity(80);
        listBg->setContentSize({ W - 40.f, 100.f });
        listBg->setPosition({ W / 2, 70.f });
        m_mainLayer->addChild(listBg);

        m_list = ScrollLayer::create({ W - 40.f, 100.f });
        m_list->setPosition({ 20.f, 20.f });
        m_mainLayer->addChild(m_list);

        this->refresh();
        return true;
    }

    void refresh() {
        m_status->setString(fmt::format(
            "Mode: {}   |   {} inputs   |   {}",
            macro::modeName(), macro::inputs.size(),
            macro::name.empty() ? "unsaved" : macro::name
        ).c_str());

        for (int i = 0; i < 3; i++) {
            bool active = static_cast<int>(macro::mode) == (i == 2 ? 0 : i + 1);
            auto spr = static_cast<ButtonSprite*>(m_modeBtns[i]->getNormalImage());
            spr->setColor(active ? ccColor3B{ 255, 255, 255 } : ccColor3B{ 140, 140, 140 });
        }

        this->buildList();
    }

    void buildList() {
        auto content = m_list->m_contentLayer;
        content->removeAllChildren();

        auto files = macro::list();
        constexpr float rowH = 28.f;
        float viewH = m_list->getContentSize().height;
        float totalH = std::max(viewH, rowH * files.size());
        content->setContentSize({ W - 40.f, totalH });

        if (files.empty()) {
            auto empty = CCLabelBMFont::create("No saved macros yet", "chatFont.fnt");
            empty->setPosition({ (W - 40.f) / 2, totalH / 2 });
            empty->setOpacity(150);
            content->addChild(empty);
        }

        for (size_t i = 0; i < files.size(); i++) {
            float y = totalH - rowH * (i + 0.5f);
            auto stem = files[i].stem().string();

            auto label = CCLabelBMFont::create(stem.c_str(), "bigFont.fnt");
            label->limitLabelWidth(200.f, 0.45f, 0.1f);
            label->setAnchorPoint({ 0.f, 0.5f });
            label->setPosition({ 10.f, y });
            content->addChild(label);

            auto rowMenu = CCMenu::create();
            rowMenu->setPosition({ 0.f, 0.f });
            content->addChild(rowMenu);

            auto loadSpr = ButtonSprite::create("Load", 40, true, "goldFont.fnt", "GJ_button_01.png", 22.f, 0.55f);
            auto loadBtn = CCMenuItemSpriteExtra::create(loadSpr, this, menu_selector(MacroPopup::onLoad));
            loadBtn->setUserObject(CCString::create(files[i].string()));
            loadBtn->setPosition({ W - 120.f, y });
            rowMenu->addChild(loadBtn);

            auto delSpr = CCSprite::createWithSpriteFrameName("GJ_deleteIcon_001.png");
            delSpr->setScale(0.6f);
            auto delBtn = CCMenuItemSpriteExtra::create(delSpr, this, menu_selector(MacroPopup::onDelete));
            delBtn->setUserObject(CCString::create(files[i].string()));
            delBtn->setPosition({ W - 70.f, y });
            rowMenu->addChild(delBtn);
        }

        content->setPositionY(viewH - totalH); // start scrolled to the top
    }

    void restartAndResume() {
        // Recording and playback both need to start from a clean attempt.
        this->onClose(nullptr);
        if (m_pauseLayer) {
            m_pauseLayer->onRestart(nullptr);
        }
    }

    void onMode(CCObject* sender) {
        int tag = sender->getTag();
        if (tag == 0) {
            macro::mode = Mode::Record;
            macro::inputs.clear();
            macro::name.clear();
            Notification::create("Recording started", NotificationIcon::Info)->show();
            this->restartAndResume();
        } else if (tag == 1) {
            if (macro::inputs.empty()) {
                Notification::create("Load or record a macro first", NotificationIcon::Error)->show();
                return;
            }
            macro::mode = Mode::Play;
            macro::playIndex = 0;
            Notification::create("Playing macro", NotificationIcon::Info)->show();
            this->restartAndResume();
        } else {
            macro::mode = Mode::Off;
            this->refresh();
        }
    }

    void onSave(CCObject*) {
        auto res = macro::save(m_nameInput->getString());
        if (!res) {
            Notification::create(res.unwrapErr(), NotificationIcon::Error)->show();
            return;
        }
        Notification::create(fmt::format("Saved \"{}\"", macro::name), NotificationIcon::Success)->show();
        this->refresh();
    }

    void onLoad(CCObject* sender) {
        auto path = static_cast<CCString*>(static_cast<CCNode*>(sender)->getUserObject())->getCString();
        auto res = macro::load(path);
        if (!res) {
            Notification::create(res.unwrapErr(), NotificationIcon::Error)->show();
            return;
        }
        if (macro::mode == Mode::Record) macro::mode = Mode::Off;
        m_nameInput->setString(macro::name);
        Notification::create(fmt::format("Loaded \"{}\" - press Play", macro::name), NotificationIcon::Success)->show();
        this->refresh();
    }

    void onDelete(CCObject* sender) {
        std::string path = static_cast<CCString*>(static_cast<CCNode*>(sender)->getUserObject())->getCString();
        auto stem = std::filesystem::path(path).stem().string();
        Ref<MacroPopup> self = this;
        createQuickPopup(
            "Delete macro",
            fmt::format("Delete <cy>{}</c>? This can't be undone.", stem),
            "Cancel", "Delete",
            [self, path](FLAlertLayer*, bool yes) {
                if (!yes) return;
                std::error_code ec;
                std::filesystem::remove(path, ec);
                self->refresh();
            }
        );
    }

public:
    static MacroPopup* create(PauseLayer* pause) {
        auto ret = new MacroPopup();
        if (ret->init(pause)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class $modify(MacroPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto spr = CircleButtonSprite::createWithSpriteFrameName(
            "GJ_playBtn2_001.png", 0.9f, CircleBaseColor::Green, CircleBaseSize::Small
        );
        if (!spr) return;
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(MacroPauseLayer::onMacros));
        btn->setID("macros-button"_spr);

        if (auto menu = this->getChildByID("left-button-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            auto newMenu = CCMenu::create();
            newMenu->setPosition({ 35.f, CCDirector::get()->getWinSize().height / 2 });
            newMenu->addChild(btn);
            this->addChild(newMenu);
        }
    }

    void onMacros(CCObject*) {
        if (auto popup = MacroPopup::create(this)) popup->show();
    }
};
