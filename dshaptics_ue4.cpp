// dshaptics_ue4.dll - log every sound an Unreal Engine 4 game starts, natively.
//
// UE4's built-in audio sends every one-shot and component sound through one function,
// FAudioDevice::AddNewActiveSound(const FActiveSound&, ...). Sounds started from C++ (attack swings,
// hits, footsteps from anim notifies) never pass through a UFunction, so the UE4SS Lua mod can't see
// them; this hook can. One line per sound is appended to the file the daemon tails:
//     ue:<sound asset name>|<owner name without instance number>
// Names are resolved inside the game with FName::ToString and cached per FName index.
//
// Addresses come from sigs_ue4.txt next to the DLL ("name=pattern" or "name=+rva", first that
// resolves wins). FActiveSound field offsets (Sound, OwnerName) are in the same file:
//     SoundOffset=18   OwnerNameOffset=44   EventFile=C:\Users\Public\dshaptics\events_ue.log
// A sound ending (played out, stopped, its scene skipped) is seen where the engine destroys its FActiveSound:
//     ue_end:<sound asset name>|<owner name>
// only for names containing one of EndFilter=<part>[,<part>...] (a cutscene's sound bed: when it goes, the
// scene is over), and only when ActiveSoundDtor resolves. Without those two lines nothing changes.
// The daemon writes this file from the "native" section of the game's profile json before injecting,
// so a new game is a json change, not a rebuild (tools/find_ue4_audio.py fills that section from the exe).
// Loaded by the daemon (CreateRemoteThread + LoadLibraryW). Hooks via MinHook.

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "MinHook.h"
#include "rumble_tap.h"     // the game's own vibration requests, logged beside our event lines

struct FString { wchar_t* data; int32_t num, max; };
using AddNewActiveSound_t = void(*)(void*, uint8_t*, void*, void*);
using ToString_t = void(*)(const uint32_t* fname, FString* out);

static AddNewActiveSound_t g_add = nullptr;
static ToString_t g_toString = nullptr;
static size_t g_soundOff = 0x18, g_ownerOff = 0x44;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_lock;
static std::unordered_map<uint32_t, std::string> g_names;
using Dtor_t = void(*)(uint8_t*);
static Dtor_t g_dtor = nullptr;
static std::vector<std::string> g_endFilter;
static std::vector<uint8_t*> g_temps;       // FActiveSounds that are only the caller's copy (guarded by g_lock)
// Where a sound is: FActiveSound::Transform's translation (3 floats, cm). LocationOffset=<hex> in the sigs file turns
// "ue:<name>|<owner>" into "ue:<name>|<owner>|<x>,<y>,<z>" for sounds that have a place (2D sounds: no third field).
// Finding that offset in a new build: Dump=<hex bytes> + DumpFilter=<part>[,<part>...] write the first bytes of
// matching FActiveSounds as hex to as_dump.log next to the event file (at most 300 per filter part).
static size_t g_locOff = 0, g_dumpLen = 0;
static std::vector<std::string> g_dumpFilter;
static std::vector<int> g_dumpLeft;
static HANDLE g_dump = INVALID_HANDLE_VALUE;

static bool copy_bytes(const uint8_t* p, uint8_t* out, size_t n) {
    __try { memcpy(out, p, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void dump_sound(const uint8_t* as, const std::string& head) {
    static uint8_t raw[0x800]; static char hex[0x1000 + 4];     // only under g_lock
    size_t n = g_dumpLen < sizeof raw ? g_dumpLen : sizeof raw;
    EnterCriticalSection(&g_lock);
    if (copy_bytes(as, raw, n)) {
        for (size_t i = 0; i < n; i++) sprintf(hex + 2 * i, "%02x", raw[i]);
        std::string line = head + " " + std::string(hex, 2 * n) + "\n";
        DWORD w; WriteFile(g_dump, line.c_str(), (DWORD)line.size(), &w, nullptr);
    }
    LeaveCriticalSection(&g_lock);
}

static void log_line(const char* s, int n) {
    if (g_log == INVALID_HANDLE_VALUE || n <= 0) return;
    DWORD w; WriteFile(g_log, s, (DWORD)n, &w, nullptr);   // FILE_APPEND_DATA: each line lands whole
}

// FName (ComparisonIndex, Number=0) -> name. The FString buffer is the engine's and is never freed;
// that is one small allocation per distinct name, which the cache keeps to a few thousand at most.
static bool call_to_string(const uint32_t* fn, FString* s) {
    __try { g_toString(fn, s); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static std::string name_of(uint32_t cmp) {
    if (!cmp || !g_toString) return "";
    EnterCriticalSection(&g_lock);
    auto it = g_names.find(cmp);
    if (it != g_names.end()) { std::string r = it->second; LeaveCriticalSection(&g_lock); return r; }
    LeaveCriticalSection(&g_lock);
    uint32_t fn[2] = { cmp, 0 };
    FString s{};
    std::string out;
    if (!call_to_string(fn, &s)) s.data = nullptr;
    if (s.data)
        for (int i = 0; i < s.num && s.data[i]; i++) out += (char)(s.data[i] < 128 ? s.data[i] : '?');
    EnterCriticalSection(&g_lock);
    g_names[cmp] = out;
    LeaveCriticalSection(&g_lock);
    return out;
}

static bool read_ids(uint8_t* as, uint32_t* sound, uint32_t* owner) {
    __try {
        uint8_t* snd = *(uint8_t**)(as + g_soundOff);
        *sound = snd ? *(uint32_t*)(snd + 0x18) : 0;      // UObject::NamePrivate
        *owner = *(uint32_t*)(as + g_ownerOff);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// name already resolved at the sound's start, or "": no engine call while an object is being destroyed
static std::string cached_name(uint32_t cmp) {
    std::string r;
    EnterCriticalSection(&g_lock);
    auto it = g_names.find(cmp);
    if (it != g_names.end()) r = it->second;
    LeaveCriticalSection(&g_lock);
    return r;
}

// The engine keeps its own heap copy of each started sound and destroys it when the sound is over. The copies
// handed to AddNewActiveSound (the caller's stack object, the one carried to the audio thread) die right after
// the call: those are remembered in g_temps and skipped here.
static void hk_ActiveSoundDtor(uint8_t* as) {
    bool temp = false;
    EnterCriticalSection(&g_lock);
    for (size_t i = 0; i < g_temps.size(); i++)
        if (g_temps[i] == as) { g_temps.erase(g_temps.begin() + i); temp = true; break; }
    LeaveCriticalSection(&g_lock);
    uint32_t sound = 0, owner = 0;
    if (!temp && as && read_ids(as, &sound, &owner) && sound) {
        std::string n = cached_name(sound);
        bool want = false;
        for (auto& f : g_endFilter) if (!n.empty() && n.find(f) != std::string::npos) { want = true; break; }
        if (want) {
            std::string line = "ue_end:" + n + "|" + cached_name(owner) + "\n";
            log_line(line.c_str(), (int)line.size());
        }
    }
    g_dtor(as);
}

static void hk_AddNewActiveSound(void* dev, uint8_t* as, void* a3, void* a4) {
    uint32_t sound = 0, owner = 0;
    if (as && g_dtor) {
        EnterCriticalSection(&g_lock);
        if (g_temps.size() > 256) g_temps.erase(g_temps.begin(), g_temps.begin() + 128);   // never destroyed ones
        g_temps.push_back(as);
        LeaveCriticalSection(&g_lock);
    }
    if (as && read_ids(as, &sound, &owner) && sound) {
        std::string nm = name_of(sound), line = "ue:" + nm + "|" + name_of(owner);
        float loc[3];
        if (g_locOff && copy_bytes(as + g_locOff, (uint8_t*)loc, sizeof loc)
            && loc[0] == loc[0] && loc[1] == loc[1] && loc[2] == loc[2]                      // not NaN
            && (loc[0] != 0 || loc[1] != 0 || loc[2] != 0)
            && loc[0] > -1e7f && loc[0] < 1e7f && loc[1] > -1e7f && loc[1] < 1e7f && loc[2] > -1e7f && loc[2] < 1e7f) {
            char b[64]; sprintf(b, "|%d,%d,%d", (int)loc[0], (int)loc[1], (int)loc[2]);
            line += b;
        }
        if (g_dumpLen && g_dump != INVALID_HANDLE_VALUE)
            for (size_t i = 0; i < g_dumpFilter.size(); i++)
                if (nm.find(g_dumpFilter[i]) != std::string::npos) {
                    if (g_dumpLeft[i] > 0) { g_dumpLeft[i]--; dump_sound(as, line); }
                    break;
                }
        line += "\n";
        log_line(line.c_str(), (int)line.size());
        rumble_tap::note(line.c_str(), (int)line.size());
    }
    g_add(dev, as, a3, a4);
}

// ---------------------------------------------------------------- pattern scan
static std::vector<int> parse_pattern(const std::string& p) {
    std::vector<int> out;
    for (size_t i = 0; i < p.size();) {
        if (p[i] == ' ') { i++; continue; }
        if (p[i] == '?') { out.push_back(-1); i += (i + 1 < p.size() && p[i + 1] == '?') ? 2 : 1; continue; }
        out.push_back(std::stoi(p.substr(i, 2), nullptr, 16)); i += 2;
    }
    return out;
}

// The first place the pattern matches; *hits = how many places match (counted up to 2: a pattern is meant
// to match exactly once, more means it may have caught something else - logged, the first is still used).
static uint8_t* scan(const std::vector<int>& pat, int* hits) {
    auto base = (uint8_t*)GetModuleHandleW(nullptr);
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* first = nullptr;
    *hits = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* p = base + sec->VirtualAddress;
        size_t n = sec->Misc.VirtualSize;
        for (size_t i = 0; i + pat.size() <= n; i++) {
            size_t k = 0;
            while (k < pat.size() && (pat[k] < 0 || p[i + k] == pat[k])) k++;
            if (k == pat.size()) { if (!first) first = p + i; if (++*hits == 2) return first; }
        }
    }
    return first;
}

// The running exe's build = the link time in its PE header. A fixed address ("+<rva>") is right for one
// build only: after a game update it points into some other code, and hooking there crashes the game.
static unsigned long exe_build() {
    auto base = (uint8_t*)GetModuleHandleW(nullptr);
    return ((IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew))->FileHeader.TimeDateStamp;
}

static std::string dll_dir(HMODULE self) {
    char path[MAX_PATH]; GetModuleFileNameA(self, path, MAX_PATH);
    std::string s(path); return s.substr(0, s.find_last_of("\\/") + 1);
}

static DWORD WINAPI init(LPVOID self) {
    InitializeCriticalSection(&g_lock);
    std::string dir = dll_dir((HMODULE)self);
    std::string eventFile = "C:\\Users\\Public\\dshaptics\\events_ue.log";
    bool mh = false;
    // Loaded while the game is still starting, the functions may not be found or not be hookable yet
    // (Wukong's DLL, 2026-10-06: "hook failed" 21 s after launch, and it never tried again). So the whole
    // look-up is repeated every 3 s for 5 minutes; the status file is rewritten on every attempt.
    for (int attempt = 1; attempt <= 100 && !g_add; attempt++) {
        if (attempt > 1) Sleep(3000);
        FILE* f = fopen((dir + "sigs_ue4.txt").c_str(), "r");
        FILE* status = fopen((dir + "dshaptics_ue4.log").c_str(), "w");
        if (status) fprintf(status, "attempt %d\n", attempt);
        uint8_t* addAt = nullptr; uint8_t* dtorAt = nullptr;
        char line[1024];
        unsigned long build = 0;                     // "Build=<hex>": the exe the fixed addresses were taken from
        while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "Build=", 6)) build = strtoul(line + 6, nullptr, 16);
        if (f) rewind(f);
        while (f && fgets(line, sizeof line, f)) {
            std::string l(line);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            auto eq = l.find('=');
            if (l.empty() || l[0] == '#' || eq == std::string::npos) continue;
            std::string name = l.substr(0, eq), val = l.substr(eq + 1);
            if (val.empty()) continue;
            if (name == "SoundOffset") { g_soundOff = strtoull(val.c_str(), nullptr, 16); continue; }
            if (name == "OwnerNameOffset") { g_ownerOff = strtoull(val.c_str(), nullptr, 16); continue; }
            if (name == "EventFile") { eventFile = val; continue; }
            if (name == "Build") continue;
            if (name == "LocationOffset") { g_locOff = strtoull(val.c_str(), nullptr, 16); continue; }
            if (name == "Dump") { g_dumpLen = strtoull(val.c_str(), nullptr, 16); continue; }
            if (name == "DumpFilter") {
                g_dumpFilter.clear();
                for (size_t a = 0; a <= val.size();) {
                    size_t b = val.find(',', a); if (b == std::string::npos) b = val.size();
                    if (b > a) g_dumpFilter.push_back(val.substr(a, b - a));
                    a = b + 1;
                }
                g_dumpLeft.assign(g_dumpFilter.size(), 300);
                continue;
            }
            if (name == "EndFilter") {
                g_endFilter.clear();
                for (size_t a = 0; a <= val.size();) {
                    size_t b = val.find(',', a); if (b == std::string::npos) b = val.size();
                    if (b > a) g_endFilter.push_back(val.substr(a, b - a));
                    a = b + 1;
                }
                continue;
            }
            // "+<hex rva>" = fixed address, used only when the exe is the build named on the Build= line
            if (val[0] == '+' && build != exe_build()) {
                if (status) fprintf(status, "%s: fixed address skipped (taken from build %08lx, this exe is %08lx)\n",
                                    name.c_str(), build, exe_build());
                continue;
            }
            int hits = 1;
            uint8_t* at = val[0] == '+' ? (uint8_t*)GetModuleHandleW(nullptr) + strtoull(val.c_str() + 1, nullptr, 16)
                                        : scan(parse_pattern(val), &hits);
            if (!at) { if (status) fprintf(status, "%s: not found (%s)\n", name.c_str(), val.c_str()); continue; }
            if (hits > 1 && status) fprintf(status, "%s: pattern matches more than one place - the first is used\n", name.c_str());
            if (name == "AddNewActiveSound" && !addAt) addAt = at;
            if (name == "FNameToString" && !g_toString) g_toString = (ToString_t)at;
            if (name == "ActiveSoundDtor" && !dtorAt) dtorAt = at;
            if (status) fprintf(status, "%s: %p\n", name.c_str(), at);
        }
        if (f) fclose(f);
        if (g_log == INVALID_HANDLE_VALUE || !g_log)
            g_log = CreateFileA(eventFile.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_ALWAYS, 0, nullptr);
        if (g_dumpLen && !g_dumpFilter.empty() && g_dump == INVALID_HANDLE_VALUE)
            g_dump = CreateFileA((eventFile.substr(0, eventFile.find_last_of("\\/") + 1) + "as_dump.log").c_str(), FILE_APPEND_DATA,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, 0, nullptr);
        if (!mh) mh = MH_Initialize() == MH_OK;
        if (mh) rumble_tap::start(eventFile.substr(0, eventFile.find_last_of("\\/") + 1));   // same folder as the event file
        MH_STATUS st = MH_ERROR_NOT_INITIALIZED;
        if (mh && g_toString && addAt) {
            void* tramp = nullptr;
            if (dtorAt && !g_endFilter.empty() && !g_dtor) {     // first: g_temps must be filling before sounds end
                void* dt = nullptr;
                if (MH_CreateHook(dtorAt, hk_ActiveSoundDtor, &dt) == MH_OK) {
                    g_dtor = (Dtor_t)dt;
                    if (MH_EnableHook(dtorAt) != MH_OK) { MH_RemoveHook(dtorAt); g_dtor = nullptr; }
                }
            }
            st = MH_CreateHook(addAt, hk_AddNewActiveSound, &tramp);
            if (st == MH_OK) {
                g_add = (decltype(g_add))tramp;              // set before enabling: the hook may fire at once
                st = MH_EnableHook(addAt);
                if (st != MH_OK) { MH_RemoveHook(addAt); g_add = nullptr; }
            }
        }
        if (status) {
            fprintf(status, "events -> %s\nAddNewActiveSound %s%s\n", eventFile.c_str(), g_add ? "hooked" : "NOT hooked: ",
                    g_add ? "" : (!g_toString || !addAt) ? "addresses not found" : MH_StatusToString(st));
            fprintf(status, "ActiveSoundDtor %s (%d end filters)\n", g_dtor ? "hooked" : "not hooked", (int)g_endFilter.size());
            fclose(status);
        }
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, init, h, 0, nullptr);   // no heavy work under the loader lock
    }
    return TRUE;
}
