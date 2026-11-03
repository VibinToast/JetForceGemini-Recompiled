#pragma once
// Jet Force Gemini port: RetroAchievements, casual only (jfg_achievements.cpp).
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

enum class JfgRaState { LoggedOut, LoggingIn, LoggedIn, Failed };

// What the launcher's RetroAchievements block (Play page) shows.
struct JfgRaStatus {
    JfgRaState state = JfgRaState::LoggedOut;
    std::string user;      // display name, when logged in
    std::string message;   // why a login failed
    uint32_t points = 0;   // the user's softcore points
    uint32_t unlocked = 0; // this game's achievements: unlocked and in total, once the game is loaded
    uint32_t total = 0;
    bool game_loaded = false;
    std::string caution;   // gameplay mods or options on that make some achievements easier, or empty
    uint32_t serial = 0;   // changes whenever anything above does
};

// At start-up: logs in again with the token of an earlier login, if there is one; otherwise nothing runs.
void jfg_achievements_init(const std::filesystem::path& app_folder, const std::string& version);
// The launcher's RetroAchievements block (Play page). The password is used once and not kept; logging out forgets the login.
void jfg_achievements_login(const std::string& user, const std::string& password);
void jfg_achievements_logout();
JfgRaStatus jfg_achievements_status();
// Once per game tick, on the game thread (jfg_debug_frame). The set is checked once per video frame the tick covers.
void jfg_achievements_frame(uint8_t* rdram);
// While the game isn't running frames (launcher, pause): keeps requests moving.
void jfg_achievements_idle();
// On-screen banners (unlocks, the game's progress when it loads, mastery), one at a time for JFG_RA_BANNER_SECONDS.
struct JfgRaBanner {
    std::string header; // "Achievement unlocked"
    std::string title;  // the achievement's title
    std::string detail; // its description
    uint32_t points = 0;
};
constexpr float JFG_RA_BANNER_SECONDS = 5.0f;
// The banner to show now, and how long it has been shown; false when there is none.
bool jfg_achievements_banner(JfgRaBanner& out, float& seconds);

// Challenge indicators: the badges of achievements that are primed right now (every condition met but the last), for
// a corner of the screen. rgba is null while the badge is still loading (or couldn't be had): draw a placeholder.
struct JfgRaIndicator {
    std::string title;
    int width = 0, height = 0;
    std::shared_ptr<const std::vector<uint8_t>> rgba;
};
std::vector<JfgRaIndicator> jfg_achievements_indicators();
