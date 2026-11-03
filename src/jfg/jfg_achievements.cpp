// Jet Force Gemini port: RetroAchievements, casual (softcore) only.
//
// RetroAchievements (RA) has a set for Jet Force Gemini (game 30). This runs RA's client library, rcheevos' rc_client,
// against the recompiled game: it reads the game's RDRAM, which N64Recomp keeps in the layout RA's sets were made
// against (the N64's 8 MB at offsets 0-0x7FFFFF, as 32-bit words in host byte order, the way mupen64plus stores it),
// and evaluates the set once per video frame (jfg_debug_frame, see jfg_achievements_frame).
//
// - Casual only. RA turns unlocks from a client it hasn't approved into softcore unlocks, and a recompilation is not
//   an approved emulator, so hardcore is never asked for. Everything the port adds (modern controls, 60 FPS, cutscene
//   skip, cheats) stays available.
// - Nothing runs and nothing is sent until the player logs in (the RetroAchievements block on the launcher's Play page).
//   Logging in uses the password once: RA hands back a token, which is kept in retroachievements.json in the app folder;
//   the password is not stored. Logging out deletes it.
// - Requests go to RA on worker threads (jfg_http_request); their replies are handed to rc_client on the game thread.
// - Built from the lib/rcheevos submodule (JFG_HAS_RCHEEVOS); with -DJFG_RETROACHIEVEMENTS=OFF these functions do nothing.
//
// See docs/RETROACHIEVEMENTS.md.
#include "jfg_achievements.h"

#ifdef JFG_HAS_RCHEEVOS

#include <algorithm>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "json/json.hpp"
#include "rc_client.h"
#include "rc_consoles.h"

#include "jfg/jfg_options.h"
#include "jfg/jfg_update_check.h"
#include "librecomp/mods.hpp"

// Badge images (PNG): a private copy of stb_image, PNG only.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../../lib/rt64/src/contrib/stb/stb_image.h"
#include <map>
#include <memory>
#include <set>

namespace fs = std::filesystem;

namespace {
constexpr uint32_t RDRAM_SIZE = 0x800000; // RA's N64 map: 8 MB of RDRAM (with the expansion pak)
constexpr uint32_t DELAY_DAT_OFFSET = 0xA3374; // delayDat (jfg_game.h ADDR_delayDat): video frames this tick covers

rc_client_t* g_client = nullptr;
fs::path g_folder;                 // the app folder
std::string g_version;
std::string g_user_agent;
std::atomic<uint8_t*> g_rdram{ nullptr };
std::atomic<bool> g_game_requested{ false };

// Replies from the worker threads, handed to rc_client on the game thread.
std::mutex g_reply_mutex;
std::deque<std::function<void()>> g_replies;

// What the launcher's RetroAchievements block shows (jfg_achievements_status).
std::mutex g_status_mutex;
JfgRaStatus g_status;

void set_status(JfgRaState state, const std::string& message) {
    std::lock_guard lock(g_status_mutex);
    g_status.state = state;
    g_status.message = message;
    g_status.serial++;
}

// Challenge indicators: achievements that are primed (every condition met but the last, like "no damage taken" in a
// boss fight), shown as their badges in a corner until they unlock or fail. Badges are downloaded once into ra_badges/
// in the app folder and decoded to RGBA for the overlay.
struct Badge {
    int width = 0, height = 0;
    std::shared_ptr<const std::vector<uint8_t>> rgba; // empty while loading, or when it couldn't be had
};
std::mutex g_challenge_mutex;
std::map<std::string, Badge> g_badges; // by badge name
struct Challenge { uint32_t id; std::string badge; std::string title; };
std::vector<Challenge> g_challenges;

void load_badge(const std::string& name, const std::string& url, const fs::path& local_override) {
    {
        std::lock_guard lock(g_challenge_mutex);
        if (g_badges.count(name)) return; // loaded or on its way
        g_badges[name] = Badge{};
    }
    std::thread([=]() {
        fs::path file = local_override;
        std::error_code ec;
        if (file.empty()) {
            const fs::path dir = g_folder / "ra_badges";
            fs::create_directories(dir, ec);
            file = dir / (name + ".png");
            if (!fs::exists(file, ec) && !url.empty()) {
                const fs::path part = dir / (name + ".png.part");
                if (jfg_http_download(url, part, 0, nullptr) == 1) fs::rename(part, file, ec);
                else fs::remove(part, ec);
            }
        }
        int w = 0, h = 0, n = 0;
        stbi_uc* pixels = stbi_load(file.string().c_str(), &w, &h, &n, 4);
        Badge badge;
        if (pixels != nullptr) {
            badge.width = w;
            badge.height = h;
            badge.rgba = std::make_shared<const std::vector<uint8_t>>(pixels, pixels + (size_t)w * h * 4);
            stbi_image_free(pixels);
        }
        std::lock_guard lock(g_challenge_mutex);
        g_badges[name] = badge;
    }).detach();
}

void show_challenge(uint32_t id, const std::string& badge, const std::string& url, const std::string& title, const fs::path& local = {}) {
    load_badge(badge, url, local);
    std::lock_guard lock(g_challenge_mutex);
    for (const Challenge& c : g_challenges) if (c.id == id) return;
    g_challenges.push_back(Challenge{ id, badge, title });
}

void hide_challenge(uint32_t id) {
    std::lock_guard lock(g_challenge_mutex);
    std::erase_if(g_challenges, [id](const Challenge& c) { return c.id == id; });
}

// Banners for the overlay (jfg_tick.cpp), shown one after another: each from when it first reaches the front.
std::mutex g_banner_mutex;
std::deque<JfgRaBanner> g_banners;
std::chrono::steady_clock::time_point g_banner_shown{};
bool g_banner_showing = false;

void log_line(const std::string& text) {
    std::fprintf(stderr, "[ra] %s\n", text.c_str());
}

void banner(const std::string& header, const std::string& title, const std::string& detail, uint32_t points) {
    std::lock_guard lock(g_banner_mutex);
    if (g_banners.size() >= 16) g_banners.pop_front(); // a burst (a save loaded with many unlocks): keep the latest
    g_banners.push_back(JfgRaBanner{ header, title, detail, points });
}

uint32_t RC_CCONV read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t*) {
    uint8_t* rdram = g_rdram.load();
    if (rdram == nullptr || address >= RDRAM_SIZE) return 0;
    if (num_bytes > RDRAM_SIZE - address) num_bytes = RDRAM_SIZE - address;
    std::memcpy(buffer, rdram + address, num_bytes); // already in the byte order RA's N64 sets expect
    return num_bytes;
}

void RC_CCONV server_call(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data, rc_client_t*) {
    std::string url = request->url;
    const bool post = request->post_data != nullptr;
    std::string post_data = post ? request->post_data : "";
    std::string content_type = request->content_type != nullptr ? request->content_type : "";
    std::thread([=]() {
        int status = 0;
        std::string body;
        const bool ok = jfg_http_request(url, post ? &post_data : nullptr, content_type, g_user_agent, status, body);
        std::lock_guard lock(g_reply_mutex);
        g_replies.emplace_back([=]() {
            rc_api_server_response_t response{};
            response.body = body.c_str();
            response.body_length = body.size();
            response.http_status_code = ok ? status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
            callback(&response, callback_data);
        });
    }).detach();
}

void run_replies() {
    std::deque<std::function<void()>> replies;
    {
        std::lock_guard lock(g_reply_mutex);
        replies.swap(g_replies);
    }
    for (auto& reply : replies) reply();
}

void RC_CCONV event_handler(const rc_client_event_t* event, rc_client_t*) {
    switch (event->type) {
        case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
            if (event->achievement != nullptr) {
                hide_challenge(event->achievement->id);
                banner("Achievement unlocked", event->achievement->title,
                       event->achievement->description != nullptr ? event->achievement->description : "", event->achievement->points);
                log_line(std::string("unlocked: ") + event->achievement->title);
            }
            break;
        case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
            if (event->achievement != nullptr) {
                show_challenge(event->achievement->id, event->achievement->badge_name,
                               event->achievement->badge_url != nullptr ? event->achievement->badge_url : "", event->achievement->title);
            }
            break;
        case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
            if (event->achievement != nullptr) hide_challenge(event->achievement->id);
            break;
        case RC_CLIENT_EVENT_GAME_COMPLETED:
            banner("Mastered", "Jet Force Gemini", "Every achievement in the set is unlocked.", 0);
            break;
        case RC_CLIENT_EVENT_SERVER_ERROR:
            if (event->server_error != nullptr && event->server_error->error_message != nullptr)
                log_line(std::string("server error: ") + event->server_error->error_message);
            break;
        case RC_CLIENT_EVENT_DISCONNECTED:
            log_line("offline: unlocks are kept and sent later");
            break;
        case RC_CLIENT_EVENT_RECONNECTED:
            log_line("back online: pending unlocks sent");
            break;
        default:
            break;
    }
}

void RC_CCONV log_message(const char* message, const rc_client_t*) {
    log_line(message);
}

fs::path token_file() { return g_folder / "retroachievements.json"; }

void save_token(const char* username, const char* token) {
    nlohmann::json j;
    j["username"] = username;
    j["token"] = token;
    std::ofstream out(token_file(), std::ios::trunc);
    out << j.dump(2) << "\n";
    out.close();
    // Only the token is kept (never the password), readable by the player's account alone where permissions apply.
    std::error_code ec;
    fs::permissions(token_file(), fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
}

void RC_CCONV login_done(int result, const char* error, rc_client_t* client, void*) {
    if (result != RC_OK) {
        const std::string why = error != nullptr ? error : "unknown error";
        log_line("login failed: " + why);
        set_status(JfgRaState::Failed, why);
        return;
    }
    const rc_client_user_t* user = rc_client_get_user_info(client);
    if (user != nullptr) {
        save_token(user->username, user->token);
        log_line(std::string("logged in as ") + user->display_name);
        {
            std::lock_guard lock(g_status_mutex);
            g_status.user = user->display_name;
            g_status.points = user->score_softcore;
        }
        set_status(JfgRaState::LoggedIn, "");
    }
}

std::string options_caution(); // below

void RC_CCONV game_loaded(int result, const char* error, rc_client_t* client, void*) {
    if (result != RC_OK) {
        log_line(std::string("game not loaded: ") + (error != nullptr ? error : "unknown error"));
        return;
    }
    const rc_client_game_t* game = rc_client_get_game_info(client);
    rc_client_user_game_summary_t summary{};
    rc_client_get_user_game_summary(client, &summary);
    if (game != nullptr) {
        log_line(std::string("game: ") + game->title + ", " + std::to_string(summary.num_unlocked_achievements) + "/" +
                 std::to_string(summary.num_core_achievements) + " unlocked");
        {
            std::lock_guard lock(g_status_mutex);
            g_status.game_loaded = true;
            g_status.unlocked = summary.num_unlocked_achievements;
            g_status.total = summary.num_core_achievements;
            g_status.serial++;
        }
        banner("RetroAchievements", game->title,
               std::to_string(summary.num_unlocked_achievements) + " of " + std::to_string(summary.num_core_achievements) +
                   " achievements unlocked (casual)", 0);
        const std::string caution = options_caution();
        if (!caution.empty()) banner("RetroAchievements", "Note", caution, 0);
    }
}

void ensure_client() {
    if (g_client != nullptr) return;
    g_client = rc_client_create(read_memory, server_call);
    rc_client_enable_logging(g_client, RC_CLIENT_LOG_LEVEL_WARN, log_message);
    rc_client_set_event_handler(g_client, event_handler);
    rc_client_set_hardcore_enabled(g_client, 0); // casual only
    char clause[128] = {};
    rc_client_get_user_agent_clause(g_client, clause, sizeof(clause));
    g_user_agent = "JetForceGeminiRecompiled/" + g_version + " " + clause;
}
// Mods that change gameplay are warned about, like the options below: achievements keep counting (casual mode allows
// save states, which change far more), but a mod can change what a challenge asks (more enemies, other weapons). Custom levels come as mods too, tagged
// Levels. A mod tagged only HUD, Controls, Visuals or Audio isn't mentioned; a mod without tags counts as gameplay.
// Returns the gameplay mods that are on, by name.
std::vector<std::string> gameplay_mods_on() {
    static const std::set<std::string> cosmetic = { "HUD", "Controls", "Visuals", "Audio" };
    std::vector<std::string> names;
    for (const recomp::mods::ModDetails& d : recomp::mods::get_all_mod_details("jet-force-gemini")) {
        if (!recomp::mods::is_mod_enabled(d.mod_id) && !recomp::mods::is_mod_auto_enabled(d.mod_id)) continue;
        bool gameplay = d.tags.empty();
        for (const std::string& tag : d.tags) {
            if (cosmetic.count(tag) == 0) gameplay = true;
        }
        if (gameplay) names.push_back(d.display_name.empty() ? d.mod_id : d.display_name);
    }
    return names;
}

// Keep Area Progress and Permanent Tribal Saves let an area's challenge be split over several visits or loads, which
// makes the timed and "in one visit" Tribal achievements easier (Rover Rescue, They're All Boxed In, We Aren't Looking
// for Diamonds, Struck in the Muck, Strength of the Wielder). Like gameplay mods, they're warned about, not paused: most
// players keep them on.
std::string options_caution() {
    std::string out;
    const std::vector<std::string> mods = gameplay_mods_on();
    if (!mods.empty()) {
        out = mods.size() == 1 ? mods[0] + " changes gameplay"
                               : mods[0] + " and " + std::to_string(mods.size() - 1) + " more mods change gameplay";
        out += ": achievements still count, but their challenges may not play as designed.";
    }
    if (jfg::g.keep_area_progress || jfg::g.permanent_tribals) {
        const std::string which = jfg::g.keep_area_progress && jfg::g.permanent_tribals ? "Keep Area Progress and Permanent Tribal Saves are"
                                : jfg::g.keep_area_progress ? "Keep Area Progress is" : "Permanent Tribal Saves is";
        if (!out.empty()) out += " ";
        out += which + " on: some \"in one visit\" Tribal achievements are easier with it. Turn it off (Gameplay), or pick the Modern (Achievements) profile, to earn those as designed.";
    }
    return out;
}

// Once a second (game thread, and the launcher's while the game isn't running): the note the launcher shows.
void update_caution() {
    thread_local auto next_check = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now < next_check) return;
    next_check = now + std::chrono::seconds(1);
    const std::string caution = options_caution();
    std::lock_guard lock(g_status_mutex);
    if (g_status.caution == caution) return;
    g_status.caution = caution;
    g_status.serial++;
}
} // namespace

void jfg_achievements_init(const fs::path& app_folder, const std::string& version) {
    g_folder = app_folder;
    g_version = version;
    // JFG_RA_TEST_BANNER=1: a sample banner, to check the overlay's look without earning an achievement.
    if (const char* test = std::getenv("JFG_RA_TEST_BANNER"); test != nullptr && test[0] == '1') {
        banner("Achievement unlocked", "Particle Acceleration", "Sample banner (JFG_RA_TEST_BANNER): an achievement's description goes here.", 10);
        banner("Achievement unlocked", "Second in the queue", "Shown after the first one.", 5);
        show_challenge(1, "test", "", "Sample challenge (JFG_RA_TEST_BANNER)", fs::path("icons/64.png"));
    }
    // A login from an earlier run: its token. Without one nothing runs until the player logs in on the launcher.
    std::ifstream tokens(token_file());
    if (!tokens) return;
    nlohmann::json j = nlohmann::json::parse(tokens, nullptr, false);
    if (!j.is_object() || !j.contains("username") || !j.contains("token")) return;
    const std::string user = j["username"].get<std::string>(), token = j["token"].get<std::string>();
    ensure_client();
    set_status(JfgRaState::LoggingIn, "");
    rc_client_begin_login_with_token(g_client, user.c_str(), token.c_str(), login_done, nullptr);
}

void jfg_achievements_login(const std::string& user, const std::string& password) {
    if (user.empty() || password.empty()) {
        set_status(JfgRaState::Failed, "Enter your username and password.");
        return;
    }
    ensure_client();
    set_status(JfgRaState::LoggingIn, "");
    rc_client_begin_login_with_password(g_client, user.c_str(), password.c_str(), login_done, nullptr);
}

void jfg_achievements_logout() {
    if (g_client != nullptr) rc_client_logout(g_client);
    {
        std::lock_guard lock(g_challenge_mutex);
        g_challenges.clear();
    }
    std::error_code ec;
    fs::remove(token_file(), ec);
    g_game_requested = false;
    {
        std::lock_guard lock(g_status_mutex);
        const uint32_t serial = g_status.serial + 1;
        g_status = JfgRaStatus{};
        g_status.serial = serial;
    }
}

JfgRaStatus jfg_achievements_status() {
    std::lock_guard lock(g_status_mutex);
    return g_status;
}

void jfg_achievements_frame(uint8_t* rdram) {
    if (g_client == nullptr) return;
    g_rdram = rdram;
    run_replies();
    // The game: once logged in, identified from the ROM the launcher stored (jfg-us.z64 in the app folder).
    if (!g_game_requested && rc_client_get_user_info(g_client) != nullptr) {
        g_game_requested = true;
        static std::vector<uint8_t> rom;
        std::ifstream in(g_folder / "jfg-us.z64", std::ios::binary);
        rom.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (!rom.empty()) {
            rc_client_begin_identify_and_load_game(g_client, RC_CONSOLE_NINTENDO_64, nullptr, rom.data(), rom.size(),
                                                   game_loaded, nullptr);
        }
    }
    update_caution();
    // Emulators check the set once per video frame (60 a second), and its hit counts ("for 400 frames") are made for
    // that. A game tick covers 2-3 video frames at the original frame rate and 1 at 60 fps (delayDat, the frames the
    // game advances its own timers by), so the set is checked that many times; memory doesn't change in between, as
    // in an emulator while the game waits for its next frame.
    const uint32_t frames = std::clamp<uint32_t>(*reinterpret_cast<const uint32_t*>(rdram + DELAY_DAT_OFFSET), 1, 6);
    for (uint32_t i = 0; i < frames; i++) rc_client_do_frame(g_client);
}

void jfg_achievements_idle() {
    if (g_client == nullptr) return;
    run_replies();
    update_caution();
    rc_client_idle(g_client);
}

bool jfg_achievements_banner(JfgRaBanner& out, float& seconds) {
    std::lock_guard lock(g_banner_mutex);
    const auto now = std::chrono::steady_clock::now();
    while (!g_banners.empty()) {
        if (!g_banner_showing) {
            g_banner_showing = true;
            g_banner_shown = now;
        }
        seconds = std::chrono::duration<float>(now - g_banner_shown).count();
        if (seconds < JFG_RA_BANNER_SECONDS) {
            out = g_banners.front();
            return true;
        }
        g_banners.pop_front(); // done: the next one starts now
        g_banner_showing = false;
    }
    return false;
}

std::vector<JfgRaIndicator> jfg_achievements_indicators() {
    std::vector<JfgRaIndicator> out;
    std::lock_guard lock(g_challenge_mutex);
    for (const Challenge& c : g_challenges) {
        JfgRaIndicator indicator;
        indicator.title = c.title;
        if (auto it = g_badges.find(c.badge); it != g_badges.end()) {
            indicator.width = it->second.width;
            indicator.height = it->second.height;
            indicator.rgba = it->second.rgba;
        }
        out.push_back(std::move(indicator));
    }
    return out;
}

#else // no rcheevos: RetroAchievements is off

void jfg_achievements_init(const std::filesystem::path&, const std::string&) {}
void jfg_achievements_login(const std::string&, const std::string&) {}
void jfg_achievements_logout() {}
JfgRaStatus jfg_achievements_status() {
    JfgRaStatus status;
    status.state = JfgRaState::Failed;
    status.message = "This build has no RetroAchievements support.";
    return status;
}
void jfg_achievements_frame(uint8_t*) {}
void jfg_achievements_idle() {}
bool jfg_achievements_banner(JfgRaBanner&, float&) { return false; }
std::vector<JfgRaIndicator> jfg_achievements_indicators() { return {}; }

#endif
