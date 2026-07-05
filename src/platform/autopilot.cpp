// autopilot.cpp  — see autopilot.h

#include "autopilot.h"
#include "../rt/render_api.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>

namespace {

struct Action {
    Uint32      timeMs;
    std::string verb;   // key/hold/release/mode/shot/quit
    std::string arg;
    bool        done = false;
    Uint32      releaseAt = 0;   // for 'key': when to send KEYUP
    SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
};

std::vector<Action> g_actions;
std::vector<Action> g_pendingUp;   // scheduled key releases
Uint32 g_start = 0;
bool   g_active = false;
bool   g_inited = false;

SDL_Scancode NameToScancode(const std::string &n) {
    if (n == "enter" || n == "return") return SDL_SCANCODE_RETURN;
    if (n == "esc")   return SDL_SCANCODE_ESCAPE;
    if (n == "space") return SDL_SCANCODE_SPACE;
    if (n == "up")    return SDL_SCANCODE_UP;
    if (n == "down")  return SDL_SCANCODE_DOWN;
    if (n == "left")  return SDL_SCANCODE_LEFT;
    if (n == "right") return SDL_SCANCODE_RIGHT;
    if (n == "ctrl")  return SDL_SCANCODE_LCTRL;
    if (n == "alt")   return SDL_SCANCODE_LALT;
    if (n == "shift") return SDL_SCANCODE_LSHIFT;
    if (n == "tab")   return SDL_SCANCODE_TAB;
    if (n == "y")     return SDL_SCANCODE_Y;
    if (n == "n")     return SDL_SCANCODE_N;
    // single letters / digits / fN
    if (n.size() == 1) {
        char c = n[0];
        if (c >= 'a' && c <= 'z') return (SDL_Scancode)(SDL_SCANCODE_A + (c - 'a'));
        if (c >= '1' && c <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + (c - '1'));
        if (c == '0')             return SDL_SCANCODE_0;
    }
    if (n.size() >= 2 && n[0] == 'f') {
        int fn = atoi(n.c_str() + 1);
        if (fn >= 1 && fn <= 12) return (SDL_Scancode)(SDL_SCANCODE_F1 + (fn - 1));
    }
    SDL_Scancode sc = SDL_GetScancodeFromName(n.c_str());
    return sc;
}

int ModeFromName(const std::string &n) {
    if (n == "classic") return RM_CLASSIC;
    if (n == "pt" || n == "pathtraced") return RM_PATHTRACED;
    if (n == "split")   return RM_SPLIT;
    if (n == "wipe")    return RM_WIPE;
    if (n == "freeze")  return RM_FREEZE;
    return RM_CLASSIC;
}

void PushKey(SDL_Scancode sc, bool down) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = sc;
    e.key.keysym.sym = SDL_GetKeyFromScancode(sc);
    SDL_PushEvent(&e);
}

void ParseScript(const char *script) {
    // Split on ';'
    std::string s(script);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t semi = s.find(';', pos);
        std::string part = s.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        pos = (semi == std::string::npos) ? s.size() : semi + 1;

        // trim
        size_t a = part.find_first_not_of(" \t\n");
        if (a == std::string::npos) continue;
        part = part.substr(a);

        // "<timeMs> <verb> [arg]"
        char verb[32] = {0}, arg[32] = {0};
        unsigned t = 0;
        int got = sscanf(part.c_str(), "%u %31s %31s", &t, verb, arg);
        if (got < 2) continue;
        Action ac;
        ac.timeMs = t;
        ac.verb = verb;
        ac.arg = arg;
        if (ac.verb == "key" || ac.verb == "hold" || ac.verb == "release")
            ac.sc = NameToScancode(ac.arg);
        g_actions.push_back(ac);
    }
    printf("[auto] loaded %zu scripted actions\n", g_actions.size());
}

} // namespace

extern "C" {

void AUTO_Init(void) {
    if (g_inited) return;
    g_inited = true;
    g_start = SDL_GetTicks();

    const char *mode = getenv("WOLFPT_MODE");
    if (mode) RENDER_SetMode((RenderMode)ModeFromName(mode));

    const char *script = getenv("WOLFPT_SCRIPT");
    const char *autoshot = getenv("WOLFPT_AUTOSHOT");
    if (script) {
        ParseScript(script);
        g_active = true;
    } else if (autoshot) {
        // Convenience: shoot at N ms then quit shortly after.
        unsigned t = (unsigned)atoi(autoshot);
        char buf[128];
        snprintf(buf, sizeof(buf), "%u shot; %u quit", t, t + 500);
        ParseScript(buf);
        g_active = true;
    }
}

void AUTO_Tick(void) {
    if (!g_inited) AUTO_Init();
    if (!g_active) return;

    Uint32 now = SDL_GetTicks() - g_start;

    // process key releases first
    for (auto it = g_pendingUp.begin(); it != g_pendingUp.end();) {
        if (now >= it->releaseAt) { PushKey(it->sc, false); it = g_pendingUp.erase(it); }
        else ++it;
    }

    for (auto &ac : g_actions) {
        if (ac.done || now < ac.timeMs) continue;
        ac.done = true;
        if (ac.verb == "key") {
            PushKey(ac.sc, true);
            Action up; up.sc = ac.sc; up.releaseAt = now + 260;
            g_pendingUp.push_back(up);
            printf("[auto] %ums key %s\n", now, ac.arg.c_str());
        } else if (ac.verb == "hold") {
            PushKey(ac.sc, true);
            printf("[auto] %ums hold %s\n", now, ac.arg.c_str());
        } else if (ac.verb == "release") {
            PushKey(ac.sc, false);
            printf("[auto] %ums release %s\n", now, ac.arg.c_str());
        } else if (ac.verb == "mode") {
            RENDER_SetMode((RenderMode)ModeFromName(ac.arg));
            printf("[auto] %ums mode %s\n", now, ac.arg.c_str());
        } else if (ac.verb == "shot") {
            RENDER_Screenshot(0);
            printf("[auto] %ums screenshot\n", now);
        } else if (ac.verb == "quit") {
            printf("[auto] %ums quit\n", now);
            SDL_Event e; SDL_zero(e); e.type = SDL_QUIT; SDL_PushEvent(&e);
        }
    }
}

int AUTO_Active(void) { return g_active ? 1 : 0; }

} // extern "C"
