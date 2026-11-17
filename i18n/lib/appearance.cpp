// SPDX-License-Identifier: GPL-3.0-or-later
//
// Settings > Appearance: the UI colour theme and the start screen (the boot logo), picked in the
// Settings app (settings.cpp shows the pages) and kept over the car's coding.
//
// The choice: /storage/data/data/aa/appearance, 32 bytes, "AAAPPR1 T=<theme> L=<logo>" padded
// with spaces, then a newline. <theme> is auto, amber, red or blue; <logo> auto or 0..11.
// Missing or not valid: auto for both, which is the stock behaviour. Written by Settings, and by
// aa_run.sh for the kill switch (aa-appearance-off, AA_APPEARANCE=0).
//
// The colour (stock): one RGB in persist items 114/115/116 (SVPPersist::SetUInt8), which every
// app tints its UI with (FillColor on ROOT) at its next activation, or at once on message 0x206.
// Only the launcher writes it: SVPRmtCan::InitSkinType at every launcher start, from the coding's
// HMI skin (2 red, 3 blue, else amber) or, on MQB with ambient lighting, from the car (BAP id 42,
// OnBapScreenViewDataEvent). Here, in the launcher, when each function is the known code
// (AA_LAUNCHER, from i18n/gen_launcher.py): InitSkinType writes the chosen colour instead, and
// OnBapScreenViewDataEvent drops id 42, while a colour is chosen; on auto both run as stock.
// Settings applies a choice at once (aa_theme_select).
//
// The logo (stock): rvc_os, which init starts before anything is mounted, shows
// /usr/share/rvc/logo/<one of 12 files>, picked by an index in NOR (/dev/mtd3 at 0x90000, the
// record {logo, exlap} of svapi_exlap_logo_get/set in libsvapi_vw). canservice writes the
// coding's start screen there whenever a coding message differs from it. Settings writes the
// chosen index once (aa_logo_select); a thread in the launcher writes it back when canservice has
// changed it, at most kLogoWrites times per boot, as each write erases a NOR sector. Every index
// is clamped to 0..11: rvc_os reads a table with it, unchecked below 0, and it is also the
// reverse camera's process.
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "aa_lang.h"

namespace {

const char* const kState = "/storage/data/data/aa/appearance";
const char* const kStateDir = "/storage/data/data/aa";
const int kRecord = 32;

struct Theme {
    const char* key;   // in the state file
    const char* text;  // aa_text key of its name
    uint8_t r, g, b;
};
// Automatic, then the launcher's three (SetDescSkinType's table), then green (ours).
const Theme kThemes[] = {
    {"auto", "auto", 0, 0, 0},
    {"amber", "theme_amber", 255, 190, 0},
    {"red", "theme_red", 233, 0, 0},
    {"blue", "theme_blue", 30, 40, 255},
    {"green", "theme_green", 75, 168, 46},  // Skoda Green (#4BA82E); not a coding choice
};
const int kThemeCount = sizeof kThemes / sizeof kThemes[0];

// rvc_os's Logo_Name_Array, in its order: the index the NOR record holds. Brand names stay as
// they are; "Standard" ("Welcome to Volkswagen") is a text of the sheet.
const char* const kLogos[] = {"logo_standard", "Twin Drive", "GTD", "GTI", "BlueMotion", "e-Golf",
                              "R-Line", "R", "Alltrack", "GTE", "Beats Audio", "Dynaudio"};
const int kLogoCount = sizeof kLogos / sizeof kLogos[0];
const int kLogoWrites = 3;  // per boot, by the launcher's thread

struct State {
    int theme;  // index in kThemes, 0 auto
    int logo;   // -1 auto, else 0..kLogoCount-1
};

#ifdef AA_TEXTS
struct OwnText {
    const char* key;
    uint32_t entry;  // literals.entries() index in aa_lang.bin
    const char* en;
    const char* zh;  // "" where the sheet has none
};
#include AA_TEXTS
#else
struct OwnText {
    const char* key;
    uint32_t entry;
    const char* en;
    const char* zh;
};
const OwnText kOwnTexts[1] = {{"", 0, "", ""}};
#endif

bool parse(const char* r, State& s) {
    s.theme = 0;
    s.logo = -1;
    if (memcmp(r, "AAAPPR1 ", 8) != 0 || r[kRecord - 1] != '\n')
        return false;
    char buf[kRecord];
    memcpy(buf, r, kRecord);
    buf[kRecord - 1] = 0;
    char* t = strstr(buf, " T=");
    char* l = strstr(buf, " L=");
    if (!t || !l)
        return false;
    t += 3;
    l += 3;
    for (int i = 0; i < kThemeCount; ++i) {
        size_t n = strlen(kThemes[i].key);
        if (strncmp(t, kThemes[i].key, n) == 0 && (t[n] == ' ' || t[n] == 0))
            s.theme = i;
    }
    if (strncmp(l, "auto", 4) != 0) {
        char* end = 0;
        long v = strtol(l, &end, 10);
        if (end != l && v >= 0 && v < kLogoCount && (*end == ' ' || *end == 0))
            s.logo = static_cast<int>(v);
    }
    return true;
}

State read_state() {
    State s = {0, -1};
    char r[kRecord];
    int fd = open(kState, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return s;
    ssize_t n = pread(fd, r, kRecord, 0);
    close(fd);
    if (n != kRecord || !parse(r, s)) {
        s.theme = 0;
        s.logo = -1;
    }
    return s;
}

int write_state(const State& s) {
    char logo[8];
    if (s.logo < 0)
        strcpy(logo, "auto");
    else
        snprintf(logo, sizeof logo, "%d", s.logo);
    char r[kRecord + 1];
    int n = snprintf(r, sizeof r, "AAAPPR1 T=%s L=%s", kThemes[s.theme].key, logo);
    if (n < 0 || n >= kRecord)
        return -1;
    memset(r + n, ' ', kRecord - 1 - n);
    r[kRecord - 1] = '\n';
    mkdir(kStateDir, 0755);
    int fd = open(kState, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    int ok = pwrite(fd, r, kRecord, 0) == kRecord && fsync(fd) == 0;
    close(fd);
    return ok ? 0 : -1;
}

// ---- the platform's calls, found when needed (no Kanzi) ----

typedef void (*SetUInt8)(int, unsigned char);
typedef void (*ChangeAmbient)(unsigned char);
typedef int (*LogoRecord)(int*);
typedef void* (*CanInstance)();
typedef int (*CanGetByte)(void*, unsigned char&);

template <typename F>
F sym(const char* name) {
    return reinterpret_cast<F>(dlsym(RTLD_DEFAULT, name));
}

// The colour in persist, as SetDescSkinType writes it, and announced.
bool write_colour(uint8_t r, uint8_t g, uint8_t b) {
    SetUInt8 set = sym<SetUInt8>("_ZN10SVPPersist8SetUInt8ENS_14InfoTypeUint8_Eh");
    ChangeAmbient announce = sym<ChangeAmbient>("_Z23SVP_ChangeAmbientColourh");
    if (!set || !announce)
        return false;
    set(114, r);
    set(115, g);
    set(116, b);
    announce(0);
    return true;
}

// The coding's byte through Settings' or the launcher's canservice client (0 if not known).
unsigned char coded(const char* getter) {
    CanInstance instance = sym<CanInstance>("_ZN12SVPSingletonI20SVPCanService_ClientE11GetInstanceEv");
    CanGetByte get = sym<CanGetByte>(getter);
    unsigned char v = 0;
    void* client = instance ? instance() : 0;
    if (client && get)
        get(client, v);
    return v;
}

// The theme the coding gives (auto): the launcher's own table.
int coded_theme() {
    unsigned char skin = coded("_ZN20SVPCanService_Client20GetDescCodingHmiSkinERh");
    return skin == 2 ? 2 : skin == 3 ? 3 : 1;
}

int coded_logo() {
    unsigned char v = coded("_ZN20SVPCanService_Client24GetDescCodingStartScreenERh");
    return v < kLogoCount ? v : 0;
}

// The NOR record's logo index (-1 if it cannot be read).
int nor_logo(int rec[2]) {
    LogoRecord get = sym<LogoRecord>("svapi_exlap_logo_get");
    rec[0] = rec[1] = 0;
    if (!get || get(rec) != 0)
        return -1;
    return rec[0];
}

// Writes `logo` (clamped) into NOR when it differs, keeping the exlap half. 1 written, 0 already
// there, -1 failed.
int write_logo(int logo) {
    if (logo < 0 || logo >= kLogoCount)
        return -1;
    LogoRecord set = sym<LogoRecord>("svapi_exlap_logo_set");
    int rec[2];
    int now = nor_logo(rec);
    if (now == logo)
        return 0;
    if (!set || now < 0)
        return -1;
    rec[0] = logo;
    return set(rec) == 0 ? 1 : -1;
}

// ---- the launcher ----

struct LauncherFunc {
    const char* name;
    uint32_t addr, size, fnv;
};
#ifdef AA_LAUNCHER
#include AA_LAUNCHER
#else
const LauncherFunc kLauncherFuncs[1] = {{"", 0, 0, 0}};
#endif

uint32_t fnv(const uint8_t* p, uint32_t n) {
    uint32_t h = 0x811C9DC5u;
    while (n--)
        h = (h ^ *p++) * 0x01000193u;
    return h;
}

const LauncherFunc* launcher_func(const char* name) {
    for (size_t i = 0; i < sizeof kLauncherFuncs / sizeof kLauncherFuncs[0]; ++i)
        if (strcmp(kLauncherFuncs[i].name, name) == 0)
            return &kLauncherFuncs[i];
    return 0;
}

typedef void (*Method0)(void*);
typedef void (*BapEvent)(void*, int, void*);
Method0 g_orig_init_skin;
BapEvent g_orig_bap;

void hook_init_skin(void* self) {
    State s = read_state();
    if (s.theme > 0 && write_colour(kThemes[s.theme].r, kThemes[s.theme].g, kThemes[s.theme].b)) {
        syslog(LOG_INFO, "aa-appearance: launcher: theme %s (instead of the coded one)", kThemes[s.theme].key);
        return;
    }
    g_orig_init_skin(self);
}

void hook_bap(void* self, int id, void* data) {
    if (id == 42 && read_state().theme > 0)
        return;  // the car's ambient colour, while a theme is chosen
    g_orig_bap(self, id, data);
}

// Puts the chosen logo back into NOR after canservice has written the coded one.
void* logo_watch(void*) {
    int writes = 0;
    time_t start = time(0);
    for (;;) {
        sleep(time(0) - start < 180 ? 5 : 60);
        State s = read_state();
        if (s.logo < 0)
            continue;
        int rec[2];
        int now = nor_logo(rec);
        if (now < 0 || now == s.logo)
            continue;
        if (writes >= kLogoWrites) {
            static bool told;
            if (!told) {
                told = true;
                syslog(LOG_WARNING, "aa-appearance: launcher: the start screen was changed back %d times; left to the coding "
                                    "until the next boot", writes);
            }
            continue;
        }
        ++writes;
        int r = write_logo(s.logo);
        syslog(LOG_INFO, "aa-appearance: launcher: start screen %d (NOR had %d): %s", s.logo, now,
               r > 0 ? "written" : "not written");
    }
    return 0;
}

void launcher_init() {
    for (size_t i = 0; i < sizeof kLauncherFuncs / sizeof kLauncherFuncs[0]; ++i) {
        const LauncherFunc& f = kLauncherFuncs[i];
        if (!f.addr || fnv(reinterpret_cast<const uint8_t*>(f.addr), f.size) != f.fnv) {
            syslog(LOG_WARNING, "aa-appearance: launcher: %s is not the known code; the coding decides colour and logo",
                   f.name);
            return;
        }
    }
    const LauncherFunc* init = launcher_func("init_skin");
    const LauncherFunc* bap = launcher_func("bap_view_data");
    if (!init || !bap)
        return;
    g_orig_init_skin = reinterpret_cast<Method0>(aa_hook_trampoline(init->addr));
    g_orig_bap = reinterpret_cast<BapEvent>(aa_hook_trampoline(bap->addr));
    if (!g_orig_init_skin || !g_orig_bap || !aa_hook_jump(init->addr, reinterpret_cast<const void*>(hook_init_skin)) ||
        !aa_hook_jump(bap->addr, reinterpret_cast<const void*>(hook_bap))) {
        syslog(LOG_ERR, "aa-appearance: launcher: patching failed");
        return;
    }
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    if (pthread_create(&t, &attr, logo_watch, 0) != 0)
        syslog(LOG_WARNING, "aa-appearance: launcher: no start screen thread");
    pthread_attr_destroy(&attr);
    State s = read_state();
    syslog(LOG_INFO, "aa-appearance: launcher: theme %s, start screen %d", kThemes[s.theme].key, s.logo);
}

bool is_exe(const char* tail) {
    char exe[128];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return false;
    exe[n] = 0;
    size_t a = strlen(exe), b = strlen(tail);
    return a >= b && strcmp(exe + a - b, tail) == 0;
}

}  // namespace

// ---- for settings.cpp ----

const char* aa_text(const char* key) {
    for (size_t i = 0; i < sizeof kOwnTexts / sizeof kOwnTexts[0]; ++i) {
        const OwnText& t = kOwnTexts[i];
        if (strcmp(t.key, key) != 0)
            continue;
        const char* code = aa_lang_current();
        if (strcmp(code, "zh") == 0)
            return t.zh[0] ? t.zh : t.en;
        if (strcmp(code, "en") != 0 && strcmp(code, "off") != 0) {
            const char* x = aa_lang_lit_text(code, t.entry);
            if (x)
                return x;
        }
        return t.en;
    }
    return key;
}

int aa_theme_count() {
    return kThemeCount;
}

const char* aa_theme_name(int i) {
    return i == 0 ? aa_text("auto") : aa_text(kThemes[i].text);
}

const char* aa_theme_key(int i) {
    return kThemes[i].key;
}

int aa_theme_current() {
    return read_state().theme;
}

// The theme's colour as 0x00RRGGBB; for auto, the coded one.
uint32_t aa_theme_rgb(int i) {
    const Theme& t = kThemes[i > 0 ? i : coded_theme()];
    return (uint32_t(t.r) << 16) | (uint32_t(t.g) << 8) | t.b;
}

int aa_theme_select(int i) {
    if (i < 0 || i >= kThemeCount)
        return -1;
    State s = read_state();
    s.theme = i;
    if (write_state(s) != 0)
        return -1;
    uint32_t rgb = aa_theme_rgb(i);
    if (!write_colour(rgb >> 16, (rgb >> 8) & 0xff, rgb & 0xff))
        return -1;
    syslog(LOG_INFO, "aa-appearance: theme %s (%06x)", kThemes[i].key, rgb);
    return 0;
}

int aa_logo_count() {
    return kLogoCount + 1;  // Automatic first
}

// Item i of the start screen page: 0 Automatic, then the logos in NOR's order.
const char* aa_logo_name(int i) {
    if (i == 0)
        return aa_text("auto");
    const char* n = kLogos[i - 1];
    return strncmp(n, "logo_", 5) == 0 ? aa_text(n) : n;
}

int aa_logo_current() {
    return read_state().logo + 1;
}

int aa_logo_select(int i) {
    if (i < 0 || i > kLogoCount)
        return -1;
    State s = read_state();
    s.logo = i - 1;
    if (write_state(s) != 0)
        return -1;
    int logo = s.logo >= 0 ? s.logo : coded_logo();
    int r = write_logo(logo);
    syslog(LOG_INFO, "aa-appearance: start screen %d%s: %s", logo, s.logo < 0 ? " (the coding's)" : "",
           r > 0 ? "written" : r == 0 ? "already there" : "not written");
    return r < 0 ? -1 : 0;
}

// The library's constructor: the launcher's hooks, in the launcher only.
void aa_appearance_init() {
    if (is_exe("/sv.g5.mib2.launcher/bin/launcher"))
        launcher_init();
}
