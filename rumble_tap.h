// rumble_tap.h - record the vibration the game itself asks for, next to the sounds it plays.
//
// A PC game shakes a pad through XInputSetState(pad, {left motor, right motor}) - a public Windows
// function, the same in every game, so there is no address to find and a game patch does not break it.
// This hooks it in whichever xinput*.dll the game loaded and writes one line per change to
//     <dir>\rumble_<exe>.log      "<unix ms> xr:<pad>:<left 0-65535>:<right 0-65535>"
// The hook DLL passes its own event lines through note() so they land in the same file with the same
// clock ("<unix ms> ue:<sound>|<owner>"): which sound was playing when the game shook the pad, and how
// hard for how long. A maker's log (which moments the original vibrates on) - the daemon does not read it.
// Some games drive a DualSense themselves instead (Lies of P: bUseWinDualSense - rumble, trigger effects and
// light bar straight over HID, nothing through XInput). Those writes are tapped too:
//     "<unix ms> ds:<hex of the output report>"      report 0x02 as the game wrote it, on every change
// (USB layout: byte 3 = small/right motor, 4 = big/left motor, 11-21 = R2 effect, 22-32 = L2 effect, 45-47 = light).
// WriteFile is hooked for this, so only short writes that start with 0x02 are looked at, and only handles
// that HidD_GetAttributes says are a Sony pad (asked once per handle).
// The XInput call is logged as asked and then passed on with both motors at 0: Sonmat's haptics replace the
// original, so the game's vibration option can stay on (for this log, for videos) without the old rumble
// shaking the pad on top (10-31 사용자). The DualSense writes above are passed on untouched (the game's own
// trigger effects and light). MinHook must already be initialised: call start() after it.
#pragma once
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "MinHook.h"

namespace rumble_tap {
struct Vib { WORD left, right; };
using SetState_t = DWORD (WINAPI*)(DWORD, Vib*);
static const char* const kDlls[] = {"xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll", "xinput1_2.dll", "xinput1_1.dll"};
const int N = sizeof kDlls / sizeof *kDlls;
static SetState_t g_orig[N];
static bool g_hooked[N];
static HANDLE g_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_cs;
static bool g_ready = false;
static DWORD g_last[8];

using WriteFile_t = BOOL (WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
static WriteFile_t g_writeFile = nullptr;                    // the real one, once WriteFile is hooked

static void write(const char* s) {
    if (!g_ready || g_file == INVALID_HANDLE_VALUE) return;
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    unsigned long long ms = (((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ULL) / 10000;
    char head[32]; int n = snprintf(head, sizeof head, "%llu ", ms);
    std::string line(head, n); line += s;
    if (line.empty() || line.back() != '\n') line += '\n';
    DWORD w; (g_writeFile ? g_writeFile : WriteFile)(g_file, line.data(), (DWORD)line.size(), &w, nullptr);   // FILE_APPEND_DATA: lands whole
}

// an event line the hook DLL just emitted (with or without its newline)
static void note(const char* s, int n) { if (g_ready && n > 0) write(std::string(s, n).c_str()); }

static void seen(DWORD pad, const Vib* v) {
    if (!v) return;
    DWORD now = (DWORD)v->left << 16 | v->right;
    bool changed = false;
    EnterCriticalSection(&g_cs);
    if (pad < 8 && g_last[pad] != now) { g_last[pad] = now; changed = true; }
    LeaveCriticalSection(&g_cs);
    if (changed) { char b[64]; snprintf(b, sizeof b, "xr:%lu:%u:%u", pad, v->left, v->right); write(b); }
}

template <int I> static DWORD WINAPI hook(DWORD pad, Vib* v) {
    seen(pad, v);
    Vib still = {0, 0};
    return g_orig[I](pad, v ? &still : v);              // logged, not felt (the header)
}
static const SetState_t kHooks[N] = {hook<0>, hook<1>, hook<2>, hook<3>, hook<4>};

// ---- a DualSense driven directly: output reports written to the pad's HID handle
struct HidAttr { ULONG size; USHORT vendor, product, version; };
using GetAttr_t = BOOLEAN (__stdcall*)(HANDLE, HidAttr*);
static GetAttr_t g_getAttr = nullptr;
static HANDLE g_pads[8], g_others[64];
static int g_nPads = 0, g_nOthers = 0;
static unsigned char g_lastReport[64];

static bool is_pad(HANDLE h) {
    bool pad = false, known = false;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_nPads && !known; i++) if (g_pads[i] == h) pad = known = true;
    for (int i = 0; i < g_nOthers && !known; i++) if (g_others[i] == h) known = true;
    LeaveCriticalSection(&g_cs);
    if (known) return pad;
    DWORD type = GetFileType(h);
    HidAttr a = {sizeof a};
    pad = type != FILE_TYPE_DISK && type != FILE_TYPE_PIPE && type != FILE_TYPE_CHAR && g_getAttr && g_getAttr(h, &a) && a.vendor == 0x054C;
    EnterCriticalSection(&g_cs);
    if (pad) { if (g_nPads < 8) g_pads[g_nPads++] = h; }
    else g_others[g_nOthers++ % 64] = h;                     // a ring: a reused handle value is asked again later
    LeaveCriticalSection(&g_cs);
    if (pad) { char b[64]; snprintf(b, sizeof b, "tap:pad handle %04x:%04x", a.vendor, a.product); write(b); }
    return pad;
}

static BOOL WINAPI hk_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD written, LPOVERLAPPED ov) {
    if (buf && n >= 32 && n <= 128 && *(const unsigned char*)buf == 0x02 && h != g_file && is_pad(h)) {
        const unsigned char* p = (const unsigned char*)buf;
        DWORD m = n < 48 ? n : 48;
        bool changed;
        EnterCriticalSection(&g_cs);
        changed = memcmp(g_lastReport, p, m) != 0;
        if (changed) memcpy(g_lastReport, p, m);
        LeaveCriticalSection(&g_cs);
        if (changed) {
            char line[8 + 2 * 48]; int k = snprintf(line, sizeof line, "ds:");
            for (DWORD i = 0; i < m; i++) k += snprintf(line + k, sizeof line - k, "%02x", p[i]);
            write(line);
        }
    }
    return g_writeFile(h, buf, n, written, ov);
}

static void hook_hid() {
    HMODULE hid = LoadLibraryA("hid.dll");
    g_getAttr = hid ? (GetAttr_t)GetProcAddress(hid, "HidD_GetAttributes") : nullptr;
    void* at = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "WriteFile");
    void* tramp = nullptr;
    MH_STATUS st = !g_getAttr ? MH_ERROR_FUNCTION_NOT_FOUND : MH_CreateHook(at, (void*)hk_WriteFile, &tramp);
    if (st == MH_OK) {
        g_writeFile = (WriteFile_t)tramp;                    // set before enabling: the hook fires at once
        st = MH_EnableHook(at);
        if (st != MH_OK) g_writeFile = nullptr;
    }
    char b[96]; snprintf(b, sizeof b, "tap:WriteFile:%s", st == MH_OK ? "hooked" : MH_StatusToString(st));
    write(b);
}

// The game may load its xinput dll late (first pad use), so keep looking.
static DWORD WINAPI watch(LPVOID) {
    hook_hid();
    for (;;) {
        for (int i = 0; i < N; i++) {
            HMODULE m = g_hooked[i] ? nullptr : GetModuleHandleA(kDlls[i]);
            void* at = m ? (void*)GetProcAddress(m, "XInputSetState") : nullptr;
            if (!at) continue;
            void* tramp = nullptr;
            MH_STATUS st = MH_CreateHook(at, (void*)kHooks[i], &tramp);
            if (st == MH_OK) {
                g_orig[i] = (SetState_t)tramp;               // set before enabling: the hook may fire at once
                st = MH_EnableHook(at);
            }
            g_hooked[i] = true;                              // one try per dll, hooked or not
            char b[96]; snprintf(b, sizeof b, "tap:%s:%s", kDlls[i], st == MH_OK ? "hooked" : MH_StatusToString(st));
            write(b);
        }
        Sleep(2000);
    }
}

static void start(const std::string& dir) {
    if (g_ready) return;
    InitializeCriticalSection(&g_cs);
    for (auto& l : g_last) l = 0;                            // "motors off" is the starting state: not worth a line
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string name(exe); name = name.substr(name.find_last_of("\\/") + 1);
    auto dot = name.find_last_of('.'); if (dot != std::string::npos) name.resize(dot);
    g_file = CreateFileA((dir + "rumble_" + name + ".log").c_str(), FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, 0, nullptr);
    g_ready = true;
    write(("tap:start:" + name).c_str());
    CreateThread(nullptr, 0, watch, nullptr, 0, nullptr);
}
}
