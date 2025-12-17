// main.cpp
//
// SDL3 "Wheel of Names" with Twitch chat integration (Windows-only sample)
//
// Requirements:
//  - SDL3 and SDL3_ttf development libraries
//  - A TTF font file in the working directory (see FONT_PATH below)
//  - A Twitch account + chat OAuth token (see TWITCH_* constants)
//  - Windows toolchain (MSVC / MinGW) and linking to SDL3, SDL3_ttf, ws2_32

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
// NO winsock on the web
#else
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

// SDL includes: SDL3 everywhere (Windows + Web)
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <SDL3_image/SDL_image.h>

#include <fstream>
#include <sstream>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

// ----------------------
// Configuration
// ----------------------

// Path to a TTF font (relative to the executable, or absolute)
// Adjust this if needed.
static const char* FONT_PATH = "assets/fonts/WheelLabel.ttf";
static const char* LIST_FONT_PATH = "assets/fonts/ListLabel.otf";

static constexpr float NAME_PANEL_WIDTH_FRAC = 0.28f; // 28% of window width


static constexpr float WAKA_PIVOT_X = 0.57f;
static constexpr float WAKA_PIVOT_Y = 0.55f;

static constexpr float CONTENT_Y_OFFSET = 25.0f; // vertical offset for wheel + name list


// ----------------------
// Types
// ----------------------

struct WheelEntry {
    std::string name;
    SDL_Color   color;
};

struct AppState {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    TTF_Font* font = nullptr;
    TTF_Font* status_font = nullptr;  // smaller status font
    TTF_Font* list_font = nullptr;  // list font
    SDL_Texture* waka_texture = nullptr;
    int waka_w = 0;
    int waka_h = 0;

    std::vector<WheelEntry> entries;
    std::mutex              entries_mutex;                 

    float current_angle = 0.0f;      // radians
    float angular_velocity = 0.0f;   // radians/sec
    bool  spinning = false;
    int   winner_index = -1;

    // --- Timer state ---
    
    float bg_waka_offset_x = 0.0f;
    float bg_waka_offset_y = 0.0f;
    float bg_waka_angle_deg = 0.0f;

    float winner_flash_remaining = 0.0f; // seconds left in flash
    float winner_flash_elapsed = 0.0f; // total time since flash started
    bool celebration_active = false;
    float celebration_time = 0.0f;
    std::string celebration_name;
    SDL_Color celebration_color{ 255, 255, 255, 255 };
    float spin_friction = 3.0f;
    bool  reset_hold_active = false;  // true while user is holding to reset
    float reset_hold_elapsed = 0.0f;  // seconds since hold began
    int   reset_hold_source = 0;      // 0 = none, 1 = space, 2 = mouse
    bool  authorized = false;
    std::atomic<bool> join_open{ false };
    TTF_Font* timer_font = nullptr;
    TimerState timer;
};

AppState app;
struct TwitchConfig {
    std::string oauth;
    std::string nick;
    std::string channel;
};

TwitchConfig g_twitch_cfg;

static void render_text_bottom_right(SDL_Renderer* renderer,
    TTF_Font* font,
    const std::string& text,
    SDL_Color color,
    float x,
    float y)
{
    if (!renderer || !font) return;
    if (text.empty()) return;

    SDL_Surface* surface = TTF_RenderText_Blended(font, text.c_str(), 0, color);
    if (!surface) {
        std::cerr << "TTF_RenderText_Blended (bottom-right) failed: "
                  << SDL_GetError() << "\n";
        return;
    }

    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (!texture) {
        std::cerr << "SDL_CreateTextureFromSurface (bottom-right) failed: "
                  << SDL_GetError() << "\n";
        SDL_DestroySurface(surface);
        return;
    }

    SDL_FRect dst{};
    dst.w = static_cast<float>(surface->w);
    dst.h = static_cast<float>(surface->h);

    // (x, y) is the bottom-right corner
    dst.x = x - dst.w;
    dst.y = y - dst.h;

    SDL_DestroySurface(surface);

    SDL_RenderTexture(renderer, texture, nullptr, &dst);
    SDL_DestroyTexture(texture);
}


static void trim_in_place(std::string& s) {
    size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        s.clear();
        return;
    }
    size_t last = s.find_last_not_of(" \t\r\n");
    s = s.substr(first, last - first + 1);
}

static std::string to_lower(const std::string& s);

static bool load_twitch_config(const char* path, TwitchConfig& out) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "[Twitch] Could not open config file: " << path << "\n";
        return false;
    }

    std::string line;
    while (std::getline(in, line)) {
        trim_in_place(line);
        if (line.empty() || line[0] == '#')
            continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        trim_in_place(key);
        trim_in_place(value);

        std::string lkey = to_lower(key);
        if (lkey == "oauth") {
            out.oauth = value;
        }
        else if (lkey == "nick") {
            out.nick = value;
        }
        else if (lkey == "channel") {
            out.channel = value;
        }
    }

    return true;
}


// ----------------------
// Utility: RNG, colors
// ----------------------

static void draw_filled_triangle(SDL_Renderer* renderer,
    SDL_FPoint a, SDL_FPoint b, SDL_FPoint c, SDL_Color color);

static void draw_spin_droplet(SDL_Renderer* renderer,
    float cx, float cy, float r, SDL_Color color);


static std::mt19937& global_rng() {
    static std::mt19937 rng{ std::random_device{}() };
    return rng;
}

// Replace the existing random_color() with this version
static SDL_Color random_color() {
    static const SDL_Color palette[] = {
        { 0,   100, 0,   255 },  // dark green
        { 100, 170, 120, 255 },  // light green
        { 255, 165, 0,   255 },  // orange
        { 240, 210, 60,  255  }
    };

    static std::atomic<uint32_t> idx{ 0 };
    uint32_t i = idx.fetch_add(1, std::memory_order_relaxed);
    return palette[i % (sizeof(palette) / sizeof(palette[0]))];
}



static uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

static float stable_tile_scale(int gx, int gy) {
    uint32_t h = hash_u32((uint32_t)gx * 73856093u ^ (uint32_t)gy * 19349663u);
    float t = (h & 0xFFFFu) / 65535.0f;   // 0..1
    return 0.5f + t * 0.3f;              // 0.5..0.8
}

static float clamp01(float t) {
    return std::max(0.0f, std::min(1.0f, t));
}

static float ease_out_cubic(float t) {
    t = clamp01(t);
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

// Forward declarations for banner helpers
static void draw_filled_rounded_rect(SDL_Renderer* renderer,
    float x, float y, float w, float h,
    float radius, SDL_Color color);

static void render_text_centered(SDL_Renderer* renderer,
    TTF_Font* font,
    const std::string& text,
    SDL_Color color,
    float x, float y);

static void render_text_left(SDL_Renderer* renderer,
                             TTF_Font* font,
                             const std::string& text,
                             SDL_Color color,
                             float x,
                             float y)
{
    if (!renderer || !font) return;
    if (text.empty()) return;

    SDL_Surface* surface = TTF_RenderText_Blended(font, text.c_str(), 0, color);
    if (!surface) {
        std::cerr << "TTF_RenderText_Blended (left) failed: "
                  << SDL_GetError() << "\n";
        return;
    }

    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (!texture) {
        std::cerr << "SDL_CreateTextureFromSurface (left) failed: "
                  << SDL_GetError() << "\n";
        SDL_DestroySurface(surface);
        return;
    }

    SDL_FRect dst{};
    dst.w = static_cast<float>(surface->w);
    dst.h = static_cast<float>(surface->h);
    dst.x = x;
    dst.y = y;

    SDL_DestroySurface(surface);

    SDL_RenderTexture(renderer, texture, nullptr, &dst);
    SDL_DestroyTexture(texture);
}

static void draw_winner_banner(SDL_Renderer* renderer,
    TTF_Font* font,
    const std::string& name,
    SDL_Color barColor,  // kept for signature compatibility
    float t)
{
    (void)barColor; // we now choose our own colors

    if (!renderer || !font || name.empty()) return;

    int winW = 0, winH = 0;
    SDL_GetRenderOutputSize(renderer, &winW, &winH);

    const float barH = 90.0f;

    // Wheel radius in draw_wheel() is 0.45 * min(winW, winH),
    // so the diameter is 0.9 * min(winW, winH).
    float minWH = static_cast<float>(std::min(winW, winH));
    const float wheelDiameter = minWH * 0.9f;

    // Make the banner just a bit wider than the wheel.
    const float barW = wheelDiameter * 1.05f;

    const float barY = winH * 0.55f;


    // Slide-in duration
    const float slideInDur = 0.35f;

    // Clamp time for sliding; once in place, stay put
    float tClamped = (slideInDur > 0.0f)
        ? std::min(t, slideInDur)
        : 0.0f;

    float p = ease_out_cubic(tClamped / slideInDur);

    float startX = winW + barW * 0.6f;
    float endX   = winW * 0.5f;
    float barCenterX = startX + (endX - startX) * p;

    // Green fill with black border
    SDL_Color border{ 0,   0,   0,   255 };
    SDL_Color fill  { 34, 132,  60,  255 }; // adjust to taste

    float radiusOuter = 14.0f;
    float radiusInner = 13.0f;

    float x = barCenterX - barW * 0.5f;
    float y = barY - barH * 0.5f;

    // Outer border and inner fill
    draw_filled_rounded_rect(renderer, x, y,
        barW, barH,
        radiusOuter, border);
    draw_filled_rounded_rect(renderer, x + 2.0f, y + 2.0f,
        barW - 4.0f, barH - 4.0f,
        radiusInner, fill);

    // --- Waka sprite on the left edge of the banner ---
    // --- Waka sprite on the left edge of the banner ---
if (app.waka_texture && app.waka_w > 0 && app.waka_h > 0) {
    // Use only the upper part of the texture (eyes + top)
    SDL_FRect src{};
    src.x = 0.0f;
    src.y = 0.0f;
    src.w = static_cast<float>(app.waka_w);
    src.h = static_cast<float>(app.waka_h) * 0.6f;

    // Inner rectangle of the banner (inside the 2-px black border)
    const float borderThickness = 2.0f;
    float innerX = x + borderThickness;
    float innerY = y + borderThickness;
    float innerH = barH - borderThickness * 2.0f;

    // Scale the cropped sprite to exactly fill the inner height
    float scale = innerH / src.h;

    SDL_FRect dst{};
    dst.w = src.w * scale;
    dst.h = innerH;

    // Left edge flush with the inner black border, top and bottom
    // flush with the top/bottom inner borders
    dst.x = innerX;
    dst.y = innerY;

    SDL_RenderTexture(renderer, app.waka_texture, &src, &dst);
}


    // Letters still appear one at a time, but no vertical "shake"
    const float letterStart    = 0.35f;
    const float letterInterval = 0.07f;

    int lettersShown = 0;
    if (t >= letterStart) {
        float lt = t - letterStart;
        lettersShown =
            std::min<int>((int)(lt / letterInterval) + 1,
                          (int)name.size());
    }

    if (lettersShown <= 0) {
        return;
    }

    std::string sub = name.substr(0, lettersShown);

    // Name stays centered on the banner
    render_text_centered(renderer,
        font,
        sub,
        SDL_Color{ 255, 255, 255, 255 }, // white text
        barCenterX,
        barY);
}


static std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

static bool is_stream_allowed(const std::string& nick, const std::string& channel)
{
    // All names here should be lowercase. Add/remove entries as needed.
    static const char* ALLOWED[] = {
        "cbf01",       // example allowed channel/host
        "shoepert",
        
    };

    std::string n  = to_lower(nick);

    std::string ch = channel;
    if (!ch.empty() && ch[0] == '#') {
        ch.erase(0, 1); // strip leading '#'
    }
    ch = to_lower(ch);

    for (const char* allowed : ALLOWED) {
        if (n == allowed || ch == allowed) {
            return true;
        }
    }
    return false;
}
static void add_player_if_new(const std::string& name,
    std::vector<WheelEntry>& entries,
    std::mutex& entries_mutex) {
    if (name.empty()) return;
    std::lock_guard<std::mutex> lock(entries_mutex);
    auto it = std::find_if(entries.begin(), entries.end(),
        [&](const WheelEntry& w) { return w.name == name; });
    if (it == entries.end()) {
        WheelEntry e;
        e.name = name;
        e.color = random_color();
        entries.push_back(e);
        std::cout << "[Twitch] Added player: " << name << std::endl;
    }
}


// ----------------------
// Twitch IRC helpers
// ----------------------
#ifndef __EMSCRIPTEN__
static bool send_line(SOCKET sock, const std::string& line) {
    std::string data = line + "\r\n";
    size_t total = 0;
    while (total < data.size()) {
        int sent = send(sock, data.c_str() + total,
            static_cast<int>(data.size() - total), 0);
        if (sent == SOCKET_ERROR) {
            return false;
        }
        total += static_cast<size_t>(sent);
    }
    return true;
}



static std::string parse_username_from_irc_line(const std::string& line) {
    // Handles possible IRCv3 tags that start with '@'
    size_t idx = 0;
    if (!line.empty() && line[0] == '@') {
        size_t space = line.find(' ');
        if (space == std::string::npos) {
            return {};
        }
        idx = space + 1;
    }
    if (idx < line.size() && line[idx] == ':') {
        size_t prefix_start = idx + 1;
        size_t ex = line.find('!', prefix_start);
        if (ex != std::string::npos) {
            return line.substr(prefix_start, ex - prefix_start);
        }
    }
    return {};
}

// Handle one IRC line from Twitch
static void handle_irc_line(const std::string& line,
    SOCKET sock,
    std::vector<WheelEntry>& entries,
    std::mutex& entries_mutex) {
    if (line.empty()) return;

    // Respond to PING to stay connected
    if (line.rfind("PING", 0) == 0) {
        size_t colon = line.find(':');
        std::string payload = (colon != std::string::npos)
            ? line.substr(colon + 1)
            : "tmi.twitch.tv";
        send_line(sock, "PONG :" + payload);
        return;
    }

    // We only care about PRIVMSG lines
    size_t priv = line.find("PRIVMSG");
    if (priv == std::string::npos) {
        return;
    }

    std::string username = parse_username_from_irc_line(line);

    // Grab the message text after "PRIVMSG <channel> :"
    size_t colonAfterPriv = line.find(" :", priv);
    if (colonAfterPriv == std::string::npos) {
        return;
    }
    std::string message = line.substr(colonAfterPriv + 2);
    std::string lower = to_lower(message);

    if (lower.rfind("!join", 0) == 0) {
        if (app.join_open.load()) {
            add_player_if_new(username, entries, entries_mutex);
        } else {
            std::cout << "[Twitch] Ignoring !join from " << username
                      << " (wheel closed)\n";
        }
    }
}


// connect to Twitch IRC and watch for "!join"
static void twitch_chat_thread(TwitchConfig cfg,
    std::atomic<bool>& running,
    std::vector<WheelEntry>& entries,
    std::mutex& entries_mutex) {
    if (cfg.oauth.empty() || cfg.nick.empty() || cfg.channel.empty()) {
        std::cerr << "[Twitch] Config not set; skipping chat integration.\n";
        return;
    }

    WSADATA wsaData{};
    int wsaErr = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaErr != 0) {
        std::cerr << "[Twitch] WSAStartup failed with error: "
            << wsaErr << "\n";
        return;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    int res = getaddrinfo("irc.chat.twitch.tv", "6667", &hints, &result);
    if (res != 0 || !result) {
        std::cerr << "[Twitch] getaddrinfo failed.\n";
        WSACleanup();
        return;
    }

    SOCKET sock = INVALID_SOCKET;
    for (addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
        sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (sock == INVALID_SOCKET) continue;
        if (connect(sock, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen)) == SOCKET_ERROR) {
            closesocket(sock);
            sock = INVALID_SOCKET;
            continue;
        }
        break;
    }
    freeaddrinfo(result);

    if (sock == INVALID_SOCKET) {
        std::cerr << "[Twitch] Could not connect to IRC.\n";
        WSACleanup();
        return;
    }

    std::cout << "[Twitch] Connected, logging in...\n";

    // cfg.oauth should already include "oauth:" prefix if you set TWITCH_OAUTH that way.
    if (!send_line(sock, "PASS " + cfg.oauth) ||
        !send_line(sock, "NICK " + cfg.nick) ||
        !send_line(sock, "JOIN " + cfg.channel)) {
        std::cerr << "[Twitch] Failed to send login messages.\n";
        closesocket(sock);
        WSACleanup();
        return;
    }

    // Make recv() time out periodically so we can notice 'running == false'
    DWORD timeoutMs = 200; // 0.2 seconds
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeoutMs),
        sizeof(timeoutMs)) == SOCKET_ERROR) {
        std::cerr << "[Twitch] setsockopt(SO_RCVTIMEO) failed: "
            << WSAGetLastError() << "\n";
        // Not fatal; we can continue, but shutdown may be slower
    }

    char buffer[1024];
    std::string recvBuffer;

    while (running.load()) {
        int bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);

        if (bytes == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
                // Normal: timeout so we can re-check 'running'
                continue;
            }
            std::cerr << "[Twitch] recv failed with error: " << err << "\n";
            break;
        }

        if (bytes == 0) {
            std::cerr << "[Twitch] Disconnected.\n";
            break;
        }

        buffer[bytes] = '\0';
        recvBuffer.append(buffer);

        size_t pos;
        while ((pos = recvBuffer.find("\r\n")) != std::string::npos) {
            std::string line = recvBuffer.substr(0, pos);
            recvBuffer.erase(0, pos + 2);

            std::cout << "[Twitch RAW] " << line << "\n";

            handle_irc_line(line, sock, entries, entries_mutex);

        }
    }

    closesocket(sock);
    WSACleanup();
    std::cout << "[Twitch] Thread exiting.\n";
}
#endif
static void handle_spin_or_reset(AppState& app, const TwitchConfig& cfg)
{
    bool didReset = false;

    {
        std::lock_guard<std::mutex> lock(app.entries_mutex);

        // If a winner is currently being shown, treat this as "reset for next round"
        if (app.celebration_active || app.winner_index >= 0) {
            // Hide winner banner
            app.celebration_active = false;
            app.celebration_time = 0.0f;
            app.celebration_name.clear();

            // Stop any flashing and clear winner
            app.winner_flash_remaining = 0.0f;
            app.winner_flash_elapsed = 0.0f;
            app.winner_index = -1;

            // Make sure wheel physics are idle
            app.spinning = false;
            app.angular_velocity = 0.0f;

            // Clear all players from the wheel so chat can !join again
            app.entries.clear();

            // Return to CLOSED state
            app.join_open.store(false);


            didReset = true;
            std::cout << "[Wheel] Reset for next round.\n";
        }
        // Otherwise, if not spinning and we have players, start a new spin
        else if (app.entries.size() >= 2 && !app.spinning && !app.join_open.load()) {
            app.spinning = true;
            app.winner_index = -1;
            app.winner_flash_remaining = 0.0f;
            app.winner_flash_elapsed = 0.0f;

            std::uniform_real_distribution<float> spinSpeed(10.0f, 13.0f);
            std::uniform_real_distribution<float> spinFriction(1.8f, 5.6f);

            app.spin_friction = spinFriction(global_rng());
            app.angular_velocity = spinSpeed(global_rng());

            std::cout << "[Wheel] Spin started.\n";
        }
    } // mutex unlocked here

    // After reset, automatically add the channel owner as if they had joined
    if (didReset && !cfg.nick.empty()) {
        add_player_if_new(cfg.nick, app.entries, app.entries_mutex);
    }
}


static void set_join_open(AppState& a, bool open)
{
    bool wasOpen = a.join_open.load(std::memory_order_relaxed);
    if (wasOpen == open) return;

    a.join_open.store(open, std::memory_order_relaxed);

    if (open) {
        timer_reset(a, TIMER_START_SECONDS); // 60
        timer_start(a);
    }
    else {
        timer_stop(a);
    }
}


// ----------------------
// SDL text rendering
// ----------------------

static void render_text_centered(SDL_Renderer* renderer,
    TTF_Font* font,
    const std::string& text,
    SDL_Color color,
    float x,
    float y) {
    if (!renderer || !font) return;
    if (text.empty()) return;

    SDL_Surface* surface = TTF_RenderText_Blended(font, text.c_str(), 0, color);
    if (!surface) {
        std::cerr << "TTF_RenderText_Blended failed: " << SDL_GetError() << "\n";
        return;
    }

    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (!texture) {
        std::cerr << "SDL_CreateTextureFromSurface failed: " << SDL_GetError() << "\n";
        SDL_DestroySurface(surface);
        return;
    }

    SDL_FRect dst{};
    dst.w = static_cast<float>(surface->w);
    dst.h = static_cast<float>(surface->h);
    dst.x = x - dst.w / 2.0f;
    dst.y = y - dst.h / 2.0f;

    SDL_DestroySurface(surface);

    SDL_RenderTexture(renderer, texture, nullptr, &dst);
    SDL_DestroyTexture(texture);
}

// Radial, rotated, and width-scaled label rendering
static void render_text_radial(SDL_Renderer* renderer,
    TTF_Font* font,
    const std::string& text,
    SDL_Color color,
    float cx,
    float cy,
    float radius,
    float angle,
    float maxTangentSpan,
    float maxRadialSpan)

{
    if (!renderer || !font) return;
    if (text.empty()) return;

    const float RAD_TO_DEG = 180.0f / 3.14159265358979323846f;

    SDL_Surface* surface = TTF_RenderText_Blended(font, text.c_str(), 0, color);
    if (!surface) {
        std::cerr << "TTF_RenderText_Blended (radial) failed: "
            << SDL_GetError() << "\n";
        return;
    }

    float srcW = static_cast<float>(surface->w);
    float srcH = static_cast<float>(surface->h);

    // Start at full size; only scale down if necessary
    float scaleAcross = 1.0f;
    if (maxTangentSpan > 0.0f && srcH > maxTangentSpan) {
        scaleAcross = maxTangentSpan / srcH;
    }

    float scaleRadial = 1.0f;
    if (maxRadialSpan > 0.0f && srcW > maxRadialSpan) {
        scaleRadial = maxRadialSpan / srcW;
    }

    float scale = std::min(scaleAcross, scaleRadial);

    // Small padding so it doesn't feel tight against boundaries.
    scale *= 0.95f;


    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (!texture) {
        std::cerr << "SDL_CreateTextureFromSurface (radial) failed: "
            << SDL_GetError() << "\n";
        SDL_DestroySurface(surface);
        return;
    }

    SDL_FRect dst{};
    dst.w = srcW * scale;
    dst.h = srcH * scale;
    SDL_DestroySurface(surface); // texture now owns the pixels

    float dx = std::cos(angle);
    float dy = std::sin(angle);

    float tx = cx + radius * dx; // text center x
    float ty = cy + radius * dy; // text center y

    dst.x = tx - dst.w / 2.0f;
    dst.y = ty - dst.h / 2.0f;

    SDL_FPoint pivot;
    pivot.x = dst.w / 2.0f;
    pivot.y = dst.h / 2.0f;

    // Rotate with the slice so names are "painted" on the wheel
    double rotationDeg = angle * RAD_TO_DEG + 180.0;

    SDL_RenderTextureRotated(renderer,
        texture,
        nullptr,
        &dst,
        rotationDeg,
        &pivot,
        SDL_FLIP_NONE);

    SDL_DestroyTexture(texture);
}

// ----------------------
// Wheel rendering helpers
// ----------------------

void SetIcon(SDL_Window* win)
{
    SDL_Surface* icon = IMG_Load("assets/icon.png");  // PNG is typical
    if (icon) {
        SDL_SetWindowIcon(win, icon);
        SDL_DestroySurface(icon);
    }
}

static void draw_filled_sector(SDL_Renderer* renderer,
    float cx, float cy,
    float radius,
    float startAngle,
    float endAngle,
    SDL_Color color) {
    if (endAngle <= startAngle) return;

    const int segments = 32;
    float delta = (endAngle - startAngle) / segments;

    SDL_FColor fcolor;
    fcolor.r = color.r / 255.0f;
    fcolor.g = color.g / 255.0f;
    fcolor.b = color.b / 255.0f;
    fcolor.a = color.a / 255.0f;

    for (int i = 0; i < segments; ++i) {
        float a0 = startAngle + delta * i;
        float a1 = startAngle + delta * (i + 1);

        SDL_Vertex verts[3];
        verts[0].position = SDL_FPoint{ cx, cy };
        verts[0].color = fcolor;
        verts[0].tex_coord = SDL_FPoint{ 0.0f, 0.0f };

        verts[1].position = SDL_FPoint{ cx + radius * std::cos(a0),
                                         cy + radius * std::sin(a0) };
        verts[1].color = fcolor;
        verts[1].tex_coord = SDL_FPoint{ 0.0f, 0.0f };

        verts[2].position = SDL_FPoint{ cx + radius * std::cos(a1),
                                         cy + radius * std::sin(a1) };
        verts[2].color = fcolor;
        verts[2].tex_coord = SDL_FPoint{ 0.0f, 0.0f };

        SDL_RenderGeometry(renderer, nullptr, verts, 3, nullptr, 0);
    }
}

static void draw_circle_outline(SDL_Renderer* renderer,
    float cx, float cy, float radius,
    SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    const int segments = 96;
    float prevX = cx + radius;
    float prevY = cy;

    for (int i = 1; i <= segments; ++i) {
        float angle = (2.0f * 3.14159265358979323846f * i) / segments;
        float x = cx + radius * std::cos(angle);
        float y = cy + radius * std::sin(angle);
        SDL_RenderLine(renderer, prevX, prevY, x, y);
        prevX = x;
        prevY = y;
    }
}


// -----------------------
// Timer rendering helpers
// -----------------------

// Rounded rectangle fill using our sector helper for the corners
static void draw_filled_rounded_rect(SDL_Renderer* renderer,
    float x, float y,
    float w, float h,
    float radius,
    SDL_Color color)
{
    if (!renderer) return;

    if (radius < 0.0f) radius = 0.0f;
    if (radius > w * 0.5f) radius = w * 0.5f;
    if (radius > h * 0.5f) radius = h * 0.5f;

    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);

    // Center and side rectangles
    SDL_FRect centerRect{ x + radius, y, w - 2.0f * radius, h };
    SDL_RenderFillRect(renderer, &centerRect);

    SDL_FRect leftRect{ x, y + radius, radius, h - 2.0f * radius };
    SDL_RenderFillRect(renderer, &leftRect);

    SDL_FRect rightRect{ x + w - radius, y + radius, radius, h - 2.0f * radius };
    SDL_RenderFillRect(renderer, &rightRect);

    const float PI = 3.14159265358979323846f;

    // Four quarter-circle corners using the existing draw_filled_sector helper
    // top-left
    draw_filled_sector(renderer,
        x + radius, y + radius,
        radius,
        PI, PI * 1.5f,
        color);
    // top-right
    draw_filled_sector(renderer,
        x + w - radius, y + radius,
        radius,
        PI * 1.5f, PI * 2.0f,
        color);
    // bottom-right
    draw_filled_sector(renderer,
        x + w - radius, y + h - radius,
        radius,
        0.0f, PI * 0.5f,
        color);
    // bottom-left
    draw_filled_sector(renderer,
        x + radius, y + h - radius,
        radius,
        PI * 0.5f, PI,
        color);
}

// Draw a single 7-segment digit in a rect [x,y,w,h]
// Draw a single 7-segment digit in a rect [x,y,w,h]
// Draw a single 7-segment digit in a rect [x,y,w,h]


static int pointer_slice_index(float current_angle, int n)
{
    if (n <= 0) return -1;

    const float PI = 3.14159265358979323846f;
    const float TWO_PI = 2.0f * PI;
    const float POINTER_ANGLE = -PI * 0.5f; // 12 o'clock

    float sliceAngle = TWO_PI / n;

    // Angle of the pointer in wheel-space
    float a = std::fmod(POINTER_ANGLE - current_angle, TWO_PI);
    if (a < 0.0f) a += TWO_PI;

    int index = static_cast<int>(a / sliceAngle);
    if (index >= n) index = n - 1;
    return index;
}



static SDL_Color readable_text_color(SDL_Color bg) {
    // Relative luminance-ish
    int lum = (int)(0.2126f * bg.r + 0.7152f * bg.g + 0.0722f * bg.b);
    return (lum < 120) ? SDL_Color{ 255,255,255,255 }
    : SDL_Color{ 0,0,0,255 };
}


static void draw_wheel(SDL_Renderer* renderer,
    TTF_Font* font,
    const std::vector<WheelEntry>& entries,
    float current_angle,
    int winner_index,
    bool spinning,
    bool winner_flash_on,
    SDL_Texture* waka_texture,
    int waka_w,
    int waka_h)
{
    int w = 0, h = 0;
    SDL_GetRenderOutputSize(renderer, &w, &h);

float listWidth   = static_cast<float>(w) * NAME_PANEL_WIDTH_FRAC;
float wheelAreaW  = static_cast<float>(w) - listWidth;
if (wheelAreaW < 0.0f) {
    wheelAreaW = static_cast<float>(w); // fallback
}

// Wheel is centered in the left area
float cx = wheelAreaW * 0.5f;
float cy = static_cast<float>(h) * 0.5f + CONTENT_Y_OFFSET;
float radius = std::min(wheelAreaW, static_cast<float>(h)) * 0.45f;

    int n = static_cast<int>(entries.size());
    const float TWO_PI = 2.0f * 3.14159265358979323846f;

    // --- Drop shadow to lower-left (always visible) ---
    const float shadow_dx = -radius * 0.19f;
    const float shadow_dy = radius * 0.05f;
    const float shadow_r = radius * 1.02f;
    SDL_Color shadowCol{ 0, 0, 0, 70 };

    draw_filled_sector(renderer,
        cx + shadow_dx,
        cy + shadow_dy,
        shadow_r,
        0.0f,
        TWO_PI,
        shadowCol);

    // --- Colored slices and labels (only if we have entries) ---
    float sliceAngle = 0.0f;
    int   active_index = -1;

    if (n > 0) {
        sliceAngle = TWO_PI / n;
        active_index = spinning ? pointer_slice_index(current_angle, n) : -1;

        for (int i = 0; i < n; ++i) {
            float start = current_angle + i * sliceAngle;
            float end = start + sliceAngle;

            SDL_Color col = entries[i].color;

            bool isActive = spinning && (i == active_index);
            bool isWinner = (!spinning) && (i == winner_index);
            bool isFlash = isWinner && winner_flash_on;

            if (isActive || isFlash) {
                col.r = static_cast<Uint8>(std::min<int>(255, col.r + 70));
                col.g = static_cast<Uint8>(std::min<int>(255, col.g + 70));
                col.b = static_cast<Uint8>(std::min<int>(255, col.b + 70));
            }
            else if (isWinner) {
                col.r = static_cast<Uint8>(std::min<int>(255, col.r + 40));
                col.g = static_cast<Uint8>(std::min<int>(255, col.g + 40));
                col.b = static_cast<Uint8>(std::min<int>(255, col.b + 40));
            }

            draw_filled_sector(renderer, cx, cy, radius, start, end, col);

            if (isActive || isFlash) {
                SDL_Color glow{ 255, 255, 255, 80 };
                draw_filled_sector(renderer, cx, cy, radius, start, end, glow);
            }
        }
    }

    // Rim is always visible, even with zero entries
    draw_circle_outline(renderer, cx, cy, radius, SDL_Color{ 40, 40, 40, 255 });

    // --- Text labels (only if we have entries) ---
    // --- Text labels (only if we have entries) ---
    if (n > 0 && font) {
        // Radius where labels sit
        float outerTextRadius = radius * 0.72f;

        // Tangential span: chord length at that radius for one slice
        float maxTangentSpan =
            2.0f * outerTextRadius * std::sin(sliceAngle * 0.5f);

        // Radial span: how much room we have between rim and hub
        float capR = radius * 0.18f;
        float inward = outerTextRadius - capR * 1.1f;
        float outward = radius - outerTextRadius;
        float maxRadialSpan =
            2.0f * std::max(0.0f, std::min(inward, outward));

        for (int i = 0; i < n; ++i) {
            float midAngle = current_angle + (i + 0.5f) * sliceAngle;

            SDL_Color sliceCol = entries[i].color;
            SDL_Color textColor = readable_text_color(sliceCol);

            if (spinning && i == active_index) {
                textColor = SDL_Color{ 0, 0, 0, 255 };
            }
            else if (!spinning && i == winner_index) {
                textColor = SDL_Color{ 0, 0, 0, 255 };
            }

            // Draw label rotated so it "paints" onto the slice
            render_text_radial(renderer,
                font,
                entries[i].name,
                textColor,
                cx,
                cy,
                outerTextRadius,
                midAngle,
                maxTangentSpan,
                maxRadialSpan);
        }
    }


    // --- Center cap & Waka sprite (always drawn) ---
    float capR = radius * 0.18f;

    draw_filled_sector(renderer, cx, cy, capR, 0.0f, TWO_PI,
        SDL_Color{ 5, 40, 10, 255 });
    draw_circle_outline(renderer, cx, cy, capR,
        SDL_Color{ 0, 0, 0, 255 });

    if (waka_texture && waka_w > 0 && waka_h > 0) {
        float target = capR * 1.5f;
        float scale = target / (float)std::max(waka_w, waka_h);

        SDL_FRect dst{};
        dst.w = waka_w * scale;
        dst.h = waka_h * scale;

        SDL_FPoint pivot;
        pivot.x = dst.w * WAKA_PIVOT_X;
        pivot.y = dst.h * WAKA_PIVOT_Y;

        dst.x = cx - pivot.x;
        dst.y = cy - pivot.y;

        const float RAD_TO_DEG =
            180.0f / 3.14159265358979323846f;

        double rotationDeg = current_angle * RAD_TO_DEG;

        SDL_RenderTextureRotated(renderer,
            waka_texture,
            nullptr,
            &dst,
            rotationDeg,
            &pivot,
            SDL_FLIP_NONE);
    }
    // --- Pointer at 12 o'clock, attached to the hub ---
    
    {
        float pointerOuterR = radius * .25f;   // just outside the rim
        float pointerWidth = capR * 0.4f;      // width of the base on the droplet top

        SDL_FPoint tip{ cx, cy - pointerOuterR };
        SDL_FPoint baseL{ cx - pointerWidth * 0.5f, cy - capR + 0.59f };
        SDL_FPoint baseR{ cx + pointerWidth * 0.5f, cy - capR + 0.59f };

        draw_filled_triangle(renderer,
            tip,
            baseL,
            baseR,
            SDL_Color{ 0, 0, 0, 255 }); // black pointer
    }
}

static void draw_name_list(SDL_Renderer* renderer,
                           TTF_Font* font,
                           const std::vector<WheelEntry>& entries,
                           int active_index,
                           int winner_index)
{
    if (!renderer || !font) return;

    int winW = 0, winH = 0;
    SDL_GetRenderOutputSize(renderer, &winW, &winH);

    float winWF = static_cast<float>(winW);
    float winHF = static_cast<float>(winH);

    // Panel on the right side of the window, inset ~10px from the edge
    float panelWidth       = winWF * NAME_PANEL_WIDTH_FRAC;
    float panelMarginRight = 10.0f;               // gap from right edge
    float panelX           = winWF - panelWidth - panelMarginRight;

    // Panel is ~60% of the window height and vertically centered
    float panelHeightFrac  = 0.6f;                // 60% of window height
    float panelH           = winHF * panelHeightFrac;
    float panelY           = (winHF - panelH) * 0.5f + CONTENT_Y_OFFSET;

    SDL_FRect panelRect{ panelX, panelY, panelWidth, panelH };

    // Fill panel with white
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderFillRect(renderer, &panelRect);

    // Thin black border
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderRect(renderer, &panelRect);

    // Text layout inside the panel
    float paddingX = 10.0f;
    float paddingY = 10.0f;

    float contentX = panelX + paddingX;
    float contentY = panelY + paddingY;
    float contentW = panelWidth - 2.0f * paddingX;
    float contentH = panelH   - 2.0f * paddingY;

    int   lineSkip = TTF_GetFontLineSkip(font);
    float lineStep = static_cast<float>(lineSkip);

    int maxRows = static_cast<int>(contentH / lineStep);
    if (maxRows <= 0) return;

    // Two columns: total visible slots
    const int numColumns = 2;
    int maxVisible = maxRows * numColumns;
    if (maxVisible <= 0) return;

    int total = static_cast<int>(entries.size());
    int startIndex = 0;

    // If there are more names than fit, show the most recent ones
    if (total > maxVisible) {
        startIndex = total - maxVisible;
    }

    // Horizontal layout for two columns
    float columnGap = 20.0f; // space between columns
    float colWidth  = (contentW - columnGap) * 0.5f;

    float colX[2];
    colX[0] = contentX;
    colX[1] = contentX + colWidth + columnGap;

    SDL_Color textCol{ 0, 0, 0, 255 };

    // Draw up to maxVisible entries across two columns
    for (int idx = 0; idx < maxVisible && (startIndex + idx) < total; ++idx) {
        int i = startIndex + idx;

        int col = idx / maxRows;      // 0 or 1
        int row = idx % maxRows;      // 0 .. maxRows-1

        if (col >= numColumns)
            break;

        float x = colX[col];
        float y = contentY + row * lineStep;

        const auto& e = entries[i];
        render_text_left(renderer, font, e.name, textCol, x, y);
    }
}


static void draw_filled_triangle(SDL_Renderer* renderer,
    SDL_FPoint a, SDL_FPoint b, SDL_FPoint c, SDL_Color color)
{
    SDL_FColor fc{ color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f };
    SDL_Vertex v[3];
    v[0].position = a; v[0].color = fc; v[0].tex_coord = { 0,0 };
    v[1].position = b; v[1].color = fc; v[1].tex_coord = { 0,0 };
    v[2].position = c; v[2].color = fc; v[2].tex_coord = { 0,0 };
    SDL_RenderGeometry(renderer, nullptr, v, 3, nullptr, 0);
}

static void draw_spin_droplet(SDL_Renderer* renderer,
    float cx, float cy, float r, SDL_Color color)
{
    const float TWO_PI = 2.0f * 3.14159265358979323846f;

    // Round body
    draw_filled_sector(renderer, cx, cy, r, 0.0f, TWO_PI, color);

    // Pointed top
    SDL_FPoint tip{ cx, cy - r * 1.35f };
    SDL_FPoint left{ cx - r * 0.85f, cy - r * 0.10f };
    SDL_FPoint right{ cx + r * 0.85f, cy - r * 0.10f };
    draw_filled_triangle(renderer, tip, left, right, color);
}


static void draw_background(SDL_Renderer* renderer,
    SDL_Texture* waka_texture,
    int waka_w,
    int waka_h,
    float offset_x,
    float offset_y,
    float angle_deg)

{
    if (!renderer) return;

    int winW = 0, winH = 0;
    SDL_GetRenderOutputSize(renderer, &winW, &winH);

    // --- Pixelated vertical green gradient with dithering ---

    const int cell = 4; // size of each "pixel block"
    SDL_FRect r{ 0, 0, (float)cell, (float)cell };

    // top and bottom gradient colors (approximate the example)
    const Uint8 rTop = 16, gTop = 88, bTop = 32;
    const Uint8 rBot = 40, gBot = 160, bBot = 72;

    for (int y = 0; y < winH; y += cell) {
        float t = (float)y / (float)(winH - 1);
        Uint8 baseR = (Uint8)(rTop + t * (rBot - rTop));
        Uint8 baseG = (Uint8)(gTop + t * (gBot - gTop));
        Uint8 baseB = (Uint8)(bTop + t * (bBot - bTop));

        for (int x = 0; x < winW; x += cell) {
            // simple checkerboard dither
            int checker = ((x / cell) + (y / cell)) & 1;
            int delta = checker ? 10 : -10;

            int rCol = baseR + delta;
            int gCol = baseG + delta;
            int bCol = baseB + delta;

            rCol = std::clamp(rCol, 0, 255);
            gCol = std::clamp(gCol, 0, 255);
            bCol = std::clamp(bCol, 0, 255);

            SDL_SetRenderDrawColor(renderer,
                (Uint8)rCol, (Uint8)gCol, (Uint8)bCol, 255);

            r.x = (float)x;
            r.y = (float)y;
            SDL_RenderFillRect(renderer, &r);
        }
    }


    // --- Scatter the waka sprite at random positions (stable set) ---
    if (waka_texture && waka_w > 0 && waka_h > 0) {

        struct BgWakaSprite {
            float nx;    // 0..1 normalized x
            float ny;    // 0..1 normalized y
            float scale; // 0.5..0.8
        };

        static std::vector<BgWakaSprite> sprites;
        static int cachedW = 0;
        static int cachedH = 0;

        auto regenerate = [&](int w, int h) {
            cachedW = w;
            cachedH = h;
            sprites.clear();

            const int count = 6; // keep your density target
            sprites.reserve(count);

            std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
            std::uniform_real_distribution<float> scaleDist(0.5f, 0.8f);

            auto sprite_radius = [&](float scale) -> float {
                // conservative "no overlap" radius based on the larger dimension
                float base = (float)std::max(waka_w, waka_h);
                return 0.5f * base * scale;
                };

            auto torus_dist2 = [&](float x1, float y1, float x2, float y2) -> float {
                float dx = std::fabs(x1 - x2);
                float dy = std::fabs(y1 - y2);

                // wrap-aware shortest distance
                if (w > 0) dx = std::min(dx, (float)w - dx);
                if (h > 0) dy = std::min(dy, (float)h - dy);

                return dx * dx + dy * dy;
                };

            const float padding = 6.0f; // small safety margin

            for (int i = 0; i < count; ++i) {
                bool placed = false;

                // try a bunch of times to find a non-overlapping spot
                for (int attempt = 0; attempt < 300; ++attempt) {
                    float scale = scaleDist(global_rng());
                    float nx = dist01(global_rng());
                    float ny = dist01(global_rng());

                    float x = nx * w;
                    float y = ny * h;

                    float r = sprite_radius(scale);

                    bool ok = true;
                    for (const auto& s : sprites) {
                        float sx = s.nx * w;
                        float sy = s.ny * h;
                        float sr = sprite_radius(s.scale);

                        float minDist = r + sr + padding;
                        if (torus_dist2(x, y, sx, sy) < minDist * minDist) {
                            ok = false;
                            break;
                        }
                    }

                    if (ok) {
                        sprites.push_back({ nx, ny, scale });
                        placed = true;
                        break;
                    }
                }

                // If we can't place without overlap, stop early.
                // Fewer sprites is better than violating "never overlap".
                if (!placed) {
                    break;
                }
            }
        };


        if (sprites.empty() || winW != cachedW || winH != cachedH) {
            regenerate(winW, winH);
        }

               SDL_FRect dst{};

        for (const auto& s : sprites) {
            // Base position from normalized coords
            float baseX = s.nx * winW;
            float baseY = s.ny * winH;

            dst.w = (float)waka_w * s.scale;
            dst.h = (float)waka_h * s.scale;

            // Unwrapped position with global drift
            float x0 = baseX + offset_x;
            float y0 = baseY + offset_y;

            SDL_FPoint pivot{ dst.w * 0.5f, dst.h * 0.5f };

            // Tile sprites across screen borders so they
            // slide in and out smoothly with no pop-in.
            for (int tileY = -1; tileY <= 1; ++tileY) {
                for (int tileX = -1; tileX <= 1; ++tileX) {
                    float x = x0 + tileX * winW;
                    float y = y0 + tileY * winH;

                    float left   = x - dst.w * 0.5f;
                    float right  = x + dst.w * 0.5f;
                    float top    = y - dst.h * 0.5f;
                    float bottom = y + dst.h * 0.5f;

                    // Skip tiles that are completely off-screen
                    if (right < 0.0f || left > (float)winW ||
                        bottom < 0.0f || top > (float)winH) {
                        continue;
                    }

                    dst.x = left;
                    dst.y = top;

                    SDL_RenderTextureRotated(renderer,
                        waka_texture,
                        nullptr,
                        &dst,
                        angle_deg,
                        &pivot,
                        SDL_FLIP_NONE);
                }
            }
        }
    }
}
static void frame(const TwitchConfig& cfg, float dt)
{
    const float PI = 3.14159265358979323846f;
    const float TWO_PI = 2.0f * PI;

    // ---- Copy entries under mutex so we don't draw while holding the lock ----
    std::vector<WheelEntry> entriesCopy;
    {
        std::lock_guard<std::mutex> lock(app.entries_mutex);
        entriesCopy = app.entries;
    }

    int n = static_cast<int>(entriesCopy.size());
int activeIndex = -1;
if (app.spinning && n > 0) {
    activeIndex = pointer_slice_index(app.current_angle, n);
}

  

    if (app.reset_hold_active) {
        // If for some reason the winner is gone, cancel the hold
        bool winnerShowing = (app.celebration_active || app.winner_index >= 0);
        if (!winnerShowing) {
            app.reset_hold_active = false;
            app.reset_hold_elapsed = 0.0f;
            app.reset_hold_source = 0;
        } else {
            app.reset_hold_elapsed += dt;
            const float requiredHold = 1.0f; // seconds

            if (app.reset_hold_elapsed >= requiredHold) {
                // Completed hold: perform the reset via existing logic
                app.reset_hold_active  = false;
                app.reset_hold_elapsed = 0.0f;
                app.reset_hold_source  = 0;

                handle_spin_or_reset(app, cfg);
                // handle_spin_or_reset will clear winner_index / celebration
                // and prepare for the next round.
            }
        }
    }
    // ---- Wheel physics (spin, friction, choose winner) ----
    if (app.spinning && n > 0) {
        // Advance angle
        app.current_angle += app.angular_velocity * dt;

        // Wrap angle to [0, 2π) to avoid float blow-up
        app.current_angle = std::fmod(app.current_angle, TWO_PI);
        if (app.current_angle < 0.0f) {
            app.current_angle += TWO_PI;
        }

        // Apply friction
        float friction = std::max(0.0f, app.spin_friction);
        app.angular_velocity -= friction * dt;

        // If we ran out of speed, stop and pick a winner
        if (app.angular_velocity <= 0.0f) {
            app.angular_velocity = 0.0f;
            app.spinning = false;

            if (n > 0) {
                int idx = pointer_slice_index(app.current_angle, n);
                if (idx >= 0 && idx < n) {
                    app.winner_index = idx;

                    // Winner visual state
                    app.celebration_active = true;
                    app.celebration_time = 0.0f;
                    app.celebration_name = entriesCopy[idx].name;
                    app.celebration_color = entriesCopy[idx].color;
                    app.winner_flash_remaining = 2.0f;   // seconds
                    app.winner_flash_elapsed = 0.0f;
                }
            }
        }
    }

    // ---- Background waka drift / rotation ----
    const float bgSpeed  = 60.0f;   // was 30.0f
    const float rotSpeed = 60.0f;   // was 10.0f (degrees per second)


    app.bg_waka_offset_x += bgSpeed * 0.6f * dt;
    app.bg_waka_offset_y += bgSpeed * 0.35f * dt;
    app.bg_waka_angle_deg += rotSpeed * dt;

    if (app.bg_waka_angle_deg >= 360.0f)
        app.bg_waka_angle_deg -= 360.0f;
    else if (app.bg_waka_angle_deg < 0.0f)
        app.bg_waka_angle_deg += 360.0f;
    // Keep the background offsets bounded so the waka sprites loop forever
    int winW = 0, winH = 0;
    SDL_GetRenderOutputSize(app.renderer, &winW, &winH);

    if (winW > 0) {
        app.bg_waka_offset_x = std::fmod(app.bg_waka_offset_x, static_cast<float>(winW));
        if (app.bg_waka_offset_x < 0.0f)
            app.bg_waka_offset_x += static_cast<float>(winW);
    }
    if (winH > 0) {
        app.bg_waka_offset_y = std::fmod(app.bg_waka_offset_y, static_cast<float>(winH));
        if (app.bg_waka_offset_y < 0.0f)
            app.bg_waka_offset_y += static_cast<float>(winH);
    }

    // ---- Winner flashing state ----
    bool winner_flash_on = false;
    if (app.winner_flash_remaining > 0.0f) {
        app.winner_flash_remaining -= dt;
        app.winner_flash_elapsed += dt;

        if (app.winner_flash_remaining < 0.0f) {
            app.winner_flash_remaining = 0.0f;
        }

        const float flashPeriod = 0.15f; // seconds
        int phase = static_cast<int>(std::floor(app.winner_flash_elapsed / flashPeriod));
        winner_flash_on = (phase % 2) == 0;
    }

    // ---- Celebration banner timer ----
    if (app.celebration_active) {
        app.celebration_time += dt;
    }

    // ---- Rendering ----
    if (!app.renderer)
        return;

    SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
    SDL_RenderClear(app.renderer);

    // Background
    draw_background(app.renderer,
        app.waka_texture,
        app.waka_w,
        app.waka_h,
        app.bg_waka_offset_x,
        app.bg_waka_offset_y,
        app.bg_waka_angle_deg);

    // Wheel
    draw_wheel(app.renderer,
        app.font,
        entriesCopy,
        app.current_angle,
        app.winner_index,
        app.spinning,
        winner_flash_on,
        app.waka_texture,
        app.waka_w,
        app.waka_h);

    draw_name_list(app.renderer,
               app.list_font,
               entriesCopy,
               activeIndex,
               app.winner_index);
     // Status text / help: top-center rounded container
    {
        // Prefer status_font, fall back to list_font if needed
        TTF_Font* f = app.status_font ? app.status_font : app.list_font;
        if (f && app.renderer) {
            int winW = 0, winH = 0;
            SDL_GetRenderOutputSize(app.renderer, &winW, &winH);

            bool isOpen = app.join_open.load();

            std::string statusText = isOpen
                ? "Type !join in chat to join the Wheel"
                : "Wheel is now closed";

            SDL_Color textColor = isOpen
                ? SDL_Color{ 103, 127,  56, 255 }   // dark green (from reference image)
                : SDL_Color{  15,  62, 139, 255 };  // dark blue (from reference image)

            // Different background hint for open vs closed
            SDL_Color fillColor = isOpen
                ? SDL_Color{ 210, 231, 221, 255 }  // light green (from reference image)
                : SDL_Color{ 206, 226, 255, 255 }; // light blue (from reference image)

            SDL_Color borderColor{ 0, 0, 0, 255 };

            SDL_Surface* surface =
                TTF_RenderText_Blended(f, statusText.c_str(), 0, textColor);
            if (surface) {
                SDL_Texture* texture =
                    SDL_CreateTextureFromSurface(app.renderer, surface);
                if (texture) {
                    float textW = static_cast<float>(surface->w);
                    float textH = static_cast<float>(surface->h);

                    // Padding around text inside the card
                    float paddingX = 18.0f;
                    float paddingY = 8.0f;

                    float cardW = textW + paddingX * 2.0f;
                    float cardH = textH + paddingY * 2.0f;

                    // Centered horizontally, near the top (same Y as before)
                    float centerX = winW * 0.5f;
                    float centerY = winH * 0.07f;

                    float cardX = centerX - cardW * 0.5f;
                    float cardY = centerY - cardH * 0.5f;

                    float outerRadius      = 12.0f;
                    float innerRadius      = 10.0f;
                    float borderThickness  = 2.0f;

                    // Outer border
                    draw_filled_rounded_rect(app.renderer,
                        cardX,
                        cardY,
                        cardW,
                        cardH,
                        outerRadius,
                        borderColor);

                    // Inner fill
                    draw_filled_rounded_rect(app.renderer,
                        cardX + borderThickness,
                        cardY + borderThickness,
                        cardW - borderThickness * 2.0f,
                        cardH - borderThickness * 2.0f,
                        innerRadius,
                        fillColor);

                    // Render text inside
                    SDL_FRect dst{};
                    dst.w = textW;
                    dst.h = textH;
                    dst.x = cardX + paddingX;
                    dst.y = cardY + paddingY;

                    SDL_RenderTexture(app.renderer, texture, nullptr, &dst);

                    SDL_DestroyTexture(texture);
                }
                SDL_DestroySurface(surface);
            }
        }
    }


    // Winner banner overlay
    if (app.celebration_active && !app.celebration_name.empty()) {
        draw_winner_banner(app.renderer,
            app.status_font ? app.status_font : app.font,
            app.celebration_name,
            app.celebration_color,
            app.celebration_time);
    }


    // Hold-to-reset message in lower-right corner
    if (app.reset_hold_active && (app.status_font || app.font)) {
        int winW = 0, winH = 0;
        SDL_GetRenderOutputSize(app.renderer, &winW, &winH);

        std::string msg = "hold for one second to reset...";
        TTF_Font* f = app.status_font ? app.status_font : app.font;
        if (!f) {
            // Shouldn't happen, but bail out safely if it does
            SDL_RenderPresent(app.renderer);
            return;
        }

        // Render once to measure text
        SDL_Color white{ 255, 255, 255, 255 };
        SDL_Surface* surface = TTF_RenderText_Blended(f, msg.c_str(), 0, white);
        if (surface) {
            SDL_Texture* texture = SDL_CreateTextureFromSurface(app.renderer, surface);
            if (texture) {
                float textW = static_cast<float>(surface->w);
                float textH = static_cast<float>(surface->h);

                // Card padding and size
                float paddingX = 16.0f;
                float paddingY = 8.0f;
                float cardW = textW + paddingX * 2.0f;
                float cardH = textH + paddingY * 2.0f;

                float margin = 24.0f;

                // Final position (bottom-right corner with margin)
                float finalX = static_cast<float>(winW) - margin - cardW;
                float finalY = static_cast<float>(winH) - margin - cardH;

                // Start position: fully off-screen to the right
                float startX = static_cast<float>(winW) + cardW;

                // Slide-in over 0.15 seconds
                const float slideInDuration = 0.15f;
                float t = app.reset_hold_elapsed;
                if (t < 0.0f) t = 0.0f;
                if (t > slideInDuration) t = slideInDuration;

                float p = (slideInDuration > 0.0f)
                    ? (t / slideInDuration)
                    : 1.0f;

                // Use existing easing helper for a quick, smooth slide
                p = ease_out_cubic(p);

                float currentX = startX + (finalX - startX) * p;
                float currentY = finalY;

                // Draw rounded card: white border + black fill
                SDL_Color borderColor{ 255, 255, 255, 255 };
                SDL_Color fillColor{ 0, 0, 0, 255 };

                float outerRadius = 10.0f;
                float innerRadius = 8.0f;
                float borderThickness = 2.0f;

                // Outer (border)
                draw_filled_rounded_rect(app.renderer,
                    currentX,
                    currentY,
                    cardW,
                    cardH,
                    outerRadius,
                    borderColor);

                // Inner (fill), inset by border thickness
                draw_filled_rounded_rect(app.renderer,
                    currentX + borderThickness,
                    currentY + borderThickness,
                    cardW - borderThickness * 2.0f,
                    cardH - borderThickness * 2.0f,
                    innerRadius,
                    fillColor);

                // Draw text inside the card, in white
                SDL_FRect dst{};
                dst.w = textW;
                dst.h = textH;
                dst.x = currentX + paddingX;
                dst.y = currentY + paddingY;

                SDL_RenderTexture(app.renderer, texture, nullptr, &dst);

                SDL_DestroyTexture(texture);
            }

            SDL_DestroySurface(surface);
        }
    }


    SDL_RenderPresent(app.renderer);
}


#ifdef __EMSCRIPTEN__
extern "C" {

    EMSCRIPTEN_KEEPALIVE
        void wheel_join(const char* user) {
        if (!user || !user[0]) return;
        if (app.join_open.load() || (app.entries.empty() && g_twitch_cfg.nick == user)) {
            add_player_if_new(user, app.entries, app.entries_mutex);
        }
    }

    
    // Called from JS to tell the C++ side who the channel owner is
    EMSCRIPTEN_KEEPALIVE
    void wheel_set_host(const char* nick, const char* channel) {
        g_twitch_cfg.nick    = nick    ? nick    : "";
        g_twitch_cfg.channel = channel ? channel : "";

        // Update authorization based on host/channel
        app.authorized = is_stream_allowed(g_twitch_cfg.nick, g_twitch_cfg.channel);
    }


    EMSCRIPTEN_KEEPALIVE
        void wheel_reset() {
            {
                // Clear wheel + winner state
                std::lock_guard<std::mutex> lock(app.entries_mutex);
                app.entries.clear();
                app.join_open.store(false); // CLOSED
                app.spinning = false;
                app.angular_velocity = 0.0f;
                app.winner_index = -1;
                app.celebration_active = false;
                app.celebration_time = 0.0f;
                app.celebration_name.clear();
            }

            // Re-add the channel owner as the first entry (if configured)
            if (!g_twitch_cfg.nick.empty()) {
                add_player_if_new(g_twitch_cfg.nick, app.entries, app.entries_mutex);
            }
    }

    EMSCRIPTEN_KEEPALIVE
        void wheel_spin() {
        std::lock_guard<std::mutex> lock(app.entries_mutex);

        if (app.entries.size() < 2 || app.spinning) return;

        if (app.join_open.load()) return; // cannot spin while OPEN


        app.spinning = true;
        app.winner_index = -1;
        app.winner_flash_remaining = 0.0f;
        app.winner_flash_elapsed = 0.0f;

        std::uniform_real_distribution<float> spinSpeed(10.0f, 13.0f);
        std::uniform_real_distribution<float> spinFriction(1.8f, 5.6f);

        app.spin_friction = spinFriction(global_rng());
        app.angular_velocity = spinSpeed(global_rng());
    }

} // extern "C"
#endif



// ----------------------
// Main
// ----------------------

int main(int /*argc*/, char** /*argv*/) {

#ifdef _WIN32
    // Use the icon embedded in the .exe via your .rc file
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "101");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "101");
#endif

    SDL_SetRenderVSync(app.renderer, 1);

// SDL3: returns true on success, false on failure
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }

    // SDL3_ttf: returns true on success, false on failure
    if (!TTF_Init()) {
        std::cerr << "TTF_Init failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        return 1;
    }
    app.font = TTF_OpenFont(FONT_PATH, 34.0f);
    if (!app.font) {
        std::cerr << "TTF_OpenFont failed for '" << FONT_PATH
            << "': " << SDL_GetError() << "\n";
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    app.status_font = TTF_OpenFont(FONT_PATH, 26.0f);
    if (!app.status_font) {
        app.status_font = app.font;
    }
    if (app.font) {
        TTF_SetFontHinting(app.font, TTF_HINTING_LIGHT);
    }
    if (app.status_font && app.status_font != app.font) {
        TTF_SetFontHinting(app.status_font, TTF_HINTING_LIGHT);
    }

    app.list_font = TTF_OpenFont(LIST_FONT_PATH, 15.0f);
    if (app.list_font) {
        TTF_SetFontHinting(app.list_font, TTF_HINTING_LIGHT);
    }

    int windowWidth = 940;
    int windowHeight = 720;

    // SDL3: SDL_CreateWindowAndRenderer returns 0 on success, negative on error
    if (!SDL_CreateWindowAndRenderer("ShoepeWheel",
        windowWidth, windowHeight,
        SDL_WINDOW_RESIZABLE,
        &app.window, &app.renderer)) {
        std::cerr << "SDL_CreateWindowAndRenderer failed: "
            << SDL_GetError() << "\n";
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);

    // SDL3_image: no IMG_Init / IMG_Quit needed in current versions
    app.waka_texture = IMG_LoadTexture(app.renderer, "assets/images/waka.png");
    if (!app.waka_texture) {
        std::cerr << "IMG_LoadTexture failed: " << SDL_GetError() << "\n";
    }
    else {
        SDL_SetTextureScaleMode(app.waka_texture, SDL_SCALEMODE_NEAREST);

        float tw = 0.0f, th = 0.0f;
        if (SDL_GetTextureSize(app.waka_texture, &tw, &th)) {
            app.waka_w = static_cast<int>(tw);
            app.waka_h = static_cast<int>(th);
        }
    }


    // Hardcoded test entries so you can see the wheel without Twitch
  /*
    {
        std::lock_guard<std::mutex> lock(app.entries_mutex);

        const char* names[] = {
            "Alyssa", "Brandon", "Cassie", "Dorian", "Evelyn",
            "Fayeon", "Gideon", "HarperJay", "IrisNova", "JunoSpark",
            "Kairos", "LumenFox", "MaraSky", "Nico_River", "OrionVale",
            "PiperWren", "QuentinZ", "Rhea_Moon", "SableEcho", "TaliaStone",
            "UmaVortex", "ViktorPine", "WendyQuartz", "XanderByte", "YaraNimbus",
            "ZoeKestrel", "ArcadeNimbus7", "BinaryBumble", "CosmicTangerine", "Driftwood_19",
            "EmberCircuit", "FrostyMarmot", "GlitchGarden", "HollowComet", "IndigoHarbor",
            "JigsawLattice", "KettleDrummer", "LanternLogic", "MidnightPogo", "NeonSailor",
            "OctaveMosaic", "PaperDragon", "QuantumNoodle", "RaccoonRumba", "SilverAnvil",
            "TurbineTurtle", "UmbraPancake", "VelvetRockets", "WaffleSentinel", "Zenith_Stargazer"
        };


        app.entries.clear();
        app.entries.reserve(std::size(names));

        for (const char* n : names) {
            app.entries.push_back(WheelEntry{ n, random_color() });
        }
            
    }
  */

    
    TwitchConfig& cfg = g_twitch_cfg;
#ifndef __EMSCRIPTEN__
    load_twitch_config("twitch.cfg", cfg);
    app.authorized = is_stream_allowed(cfg.nick, cfg.channel);
    if (!cfg.nick.empty()) {
        add_player_if_new(cfg.nick, app.entries, app.entries_mutex);
    }

    // Start Twitch thread (only if configured)
    std::atomic<bool> twitchRunning{ false };
    std::thread       twitchThread;
    bool twitchEnabled =
        (!cfg.oauth.empty() &&
            !cfg.nick.empty() &&
            !cfg.channel.empty());

    if (twitchEnabled) {
        twitchRunning = true;
        twitchThread = std::thread(
            twitch_chat_thread,
            cfg,
            std::ref(twitchRunning),
            std::ref(app.entries),
            std::ref(app.entries_mutex));
    }
#endif



    bool quit = false;
    SDL_Event e;
    Uint64 lastTicks = SDL_GetTicks();

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop_arg(
        [](void* arg) {
            TwitchConfig* cfg = static_cast<TwitchConfig*>(arg);

            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) {
                    // ignored in browser
                }
                else if (e.type == SDL_EVENT_KEY_DOWN) {
                    if (e.key.key == SDLK_SPACE) {
                        bool isOpen = app.join_open.load();
                        if (isOpen) {
                            // When OPEN, ignore spin/reset input.
                        } else {
                            bool winnerShowing =
                                (app.celebration_active || app.winner_index >= 0);

                            if (winnerShowing) {
                                if (!app.reset_hold_active) {
                                    app.reset_hold_active  = true;
                                    app.reset_hold_elapsed = 0.0f;
                                    app.reset_hold_source  = 1; // space
                                }
                            } else {
                                handle_spin_or_reset(app, *cfg);
                            }
                        }
                    }
                }
                else if (e.type == SDL_EVENT_KEY_UP) {
                    if (e.key.key == SDLK_SPACE &&
                        app.reset_hold_source == 1) {
                        app.reset_hold_active  = false;
                        app.reset_hold_elapsed = 0.0f;
                        app.reset_hold_source  = 0;
                    }
                }
else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
    if (e.button.button == SDL_BUTTON_LEFT) {
        bool isOpen = app.join_open.load();
        bool winnerShowing =
            (app.celebration_active || app.winner_index >= 0);

        if (isOpen) {
            // When OPEN, left-click is disabled (no spin, no reset-hold).
        } else if (winnerShowing) {
            // After a winner is selected, hold left mouse to reset.
            if (!app.reset_hold_active) {
                app.reset_hold_active  = true;
                app.reset_hold_elapsed = 0.0f;
                app.reset_hold_source  = 2; // mouse
            }
        } else {
            // CLOSED: left-click starts a spin.
            handle_spin_or_reset(app, *cfg);
        }
    }
    else if (e.button.button == SDL_BUTTON_RIGHT) {
        bool winnerShowing =
            (app.celebration_active || app.winner_index >= 0);

        if (winnerShowing) {
            // After a winner is selected, right-click open/close toggle is disabled.
            std::cout << "[Wheel] Join toggle ignored (winner selected)\n";
        } else {
            bool current = app.join_open.load();
            app.join_open.store(!current);
            std::cout << "[Wheel] Join state toggled to "
                      << (app.join_open.load() ? "OPEN" : "CLOSED") << "\n";
        }
    }
}

                else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    if (e.button.button == SDL_BUTTON_LEFT &&
                        app.reset_hold_source == 2) {
                        app.reset_hold_active  = false;
                        app.reset_hold_elapsed = 0.0f;
                        app.reset_hold_source  = 0;
                    }
                }
            }


            static Uint64 last = SDL_GetTicks();
            Uint64 now = SDL_GetTicks();
            float dt = static_cast<float>(now - last) / 1000.0f;
            last = now;

            frame(*cfg, dt);
        },
        &g_twitch_cfg,
        0,
        1
    );
#else
        while (!quit) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                quit = true;
            }
            else if (e.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keycode key = e.key.key;

                if (key == SDLK_ESCAPE) {
                    quit = true;
                }
                else if (key == SDLK_SPACE) {
                    bool winnerShowing =
                        (app.celebration_active || app.winner_index >= 0);

                    if (winnerShowing) {
                        if (!app.reset_hold_active) {
                            app.reset_hold_active  = true;
                            app.reset_hold_elapsed = 0.0f;
                            app.reset_hold_source  = 1; // space
                        }
                    } else {
                        // Normal behavior: spin / reset as before
                        handle_spin_or_reset(app, cfg);
                    }
                }
            }
            else if (e.type == SDL_EVENT_KEY_UP) {
                if (e.key.key == SDLK_SPACE &&
                    app.reset_hold_source == 1) {
                    // Cancel the hold if the spacebar is released early
                    app.reset_hold_active  = false;
                    app.reset_hold_elapsed = 0.0f;
                    app.reset_hold_source  = 0;
                }
            }
else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
    if (e.button.button == SDL_BUTTON_LEFT) {
        bool winnerShowing =
            (app.celebration_active || app.winner_index >= 0);

        if (winnerShowing) {
            if (!app.reset_hold_active) {
                app.reset_hold_active  = true;
                app.reset_hold_elapsed = 0.0f;
                app.reset_hold_source  = 2; // mouse
            }
        } else {
            // Normal behavior: spin / reset as before
            handle_spin_or_reset(app, cfg);
        }
    }
    else if (e.button.button == SDL_BUTTON_RIGHT) {
        bool current = app.join_open.load();
        app.join_open.store(!current);
        std::cout << "[Wheel] Join state toggled to "
                  << (app.join_open.load() ? "OPEN" : "CLOSED") << "\n";
    }
}

            else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                if (e.button.button == SDL_BUTTON_LEFT &&
                    app.reset_hold_source == 2) {
                    // Cancel the hold if mouse button is released early
                    app.reset_hold_active  = false;
                    app.reset_hold_elapsed = 0.0f;
                    app.reset_hold_source  = 0;
                }
            }
        }

        Uint64 now = SDL_GetTicks();
        float dt = static_cast<float>(now - lastTicks) / 1000.0f;
        lastTicks = now;

        frame(cfg, dt);
        
    }

#endif


    // Shutdown Twitch
#ifndef __EMSCRIPTEN__
    if (twitchEnabled) {
        twitchRunning.store(false);
        if (twitchThread.joinable()) {
            twitchThread.join();
        }
    }
#endif

    if (app.renderer) SDL_DestroyRenderer(app.renderer);
    if (app.window)   SDL_DestroyWindow(app.window);
    if (app.status_font && app.status_font != app.font) {
        TTF_CloseFont(app.status_font);
    }
    if (app.font) {
        TTF_CloseFont(app.font);
    }


    TTF_Quit();
    
    SDL_Quit();
    return 0;
}
