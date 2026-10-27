// goginput_remappping_tool_sdl.cpp -- interactive preset builder for GOG's DirectInput wrapper (goginput.ini)
//
// Build (MSVC, x86 or x64, from a Developer Command Prompt):
//     cl /EHsc /O2 goginput_remapping_tool_sdl.cpp dinput8.lib dxguid.lib user32.lib
// Build (MinGW-w64):
//     g++ -O2 -static goginput_remappping_tool_sdl.cpp -o goginput_remapping_tool_sdl.exe -ldinput8 -ldxguid -luser32
//
// Usage:
//     goginput_remappping_tool_sdl.exe [path\to\goginput.ini]     (default: .\goginput.ini)
//     goginput_remappping_tool_sdl.exe --monitor                  (just show what the pad reports)
//     options: --sdl <path\SDL2.dll>   use this SDL2.dll (default: next to the ini, next to the tool, then PATH)
//              --no-sdl                 DirectInput only
//              --sdl-native             don't force SDL's DirectInput backend (see the SDL section)
// SDL2.dll is loaded at run time, so there is no SDL link dependency. Its bitness must match this exe.
//
// ini semantics (taken from the shipped goginput.ini):  <physical input> = <what the game sees>
// The "game sees" side is the Xbox-style layout the shipped presets target:
//   buttons 0..11 = A B X Y LB RB LT RT Back Start LS RS, x/y = left stick, rx/ry = right stick, dpad = hat.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <fstream>
#include <iostream>
#include <algorithm>

#if defined(_WIN32) && !defined(TEST_CORE)
#define WIN_UI 1
#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <conio.h>
#endif

using std::string;
using std::vector;

// ============================================================ core (no Windows dependencies)
struct Map { string src, dst; };
struct Caps { bool ax[6] = {false, false, false, false, false, false}; int buttons = 0; };

static const char* AXN[6] = {"x", "y", "z", "rx", "ry", "rz"};
static string axName(int a, int sign) { return string("pad_") + AXN[a] + "axis" + (sign > 0 ? "plus" : "minus"); }

static string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == string::npos ? "" : s.substr(a, b - a + 1);
}
static string lower(string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static string upper(string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }

static vector<string> readLines(const string& path, bool& ok) {
    std::ifstream f(path.c_str(), std::ios::binary);
    ok = f.good();
    vector<string> L; string l;
    while (std::getline(f, l)) { if (!l.empty() && l.back() == '\r') l.pop_back(); L.push_back(l); }
    return L;
}
static bool writeLines(const string& path, const vector<string>& L) {
    std::ofstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    for (auto& l : L) f << l << "\r\n";
    return f.good();
}

static bool header(const string& l, string* n = nullptr) {
    string t = trim(l);
    if (t.size() < 2 || t[0] != '[' || t.back() != ']') return false;
    if (n) *n = trim(t.substr(1, t.size() - 2));
    return true;
}
static int findName(const vector<string>& L, const string& name) {
    string w = lower(name), n;
    for (size_t i = 0; i < L.size(); i++) if (header(L[i], &n) && lower(n) == w) return (int)i;
    return -1;
}
static int findGuid(const vector<string>& L, const string& guid) {
    int cur = -1;
    for (size_t i = 0; i < L.size(); i++) {
        if (header(L[i])) { cur = (int)i; continue; }
        if (cur < 0) continue;
        size_t e = L[i].find('=');
        if (e == string::npos) continue;
        if (lower(trim(L[i].substr(0, e))) == "guid" && upper(trim(L[i].substr(e + 1))) == upper(guid)) return cur;
    }
    return -1;
}
static int secEnd(const vector<string>& L, int i) {   // exclusive; trailing blank lines are not part of the section
    size_t j = i + 1;
    while (j < L.size() && !header(L[j])) j++;
    while (j > (size_t)i + 1 && trim(L[j - 1]).empty()) j--;
    return (int)j;
}
static void putSection(vector<string>& L, const vector<string>& sec, int replace) {
    if (replace >= 0) {
        int e = secEnd(L, replace);
        L.erase(L.begin() + replace, L.begin() + e);
        L.insert(L.begin() + replace, sec.begin(), sec.end());
        return;
    }
    int k = findName(L, "keyboardandmouse");
    if (k >= 0) {
        vector<string> add = sec; add.push_back("");
        L.insert(L.begin() + k, add.begin(), add.end());
        return;
    }
    if (!L.empty() && !trim(L.back()).empty()) L.push_back("");
    L.insert(L.end(), sec.begin(), sec.end());
}

// Turn the recorded (physical -> logical) pairs into the final ini lines.
// Unmapped physical inputs that would collide with a logical target in use are sent to dev_null,
// exactly like the shipped DualShock/DualSense presets do with the analog trigger axes.
static vector<Map> finalize(const vector<Map>& raw, const Caps& c, vector<string>& warn) {
    std::map<string, string> m; std::set<string> used, exist;
    for (auto& r : raw) {
        used.insert(r.dst);
        auto it = m.find(r.src);
        if (it != m.end()) { if (it->second != r.dst) warn.push_back(r.src + " was assigned twice; keeping " + it->second); continue; }
        m[r.src] = r.dst;
    }
    for (int a = 0; a < 6; a++) if (c.ax[a]) { exist.insert(axName(a, 1)); exist.insert(axName(a, -1)); }
    for (int b = 0; b < c.buttons && b < 32; b++) exist.insert("pad_button" + std::to_string(b));
    for (auto& e : exist) if (!m.count(e) && used.count(e)) m[e] = "dev_null";
    for (auto it = m.begin(); it != m.end();) { if (it->first == it->second) it = m.erase(it); else ++it; }
    vector<string> order;
    for (int a = 0; a < 6; a++) { order.push_back(axName(a, 1)); order.push_back(axName(a, -1)); }
    for (const char* d : {"up", "down", "left", "right"}) order.push_back(string("pad_dpad") + d);
    for (int b = 0; b < 32; b++) order.push_back("pad_button" + std::to_string(b));
    vector<Map> out;
    for (auto& o : order) if (m.count(o)) out.push_back({o, m[o]});
    return out;
}
static string padKey(const string& k) { return k.size() < 18 ? k + string(18 - k.size(), ' ') : k + " "; }
static vector<string> buildSection(const string& name, const string& guid, const vector<Map>& m) {
    vector<string> s;
    s.push_back("[" + name + "]");
    s.push_back(padKey("guid") + "= " + guid);
    for (auto& x : m) s.push_back(padKey(x.src) + "= " + x.dst);
    return s;
}


struct Hit { int kind = 0; int idx = 0; int sign = 0; long mag = 0; };   // kind: 1 button, 2 axis (sign = direction of travel), 3 hat (idx 0..3 = up,right,down,left)
static string hitName(const Hit& h) {
    static const char* dn[4] = {"hat UP", "hat RIGHT", "hat DOWN", "hat LEFT"};
    if (h.kind == 1) return "pad_button" + std::to_string(h.idx);
    if (h.kind == 2) return axName(h.idx, h.sign);
    if (h.kind == 3) return dn[h.idx & 3];
    return "(none)";
}

// mode 0 = stick (axes only), 1 = button/trigger (button > hat > axis), 2 = d-pad (hat > button > axis); sdl = SDL_GameController name
struct Step { const char* label; int mode; int tax; const char* tgt; const char* sdl; };
static const Step steps[] = {
    {"LEFT STICK  - push fully RIGHT", 0, 0, "", "leftx"},
    {"LEFT STICK  - push fully DOWN", 0, 1, "", "lefty"},
    {"RIGHT STICK - push fully RIGHT", 0, 3, "", "rightx"},
    {"RIGHT STICK - push fully DOWN", 0, 4, "", "righty"},
    {"A / Cross (bottom face button)", 1, -1, "pad_button0", "a"},
    {"B / Circle (right face button)", 1, -1, "pad_button1", "b"},
    {"X / Square (left face button)", 1, -1, "pad_button2", "x"},
    {"Y / Triangle (top face button)", 1, -1, "pad_button3", "y"},
    {"LB / L1 (left bumper)", 1, -1, "pad_button4", "leftshoulder"},
    {"RB / R1 (right bumper)", 1, -1, "pad_button5", "rightshoulder"},
    {"LT / L2 (left trigger)", 1, -1, "pad_button6", "lefttrigger"},
    {"RT / R2 (right trigger)", 1, -1, "pad_button7", "righttrigger"},
    {"Back / Select / Share", 1, -1, "pad_button8", "back"},
    {"Start / Options", 1, -1, "pad_button9", "start"},
    {"LS / L3 (click left stick)", 1, -1, "pad_button10", "leftstick"},
    {"RS / R3 (click right stick)", 1, -1, "pad_button11", "rightstick"},
    {"D-PAD UP", 2, -1, "pad_dpadup", "dpup"},
    {"D-PAD DOWN", 2, -1, "pad_dpaddown", "dpdown"},
    {"D-PAD LEFT", 2, -1, "pad_dpadleft", "dpleft"},
    {"D-PAD RIGHT", 2, -1, "pad_dpadright", "dpright"},
};
static const unsigned NSTEPS = (unsigned)(sizeof(steps) / sizeof(steps[0]));

// ---- SDL game-controller mapping strings ("a:b1,leftx:a0,lefttrigger:+a2,dpup:h0.1,...") ----
static std::map<string, string> parseMapping(const string& m) {
    std::map<string, string> r; size_t p = 0; int field = 0;
    while (p <= m.size()) {
        size_t e = m.find(',', p); if (e == string::npos) e = m.size();
        string f = m.substr(p, e - p);
        size_t c = f.find(':');
        if (field >= 2 && c != string::npos) r[trim(f.substr(0, c))] = trim(f.substr(c + 1));   // fields 0/1 are GUID and name
        field++; p = e + 1;
    }
    return r;
}
static bool parseElem(string v, Hit& h) {   // one mapping value, still in SDL's joystick index space
    if (v.empty()) return false;
    int half = 0; size_t p = 0; bool inv = false;
    if (v[0] == '+') { half = 1; p = 1; } else if (v[0] == '-') { half = -1; p = 1; }
    if (v.back() == '~') { inv = true; v.pop_back(); }
    if (p >= v.size()) return false;
    char t = v[p];
    if (t == 'b') { h.kind = 1; h.idx = atoi(v.c_str() + p + 1); return true; }
    if (t == 'a') { h.kind = 2; h.idx = atoi(v.c_str() + p + 1); h.sign = half ? half : 1; if (inv) h.sign = -h.sign; return true; }
    if (t == 'h') {
        size_t d = v.find('.', p); if (d == string::npos) return false;
        int mask = atoi(v.c_str() + d + 1);
        int dir = mask == 1 ? 0 : mask == 2 ? 1 : mask == 4 ? 2 : mask == 8 ? 3 : -1;
        if (dir < 0) return false;
        h.kind = 3; h.idx = dir; return true;
    }
    return false;
}
// SDL's DirectInput backend numbers axes in the order DirectInput lists them (x,y,z,rx,ry,rz), skipping axes the pad lacks,
// and buttons by their DirectInput index. So SDL "a3" is the 4th axis this pad really has.
static bool toDI(const Hit& s, const Caps& c, Hit& out) {
    out = s;
    if (s.kind == 2) { int n = -1; for (int a = 0; a < 6; a++) if (c.ax[a] && ++n == s.idx) { out.idx = a; return true; } return false; }
    if (s.kind == 1) return s.idx < c.buttons;
    return s.kind == 3;
}
static vector<Hit> suggest(const string& mapping, const Caps& c) {
    std::map<string, string> m = parseMapping(mapping);
    vector<Hit> r(NSTEPS);
    for (unsigned i = 0; i < NSTEPS; i++) {
        auto it = m.find(steps[i].sdl); Hit s, d;
        if (it == m.end() || !parseElem(it->second, s) || !toDI(s, c, d)) continue;
        if (steps[i].mode == 0 && d.kind != 2) continue;
        r[i] = d;
    }
    return r;
}
static bool sameHit(const Hit& a, const Hit& b) { return a.kind == b.kind && a.idx == b.idx && (a.kind != 2 || a.sign == b.sign); }

// Record one answer (live press or SDL suggestion) as ini pairs. Returns a short description for the console.
static string applyHit(const Step& st, const Hit& h, vector<Map>& raw, bool& hatDpad) {
    if (st.mode == 0) {
        if (h.kind != 2) return "ignored (a stick needs an axis)";
        raw.push_back({axName(h.idx, h.sign), axName(st.tax, 1)});
        raw.push_back({axName(h.idx, -h.sign), axName(st.tax, -1)});
        return string(AXN[h.idx]) + (h.sign < 0 ? " (inverted)" : "");
    }
    if (h.kind == 3) {
        if (st.mode == 2) { hatDpad = true; return "D-pad is a hat (POV); nothing to remap"; }
        return "a hat direction can't be mapped to a button; skipped";
    }
    string src = hitName(h);
    raw.push_back({src, st.tgt});
    return src;
}

// ============================================================ Windows / DirectInput part
#ifdef WIN_UI
static IDirectInput8A* g_di = nullptr;
static IDirectInputDevice8A* g_dev = nullptr;
static Caps g_caps;
static int g_pov = 0;
static long g_rest[6] = {0, 0, 0, 0, 0, 0};
static const long THR = 18000;   // axis travel (out of 65535) that counts as "pressed"

struct DevInfo { GUID inst, prod; string name; };

static string guidStr(const GUID& g) {
    char b[64];
    sprintf(b, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", (unsigned long)g.Data1, g.Data2, g.Data3,
            g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}
static BOOL CALLBACK enumDev(const DIDEVICEINSTANCEA* d, void* ctx) {
    DevInfo i; i.inst = d->guidInstance; i.prod = d->guidProduct; i.name = d->tszProductName;
    ((vector<DevInfo>*)ctx)->push_back(i);
    return DIENUM_CONTINUE;
}
static BOOL CALLBACK enumObj(const DIDEVICEOBJECTINSTANCEA* o, void*) {
    if (!(o->dwType & DIDFT_AXIS)) return DIENUM_CONTINUE;
    DIPROPRANGE r; r.diph.dwSize = sizeof(r); r.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    r.diph.dwHow = DIPH_BYID; r.diph.dwObj = o->dwType; r.lMin = -32768; r.lMax = 32767;
    g_dev->SetProperty(DIPROP_RANGE, &r.diph);
    DIPROPDWORD dz; dz.diph.dwSize = sizeof(dz); dz.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    dz.diph.dwHow = DIPH_BYID; dz.diph.dwObj = o->dwType; dz.dwData = 0;
    g_dev->SetProperty(DIPROP_DEADZONE, &dz.diph);
    int i = -1;
    if (o->guidType == GUID_XAxis) i = 0; else if (o->guidType == GUID_YAxis) i = 1;
    else if (o->guidType == GUID_ZAxis) i = 2; else if (o->guidType == GUID_RxAxis) i = 3;
    else if (o->guidType == GUID_RyAxis) i = 4; else if (o->guidType == GUID_RzAxis) i = 5;
    if (i >= 0) g_caps.ax[i] = true;
    return DIENUM_CONTINUE;
}
static bool readState(DIJOYSTATE2& s) {
    HRESULT hr = g_dev->Poll();
    if (FAILED(hr)) { g_dev->Acquire(); g_dev->Poll(); }
    hr = g_dev->GetDeviceState(sizeof(s), &s);
    if (FAILED(hr)) { g_dev->Acquire(); hr = g_dev->GetDeviceState(sizeof(s), &s); }
    return SUCCEEDED(hr);
}
static long axisOf(const DIJOYSTATE2& s, int i) { long v[6] = {s.lX, s.lY, s.lZ, s.lRx, s.lRy, s.lRz}; return v[i]; }
static vector<Hit> active(const DIJOYSTATE2& s) {
    vector<Hit> h;
    for (int b = 0; b < g_caps.buttons && b < 32; b++) if (s.rgbButtons[b] & 0x80) { Hit x; x.kind = 1; x.idx = b; h.push_back(x); }
    for (int a = 0; a < 6; a++) if (g_caps.ax[a]) {
        long d = axisOf(s, a) - g_rest[a];
        if (labs(d) > THR) { Hit x; x.kind = 2; x.idx = a; x.sign = d > 0 ? 1 : -1; x.mag = labs(d); h.push_back(x); }
    }
    for (int p = 0; p < g_pov && p < 4; p++) {
        DWORD v = s.rgdwPOV[p];
        if (LOWORD(v) != 0xFFFF) { Hit x; x.kind = 3; x.idx = (int)(((v + 4500) / 9000) % 4); h.push_back(x); }
    }
    return h;
}
static bool keySkip() {
    while (_kbhit()) { int c = _getch(); if (c == 's' || c == 'S' || c == '\r') return true; }
    return false;
}
// mode 0 = stick (axes only), 1 = button/trigger (button > hat > axis), 2 = d-pad (hat > button > axis)
static vector<Hit> filterHits(const vector<Hit>& h, int mode) {
    if (mode != 0) return h;
    vector<Hit> r; for (auto& x : h) if (x.kind == 2) r.push_back(x);
    return r;
}
static Hit pickHit(const vector<Hit>& h, int mode) {
    static const int ord[3][3] = {{2, 0, 0}, {1, 3, 2}, {3, 1, 2}};
    for (int k = 0; k < 3; k++) {
        int kind = ord[mode][k]; if (!kind) continue;
        Hit best; bool f = false;
        for (auto& x : h) if (x.kind == kind && (!f || (kind == 2 && x.mag > best.mag))) { best = x; f = true; }
        if (f) return best;
    }
    return h[0];
}
static int waitFor(int mode, Hit& out) {   // 0 = got input, 1 = skipped / device lost
    DIJOYSTATE2 s; bool warned = false;
    for (;;) {
        if (!readState(s)) return 1;
        if (active(s).empty()) break;
        if (!warned) { puts("   (release all inputs first)"); warned = true; }
        if (keySkip()) return 1;
        Sleep(10);
    }
    Sleep(100);
    for (;;) {
        if (keySkip()) return 1;
        if (!readState(s)) return 1;
        vector<Hit> h = filterHits(active(s), mode);
        if (!h.empty()) {
            Sleep(200);   // let a trigger's button AND axis both register, then pick by priority
            if (readState(s)) { vector<Hit> h2 = filterHits(active(s), mode); if (!h2.empty()) h = h2; }
            out = pickHit(h, mode);
            return 0;
        }
        Sleep(5);
    }
}
static void monitor() {
    puts("Monitor mode: move sticks / press buttons. Names are the ini keys (buttons are 0-based). Ctrl+C to quit.\n");
    string last; DIJOYSTATE2 s;
    for (;;) {
        if (!readState(s)) { puts("device lost"); return; }
        string t; for (auto& x : active(s)) t += hitName(x) + "   ";
        if (t != last) { printf("%s\n", t.empty() ? "(none)" : t.c_str()); last = t; }
        Sleep(10);
    }
}
static bool ask(const char* q, bool def) {
    printf("%s [%s] ", q, def ? "Y/n" : "y/N"); fflush(stdout);
    string l; std::getline(std::cin, l); l = trim(l);
    if (l.empty()) return def;
    return l[0] == 'y' || l[0] == 'Y';
}
static void pause() { printf("\nPress any key to exit..."); fflush(stdout); _getch(); }

// ---- SDL (optional, loaded at run time) --------------------------------------------------------------
// The ini keys are plain DirectInput indices, so DirectInput stays the source of truth for the real presses.
// SDL adds what DirectInput can't: a friendly name, SDL's controller database (a ready-made layout for most pads)
// and a cross-check. By default SDL is forced onto its DirectInput backend (hints below) so its button/axis numbering
// lines up with DirectInput's; with HIDAPI/RawInput on, PlayStation/Switch pads get SDL-synthesised layouts.
struct SdlGuid { unsigned char b[16]; };
static struct SdlApi {
    HMODULE h;
    int (__cdecl* Init)(unsigned);
    int (__cdecl* SetHint)(const char*, const char*);
    int (__cdecl* Num)();
    const char* (__cdecl* Name)(int);
    SdlGuid (__cdecl* DevGuid)(int);
    void (__cdecl* GuidStr)(SdlGuid, char*, int);
    int (__cdecl* IsGC)(int);
    char* (__cdecl* MapIdx)(int);
    char* (__cdecl* MapGuid)(SdlGuid);
    void (__cdecl* Free)(void*);
    const char* (__cdecl* Err)();
    void* (__cdecl* RWFile)(const char*, const char*);
    int (__cdecl* AddRW)(void*, int);
    void (__cdecl* Update)();
} g_sdl;
struct SdlDev { string name, guid, mapping; unsigned vid = 0, pid = 0; bool gc = false; };

static string dirOf(const string& p) { size_t i = p.find_last_of("\\/"); return i == string::npos ? "" : p.substr(0, i + 1); }
static bool sdlLoad(const string& explicitPath, const string& iniPath) {
    vector<string> tries; DWORD err = 0;
    if (!explicitPath.empty()) tries.push_back(explicitPath);
    else { tries.push_back(dirOf(iniPath) + "SDL2.dll"); tries.push_back("SDL2.dll"); }
    for (auto& t : tries) {
        g_sdl.h = LoadLibraryA(t.c_str());
        if (g_sdl.h) break;
        DWORD e = GetLastError(); if (e == 193 || !err) err = e;   // 193 = wrong bitness
    }
    if (!g_sdl.h) {
        printf("SDL: SDL2.dll %s - continuing with DirectInput only.\n\n",
               err == 193 ? "found but its bitness differs from this exe (build x86 to use a 32-bit SDL2.dll)"
                          : "not found (put it next to goginput.ini or this tool, or pass --sdl <path>)");
        return false;
    }
#define SDLF(f, n) g_sdl.f = (decltype(g_sdl.f))(void*)GetProcAddress(g_sdl.h, n)
    SDLF(Init, "SDL_Init"); SDLF(SetHint, "SDL_SetHint"); SDLF(Num, "SDL_NumJoysticks"); SDLF(Name, "SDL_JoystickNameForIndex");
    SDLF(DevGuid, "SDL_JoystickGetDeviceGUID"); SDLF(GuidStr, "SDL_JoystickGetGUIDString"); SDLF(IsGC, "SDL_IsGameController");
    SDLF(MapIdx, "SDL_GameControllerMappingForDeviceIndex"); SDLF(MapGuid, "SDL_GameControllerMappingForGUID");
    SDLF(Free, "SDL_free"); SDLF(Err, "SDL_GetError"); SDLF(RWFile, "SDL_RWFromFile");
    SDLF(AddRW, "SDL_GameControllerAddMappingsFromRW"); SDLF(Update, "SDL_JoystickUpdate");
#undef SDLF
    if (!(g_sdl.Init && g_sdl.SetHint && g_sdl.Num && g_sdl.Name && g_sdl.DevGuid && g_sdl.GuidStr && g_sdl.IsGC && g_sdl.Free && (g_sdl.MapIdx || g_sdl.MapGuid))) {
        puts("SDL: this SDL2.dll is too old / missing functions - continuing with DirectInput only.\n"); return false;
    }
    return true;
}
static vector<SdlDev> sdlScan(bool forceDI, const string& iniPath) {
    vector<SdlDev> out;
    if (forceDI) { g_sdl.SetHint("SDL_JOYSTICK_HIDAPI", "0"); g_sdl.SetHint("SDL_JOYSTICK_RAWINPUT", "0"); g_sdl.SetHint("SDL_XINPUT_ENABLED", "0"); }
    if (g_sdl.Init(0x00000200u | 0x00002000u) != 0) { printf("SDL_Init failed: %s\n\n", g_sdl.Err ? g_sdl.Err() : "?"); return out; }   // JOYSTICK | GAMECONTROLLER
    if (g_sdl.RWFile && g_sdl.AddRW) {
        for (const string& d : {dirOf(iniPath), string()}) {
            string f = d + "gamecontrollerdb.txt";
            FILE* t = fopen(f.c_str(), "rb"); if (!t) continue; fclose(t);
            printf("SDL: loaded %d mappings from %s\n", g_sdl.AddRW(g_sdl.RWFile(f.c_str(), "rb"), 1), f.c_str());
            break;
        }
    }
    if (g_sdl.Update) g_sdl.Update();
    Sleep(150);
    int n = g_sdl.Num();
    for (int i = 0; i < n; i++) {
        SdlDev d; const char* nm = g_sdl.Name(i); d.name = nm ? nm : "?";
        SdlGuid g = g_sdl.DevGuid(i); char buf[64] = ""; g_sdl.GuidStr(g, buf, (int)sizeof(buf)); d.guid = buf;
        d.vid = g.b[4] | (g.b[5] << 8); d.pid = g.b[8] | (g.b[9] << 8);   // SDL joystick GUID layout: vendor at bytes 4-5, product at 8-9
        d.gc = g_sdl.IsGC(i) != 0;
        if (d.gc) {
            char* m = g_sdl.MapIdx ? g_sdl.MapIdx(i) : g_sdl.MapGuid(g);
            if (m) { d.mapping = m; g_sdl.Free(m); }
        }
        out.push_back(d);
    }
    return out;
}

int main(int argc, char** argv) {
    string iniPath = "goginput.ini", sdlPath; bool mon = false, useSdl = true, forceDI = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--monitor")) mon = true;
        else if (!strcmp(argv[i], "--no-sdl")) useSdl = false;
        else if (!strcmp(argv[i], "--sdl-native")) forceDI = false;
        else if (!strcmp(argv[i], "--sdl") && i + 1 < argc) sdlPath = argv[++i];
        else iniPath = argv[i];
    }
    vector<SdlDev> sdev;
    if (useSdl && sdlLoad(sdlPath, iniPath)) sdev = sdlScan(forceDI, iniPath);

    HINSTANCE hi = GetModuleHandle(NULL);
    HWND hw = CreateWindowExA(0, "STATIC", "goginput_remapping_tool_sdl", WS_OVERLAPPED, 0, 0, 1, 1, NULL, NULL, hi, NULL);
    if (FAILED(DirectInput8Create(hi, DIRECTINPUT_VERSION, IID_IDirectInput8A, (void**)&g_di, NULL))) {
        puts("DirectInput8Create failed."); pause(); return 1;
    }
    vector<DevInfo> devs;
    g_di->EnumDevices(DI8DEVCLASS_GAMECTRL, enumDev, &devs, DIEDFL_ATTACHEDONLY);
    if (devs.empty()) {
        puts("No DirectInput game controller found. Connect one and try again.");
        for (auto& d : sdev) printf("  (SDL does see: %s [%s], but games using DirectInput only get pads that DirectInput exposes)\n", d.name.c_str(), d.guid.c_str());
        pause(); return 1;
    }
    size_t pick = 0;
    if (devs.size() > 1) {
        puts("Controllers:");
        for (size_t i = 0; i < devs.size(); i++) printf("  %u) %s   %s\n", (unsigned)i + 1, devs[i].name.c_str(), guidStr(devs[i].prod).c_str());
        printf("Choose [1-%u]: ", (unsigned)devs.size());
        string l; std::getline(std::cin, l);
        int n = atoi(l.c_str()); if (n >= 1 && n <= (int)devs.size()) pick = n - 1;
    }
    const DevInfo& dv = devs[pick];
    if (FAILED(g_di->CreateDevice(dv.inst, &g_dev, NULL))) { puts("CreateDevice failed."); pause(); return 1; }
    g_dev->SetDataFormat(&c_dfDIJoystick2);
    g_dev->SetCooperativeLevel(hw, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    DIDEVCAPS cap; cap.dwSize = sizeof(cap); g_dev->GetCapabilities(&cap);
    g_caps.buttons = (int)cap.dwButtons; g_pov = (int)cap.dwPOVs;
    g_dev->EnumObjects(enumObj, NULL, DIDFT_AXIS);
    g_dev->Acquire();

    string guid = guidStr(dv.prod);
    printf("\nController : %s\nGUID       : %s\nButtons: %d   Hats: %d   Axes:", dv.name.c_str(), guid.c_str(), g_caps.buttons, g_pov);
    for (int a = 0; a < 6; a++) if (g_caps.ax[a]) printf(" %s", AXN[a]);
    puts("");
    const SdlDev* sd = nullptr;
    for (auto& d : sdev) if (d.vid == (dv.prod.Data1 & 0xFFFF) && d.pid == (dv.prod.Data1 >> 16)) { sd = &d; break; }
    vector<Hit> sug(NSTEPS); bool haveSug = false;
    if (sd) {
        printf("SDL        : %s  [%s]\n", sd->name.c_str(), sd->guid.c_str());
        if (sd->gc) { sug = suggest(sd->mapping, g_caps); for (auto& x : sug) if (x.kind) haveSug = true; }
        else puts("             (not in SDL's controller database, so no layout suggestion; a gamecontrollerdb.txt next to the tool can add it)");
    } else if (!sdev.empty()) puts("SDL        : this pad wasn't matched by VID/PID");
    puts("\nLeave the controller untouched for a moment (calibrating)...");
    Sleep(800);
    DIJOYSTATE2 s;
    for (int i = 0; i < 20; i++) { if (!readState(s)) { puts("Can't read the controller."); pause(); return 1; } Sleep(20); }
    for (int a = 0; a < 6; a++) g_rest[a] = axisOf(s, a);

    if (mon) { monitor(); return 0; }

    bool autoMode = false;
    if (haveSug) {
        puts("\nSDL suggests this layout for your pad:");
        for (unsigned i = 0; i < NSTEPS; i++) printf("   %-34s %s\n", steps[i].label, sug[i].kind ? hitName(sug[i]).c_str() : "(none)");
        autoMode = ask("\nUse it as-is, without pressing anything?  (No = press each control; SDL's guess is shown as a hint)", false);
    }
    if (!autoMode) puts("\nFor each prompt, operate the matching control on YOUR pad.  Enter or S = skip (keeps pass-through).");
    vector<Map> raw; bool hatDpad = false; unsigned agree = 0, differ = 0;
    for (unsigned i = 0; i < NSTEPS; i++) {
        const Step& st = steps[i];
        if (st.mode == 2 && hatDpad) continue;
        printf("\n[%u/%u] %s\n", i + 1, NSTEPS, st.label); fflush(stdout);
        Hit h; bool differs = false;
        if (autoMode) {
            if (!sug[i].kind) { puts("   no SDL suggestion; skipped"); continue; }
            h = sug[i];
        } else {
            if (sug[i].kind) printf("   (SDL suggests: %s)\n", hitName(sug[i]).c_str());
            if (waitFor(st.mode, h)) { puts("   skipped"); continue; }
            if (sug[i].kind) { if (sameHit(h, sug[i])) agree++; else { differ++; differs = true; } }
        }
        printf("   -> %s%s\n", applyHit(st, h, raw, hatDpad).c_str(), differs ? "   [differs from SDL]" : "");
    }
    if (!autoMode && (agree || differ)) {
        printf("\nSDL's suggestion matched %u of %u answers.", agree, agree + differ);
        if (differ) printf(" (Differences are fine if you pressed what you meant; e.g. for triggers this tool prefers a pad's digital button over its analog axis.)");
        puts("");
    }

    vector<string> warn;
    vector<Map> fin = finalize(raw, g_caps, warn);
    string def; for (char c : dv.name) { if (isalnum((unsigned char)c)) def += (char)tolower((unsigned char)c); else if (c == ' ' || c == '_' || c == '-') def += '_'; }
    if (def.empty()) def = "my_controller";
    printf("\nSection name [%s]: ", def.c_str()); fflush(stdout);
    string name; std::getline(std::cin, name); name = trim(name); if (name.empty()) name = def;

    vector<string> sec = buildSection(name, guid, fin);
    puts("\n----- generated section -----");
    for (auto& l : sec) puts(l.c_str());
    puts("-----------------------------");
    for (auto& w : warn) printf("warning: %s\n", w.c_str());
    if (fin.empty()) puts("(no remapping needed - the pad already matches the expected layout)");

    bool ok = false;
    vector<string> L = readLines(iniPath, ok);
    if (!ok) {
        string out = name + "_preset.ini";
        printf("\n%s not found. ", iniPath.c_str());
        if (writeLines(out, sec)) printf("Saved the section alone to %s - paste it into goginput.ini.\n", out.c_str());
        else puts("Could not write the preset file either.");
        pause(); return 0;
    }
    int byName = findName(L, name), byGuid = findGuid(L, guid), rep = -1;
    if (byName >= 0) {
        if (!ask("\nA section with this name already exists. Overwrite it?", true)) { puts("Nothing written."); pause(); return 0; }
        rep = byName;
    } else if (byGuid >= 0) {
        string on; header(L[byGuid], &on);
        printf("\nThis GUID already has a built-in section: [%s].\n", on.c_str());
        if (ask("Replace it with the new section?", true)) rep = byGuid;
        else puts("Adding as an extra section; the wrapper may still pick the older one for this GUID.");
    }
    putSection(L, sec, rep);
    string bak = iniPath + ".bak";
    { std::ifstream t(bak.c_str(), std::ios::binary); if (!t.good()) { std::ifstream in(iniPath.c_str(), std::ios::binary); std::ofstream o(bak.c_str(), std::ios::binary); o << in.rdbuf(); } }
    if (writeLines(iniPath, L)) printf("\nUpdated %s (original kept as %s).\n", iniPath.c_str(), bak.c_str());
    else puts("\nFailed to write the ini (read-only or permission denied?).");
    pause();
    return 0;
}
#endif  // WIN_UI

// ============================================================ Linux self-test of the core logic
#ifdef TEST_CORE
int main(int argc, char** argv) {
    if (argc < 3) { puts("usage: test in.ini out.ini"); return 1; }
    Caps c; for (int i = 0; i < 6; i++) c.ax[i] = true; c.buttons = 14;
    vector<Map> raw = {
        {axName(0, 1), axName(0, 1)}, {axName(0, -1), axName(0, -1)}, {axName(1, 1), axName(1, 1)}, {axName(1, -1), axName(1, -1)},
        {axName(2, 1), axName(3, 1)}, {axName(2, -1), axName(3, -1)}, {axName(5, 1), axName(4, 1)}, {axName(5, -1), axName(4, -1)},
        {"pad_button1", "pad_button0"}, {"pad_button2", "pad_button1"}, {"pad_button0", "pad_button2"}, {"pad_button3", "pad_button3"},
        {"pad_button4", "pad_button4"}, {"pad_button5", "pad_button5"}, {"pad_button6", "pad_button6"}, {"pad_button7", "pad_button7"},
        {"pad_button8", "pad_button8"}, {"pad_button9", "pad_button9"}, {"pad_button10", "pad_button10"}, {"pad_button11", "pad_button11"}};
    vector<string> w; auto fin = finalize(raw, c, w);
    string guid = "09CC054C-0000-0000-0000-504944564944";
    auto sec = buildSection("test_ds4", guid, fin);
    for (auto& l : sec) puts(l.c_str());
    bool ok; auto L = readLines(argv[1], ok);
    int bn = findName(L, "test_ds4"), bg = findGuid(L, guid);
    string on; if (bg >= 0) header(L[bg], &on);
    printf("byName=%d byGuid=%d (%s)\n", bn, bg, on.c_str());
    putSection(L, sec, bg);
    writeLines(argv[2], L);

    // SDL-mapping -> section (DualShock 4 DirectInput entry from SDL's controller database)
    const char* ds4 = "030000004c050000cc09000000006800,PS4 Controller,a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b4,leftstick:b10,lefttrigger:a3,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:a4,rightx:a2,righty:a5,start:b9,touchpad:b13,x:b0,y:b3,platform:Windows,";
    const char* x360 = "030000005e0400008e02000000007801,X360,a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftshoulder:b4,leftstick:b8,lefttrigger:+a2,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b9,righttrigger:-a2,rightx:a3,righty:a4~,start:b7,x:b2,y:b3,platform:Windows,";
    for (int t = 0; t < 2; t++) {
        Caps k; int na = t ? 5 : 6; for (int i = 0; i < na; i++) k.ax[i] = true; k.buttons = t ? 10 : 14;
        vector<Hit> sg = suggest(t ? x360 : ds4, k); vector<Map> r2; bool hat = false;
        for (unsigned i = 0; i < NSTEPS; i++) { if (steps[i].mode == 2 && hat) continue; if (sg[i].kind) applyHit(steps[i], sg[i], r2, hat); }
        vector<string> w2; auto f2 = finalize(r2, k, w2);
        printf("\n--- from SDL mapping: %s (hat dpad=%d)\n", t ? "x360 (axes x,y,z,rx,ry; ry inverted)" : "ds4", (int)hat);
        for (auto& l : buildSection(t ? "x360" : "ds4", "GUID", f2)) puts(l.c_str());
    }
    return 0;
}
#endif