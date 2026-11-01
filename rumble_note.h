// rumble_note.h - our event lines also go into the game's vibration log (rumble_<exe>.log), with its clock: which
// sound was playing when the game shook the pad. The vibration itself is sonmat_rumble.dll's (the loader part), which
// every game we connect loads; without it (an older loader) the lines simply go nowhere.
#pragma once
#include <windows.h>

namespace rumble_tap {
inline void note(const char* s, int n) {
    using Note = void (*)(const char*, int);
    static Note f = nullptr;
    if (!f) {
        HMODULE m = GetModuleHandleA(sizeof(void*) == 8 ? "sonmat_rumble.dll" : "sonmat_rumble_x86.dll");
        if (m) f = (Note)GetProcAddress(m, "sonmat_rumble_note");
    }
    if (f && n > 0) f(s, n);
}
}
