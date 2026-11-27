/*
    Media segments (intro, credits, ...) published by the server, see
    /MediaSegments in the Jellyfin API (10.10+), e.g. from Intro Skipper.
*/

#pragma once

#include <string>

namespace brls {
class Box;
}

namespace segment {

enum class Kind { INTRO, RECAP, PREVIEW, COMMERCIAL, CREDITS, COUNT };

/// Stored in the config, the values are also the selector indexes
enum class Action { NONE = 0, ASK = 1, SKIP = 2 };

/// @brief Map a MediaSegmentType ("Outro" on 10.10 is "Credits" later), false for unknown types
bool kindOf(const std::string& type, Kind& kind);

Action action(Kind kind);

/// "Skip Intro", "Skip Credits" ...
std::string skipLabel(Kind kind);

/// Show the Up Next card before the end of an episode
inline bool UP_NEXT = true;

/// @brief Read the per-type actions from the config
void load();

/// @brief Append the segment setting cells to @p box, shared by the settings tab and the player menu
void addSettingCells(brls::Box* box);

}  // namespace segment
