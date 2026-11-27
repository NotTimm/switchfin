#include "view/media_segment.hpp"
#include "utils/config.hpp"
#include <borealis.hpp>

using namespace brls::literals;

namespace segment {

struct Entry {
    AppConfig::Item item;
    Action fallback;
    const char* title;
    const char* skip;
};

static const Entry entries[] = {
    {AppConfig::PLAYER_SEGMENT_INTRO, Action::ASK, "main/setting/segment/intro", "main/player/skip_intro"},
    {AppConfig::PLAYER_SEGMENT_RECAP, Action::ASK, "main/setting/segment/recap", "main/player/skip_recap"},
    {AppConfig::PLAYER_SEGMENT_PREVIEW, Action::NONE, "main/setting/segment/preview", "main/player/skip_preview"},
    {AppConfig::PLAYER_SEGMENT_COMMERCIAL, Action::NONE, "main/setting/segment/commercial",
        "main/player/skip_commercial"},
    {AppConfig::PLAYER_SEGMENT_CREDITS, Action::ASK, "main/setting/segment/credits", "main/player/skip_credits"},
};
static_assert(sizeof(entries) / sizeof(entries[0]) == size_t(Kind::COUNT));

static Action actions[size_t(Kind::COUNT)];

bool kindOf(const std::string& type, Kind& kind) {
    if (type == "Intro") {
        kind = Kind::INTRO;
    } else if (type == "Recap") {
        kind = Kind::RECAP;
    } else if (type == "Preview") {
        kind = Kind::PREVIEW;
    } else if (type == "Commercial") {
        kind = Kind::COMMERCIAL;
    } else if (type == "Credits" || type == "Outro") {
        kind = Kind::CREDITS;
    } else {
        return false;
    }
    return true;
}

Action action(Kind kind) { return actions[size_t(kind)]; }

std::string skipLabel(Kind kind) { return brls::getStr(entries[size_t(kind)].skip); }

static Action toAction(int value, Action fallback) {
    if (value < int(Action::NONE) || value > int(Action::SKIP)) return fallback;
    return Action(value);
}

void load() {
    auto& conf = AppConfig::instance();
    for (size_t i = 0; i < size_t(Kind::COUNT); i++) {
        const Entry& e = entries[i];
        actions[i] = toAction(conf.getItem(e.item, int(e.fallback)), e.fallback);
    }
    UP_NEXT = conf.getItem(AppConfig::PLAYER_UP_NEXT, true);
}

void addSettingCells(brls::Box* box) {
    std::vector<std::string> options = {
        "main/setting/segment/none"_i18n,
        "main/setting/segment/ask"_i18n,
        "main/setting/segment/skip"_i18n,
    };
    for (size_t i = 0; i < size_t(Kind::COUNT); i++) {
        auto* cell = new brls::SelectorCell();
        cell->init(brls::getStr(entries[i].title), options, int(actions[i]), [i](int selected) {
            actions[i] = toAction(selected, entries[i].fallback);
            AppConfig::instance().setItem(entries[i].item, selected);
        });
        box->addView(cell);
    }

    auto* upNext = new brls::BooleanCell();
    upNext->init("main/setting/segment/up_next"_i18n, UP_NEXT, [](bool value) {
        UP_NEXT = value;
        AppConfig::instance().setItem(AppConfig::PLAYER_UP_NEXT, value);
    });
    box->addView(upNext);
}

}  // namespace segment
