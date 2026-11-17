// SPDX-License-Identifier: GPL-3.0-or-later
//
// Station names on the FM preset tiles of the radio's main screen, in place of "88.3 MHz".
//
// Stock (radio, addresses per build in AA_RADIO, from i18n/gen_radio.py): CRadioMainDlg::
// UpdateFMPreset fills the 15 tiles, each a PresetListDataExchanger (list at this+108, its item i
// from ISkListBox-like vtable slot 392, cast with __dynamic_cast from
// SkObjectGeneratorItemDataExchanger), with the preset's frequency (CRadioDataMgr::
// GetCurrentPresetByIndex, a static: 10 kHz units) through SetTextFreq ("88.3") and SetTextUnit
// ("MHz"). The tuner tells the app the station's RDS name (PS, 8 characters); the main screen reads
// it in the UI thread through CRadioDataMgr::GetPsName(char*) while the band (m_CurBand) is FM (1)
// and the frequency is m_FMFreq.
//
// Here, in the radio process only, when every function below has exactly the code of a known build
// (else the presets stay as they are), and not with the language "off" (the aa-lang-off card):
//   GetPsName  wrapped: after the stock code, the name is counted for the frequency playing. Many
//              stations send changing texts in PS (song titles); the name a frequency sent most
//              often is its name. Names are kept in AA_RADIO_STATE ("<freq> <name>" lines), written
//              when a frequency's name changes, and read at start.
//   UpdateFMPreset  wrapped: after the stock code, a tile whose frequency has a name shows the name
//              in the tile's centred layout (below); the others stay as they are. The tiles show a new
//              name the next time the app refreshes them (a station change, a preset stored, a bank).
//   PresetListDataExchanger::OnUpdateItemCtrl  wrapped: it sets a tile's Kanzi nodes from its data.
//              A tile has two layouts, its SwitchState (+60): 1, a station (Text_Band, the frequency
//              (+48), 1.5 wide and left of the centre, with Text_Unit (+44) to its right; the state
//              places them, moving them does not hold) and 0, empty (Text_Band 1.7 wide, centred, with
//              the label +56, "Empty"). A tile with a name is put in layout 0 with the name as its label,
//              and the name also in +48 (hidden there; it tells it from an empty tile). Its Text_Band
//              is made 3.2 wide (the tile is about 3.4), so the whole name shows centred, at the
//              frequency's size; an empty tile gets the stock width back (the list reuses the nodes).
//              The layout is only the look: the dialog handles taps from the preset data. Without the
//              Kanzi calls it needs, the name goes in both station fields instead: its first word in
//              the frequency field, the rest in the unit's.
// Only printable ASCII names are kept (PS uses the RDS character set, not UTF-8).
#include <cxxabi.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include <string>

#include "aa_lang.h"

#ifndef AA_RADIO_STATE
#define AA_RADIO_STATE "/storage/data/data/aa/stations"
#endif

namespace {

struct RadioFunc {
    const char* name;
    uint32_t addr, size, fnv;
};
#ifdef AA_RADIO
#include AA_RADIO
#else
const RadioFunc kRadioFuncs[1] = {{"", 0, 0, 0}};
#define AA_RADIO_FM_FREQ 0
#define AA_RADIO_CUR_BAND 0
#define AA_RADIO_ITEM_TI 0
#endif

const int kPresets = 15;   // UpdateFMPreset's loop
const int kStations = 64;  // frequencies remembered
const int kNames = 6;      // names counted per frequency
const int kKnown = 4;      // the count a name read from the file starts with

struct Name {
    char text[9];
    uint16_t count;
};
struct Station {
    uint16_t freq;  // 0: free
    Name names[kNames];
};
Station g_stations[kStations];

typedef void (*Method0)(void*);
typedef void (*GetPs)(void*, char*);
typedef unsigned short (*PresetFreq)(unsigned short);
typedef void (*SetText)(void*, const char*);
typedef void (*ItemCtrl)(void*, int, void*);
Method0 g_orig_update;
GetPs g_orig_ps;
PresetFreq g_preset_freq;
SetText g_set_freq, g_set_unit;
ItemCtrl g_orig_item;
const abi::__class_type_info* g_item_base;

// Kanzi (libUICore-kanzi), for the wide name field; g_wide only when every call is there.
struct Kanzi {
    unsigned (*child_count)(void*);
    void* (*child_at)(void*, unsigned);
    const char* (*get_name)(void*);
    int (*get_float)(void*, void*, float*);
    int (*set_float)(void*, void*, float);
    void (*invalidate_layout)(void*);
    void** layout_width;      // KZU_PROPERTY_TYPE_LAYOUT_WIDTH
} g_kz;
bool g_wide;
const float kWideWidth = 3.2f;
float g_stock_width;  // an empty tile's Text_Band width, as first seen
bool g_stock_known;

uint32_t fnv(const uint8_t* p, uint32_t n) {
    uint32_t h = 0x811C9DC5u;
    while (n--)
        h = (h ^ *p++) * 0x01000193u;
    return h;
}

const RadioFunc* func(const char* name) {
    for (size_t i = 0; i < sizeof kRadioFuncs / sizeof kRadioFuncs[0]; ++i)
        if (strcmp(kRadioFuncs[i].name, name) == 0)
            return &kRadioFuncs[i];
    return 0;
}

Station* station(uint16_t freq, bool make) {
    Station* free_slot = 0;
    for (int i = 0; i < kStations; ++i) {
        if (g_stations[i].freq == freq)
            return &g_stations[i];
        if (!g_stations[i].freq && !free_slot)
            free_slot = &g_stations[i];
    }
    if (make && free_slot) {
        memset(free_slot, 0, sizeof *free_slot);
        free_slot->freq = freq;
    }
    return make ? free_slot : 0;
}

const char* best(const Station* s) {
    const Name* b = 0;
    for (int i = 0; i < kNames; ++i)
        if (s->names[i].count && (!b || s->names[i].count > b->count))
            b = &s->names[i];
    return b ? b->text : 0;
}

// The PS bytes as a name: printable ASCII, outer spaces trimmed; false if nothing is left or a byte
// is not printable ASCII.
bool clean(const char* ps, char out[9]) {
    int a = 0, b = 8;
    while (a < 8 && ps[a] == ' ')
        ++a;
    while (b > a && (ps[b - 1] == ' ' || ps[b - 1] == 0))
        --b;
    if (a == b)
        return false;
    for (int i = a; i < b; ++i)
        if (ps[i] < 0x20 || ps[i] > 0x7e)
            return false;
    memcpy(out, ps + a, b - a);
    out[b - a] = 0;
    return true;
}

void count(Station* s, const char* name, uint16_t n) {
    Name* low = &s->names[0];
    for (int i = 0; i < kNames; ++i) {
        if (s->names[i].count && strcmp(s->names[i].text, name) == 0) {
            if (s->names[i].count < 60000)
                s->names[i].count += n;
            return;
        }
        if (s->names[i].count < low->count)
            low = &s->names[i];
    }
    strcpy(low->text, name);  // a new name takes the least seen one's place
    low->count = n;
}

void save() {
    const char* tmp = AA_RADIO_STATE ".tmp";
    FILE* f = fopen(tmp, "w");
    if (!f)
        return;
    for (int i = 0; i < kStations; ++i) {
        const char* n = g_stations[i].freq ? best(&g_stations[i]) : 0;
        if (n)
            fprintf(f, "%u %s\n", g_stations[i].freq, n);
    }
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    if (ok)
        rename(tmp, AA_RADIO_STATE);
}

void load() {
    FILE* f = fopen(AA_RADIO_STATE, "r");
    if (!f)
        return;
    char line[64];
    while (fgets(line, sizeof line, f)) {
        unsigned freq = 0;
        int at = 0;
        if (sscanf(line, "%u %n", &freq, &at) != 1 || !freq || freq > 65535 || !at)
            continue;
        char ps[8];
        memset(ps, ' ', sizeof ps);
        size_t n = strcspn(line + at, "\r\n");
        memcpy(ps, line + at, n < 8 ? n : 8);
        char name[9];
        Station* s = clean(ps, name) ? station(freq, true) : 0;
        if (s)
            count(s, name, kKnown);
    }
    fclose(f);
}

void hook_get_ps(void* self, char* out) {
    g_orig_ps(self, out);
    if (*reinterpret_cast<const int*>(AA_RADIO_CUR_BAND) != 1)
        return;
    uint16_t freq = *reinterpret_cast<const uint16_t*>(AA_RADIO_FM_FREQ);
    char name[9];
    Station* s = freq && clean(out, name) ? station(freq, true) : 0;
    if (!s)
        return;
    const char* before = best(s);
    char was[9] = "";
    if (before)
        strcpy(was, before);
    count(s, name, 1);
    if (strcmp(was, best(s)) != 0) {
        syslog(LOG_INFO, "aa-radio: %u.%u MHz is \"%s\"", freq / 100, freq / 10 % 10, best(s));
        save();
    }
}

void hook_update_fm(void* self) {
    g_orig_update(self);
    void* list = *reinterpret_cast<void**>(static_cast<char*>(self) + 108);
    if (!list)
        return;
    typedef void* (*Item)(void*, int);
    Item item = reinterpret_cast<Item>((*reinterpret_cast<void***>(list))[392 / 4]);
    for (int i = 0; i < kPresets; ++i) {
        unsigned short freq = g_preset_freq(i);
        if (freq < 8750 || freq > 10800)  // an empty preset ("Empty") or not an FM frequency
            continue;
        const Station* s = station(freq, false);
        const char* name = s ? best(s) : 0;
        void* base = name ? item(list, i) : 0;
        void* tile = base ? abi::__dynamic_cast(base, g_item_base,
                                                reinterpret_cast<const abi::__class_type_info*>(AA_RADIO_ITEM_TI), 0)
                          : 0;
        if (!tile)
            continue;
        if (g_wide) {  // the centred layout with the name as its label, which hook_item_ctrl widens
            reinterpret_cast<std::string*>(static_cast<char*>(tile) + 56)->assign(name);
            *reinterpret_cast<uint16_t*>(static_cast<char*>(tile) + 60) = 0;
            g_set_unit(tile, "");
            g_set_freq(tile, name);  // both setters call the item's Update()
            continue;
        }
        // Without it: the name in the tile's two fields, as "88.3" and "MHz" are, its first word in
        // the frequency field (about 5 characters wide), the rest in the unit's.
        const char* sp = strchr(name, ' ');
        std::string first(name, sp ? sp - name : strlen(name));
        g_set_freq(tile, first.c_str());
        g_set_unit(tile, sp ? sp + 1 : "");
    }
}

// A child named `name` at most `depth` levels below `node`, or 0.
void* find(void* node, const char* name, int depth) {
    unsigned n = g_kz.child_count(node);
    for (unsigned i = 0; i < n; ++i) {
        void* c = g_kz.child_at(node, i);
        const char* cn = c ? g_kz.get_name(c) : 0;
        if (cn && strcmp(cn, name) == 0)
            return c;
    }
    for (unsigned i = 0; depth > 0 && i < n; ++i) {
        void* c = g_kz.child_at(node, i);
        void* f = c ? find(c, name, depth - 1) : 0;
        if (f)
            return f;
    }
    return 0;
}

void hook_item_ctrl(void* self, int index, void* ctrl) {
    g_orig_item(self, index, ctrl);
    if (!g_wide || !ctrl)
        return;
    char* d = static_cast<char*>(self);
    bool named = *reinterpret_cast<uint16_t*>(d + 60) == 0 && !reinterpret_cast<std::string*>(d + 48)->empty();
    void* node = reinterpret_cast<void* (*)(void*)>((*reinterpret_cast<void***>(ctrl))[56 / 4])(ctrl);  // GetObjectNode
    void* kz = node ? *reinterpret_cast<void**>(static_cast<char*>(node) + 0x50) : 0;
    void* state = kz ? find(kz, "0", 0) : 0;
    void* band = state ? find(state, "Text_Band", 3) : 0;
    float w = 0;
    if (!band || !g_kz.get_float(band, *g_kz.layout_width, &w))
        return;
    if (!g_stock_known && w != kWideWidth) {  // a node not changed yet: the stock width
        g_stock_width = w;
        g_stock_known = true;
    }
    float want = named ? kWideWidth : g_stock_width;
    if (!g_stock_known || w == want)
        return;
    g_kz.set_float(band, *g_kz.layout_width, want);
    g_kz.invalidate_layout(band);
}

bool is_radio() {
    char exe[96];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return false;
    exe[n] = 0;
    return strstr(exe, "/sv.g5.mib2.radio/bin/radio") != 0;
}

}  // namespace

void aa_radio_init() {
    if (!is_radio())
        return;
    for (size_t i = 0; i < sizeof kRadioFuncs / sizeof kRadioFuncs[0]; ++i) {
        const RadioFunc& f = kRadioFuncs[i];
        if (!f.addr || fnv(reinterpret_cast<const uint8_t*>(f.addr), f.size) != f.fnv) {
            syslog(LOG_WARNING, "aa-radio: %s is not the known code; the presets show frequencies", f.name);
            return;
        }
    }
    g_item_base = static_cast<const abi::__class_type_info*>(
        dlsym(RTLD_DEFAULT, "_ZTI34SkObjectGeneratorItemDataExchanger"));
    g_preset_freq = reinterpret_cast<PresetFreq>(func("preset_freq")->addr);
    g_set_freq = reinterpret_cast<SetText>(func("set_freq")->addr);
    g_set_unit = reinterpret_cast<SetText>(func("set_unit")->addr);
    uint32_t update = func("update_fm")->addr, ps = func("get_ps")->addr;
    g_orig_update = reinterpret_cast<Method0>(aa_hook_trampoline(update));
    g_orig_ps = reinterpret_cast<GetPs>(aa_hook_trampoline(ps));
    if (!g_item_base || !g_orig_update || !g_orig_ps) {
        syslog(LOG_WARNING, "aa-radio: a function is missing; the presets show frequencies");
        return;
    }
    load();
    if (!aa_hook_jump(ps, reinterpret_cast<const void*>(hook_get_ps)) ||
        !aa_hook_jump(update, reinterpret_cast<const void*>(hook_update_fm))) {
        syslog(LOG_ERR, "aa-radio: patching failed");
        return;
    }
    // The wide name field: optional, the names go in both fields without it.
#define KZ(f, name) g_kz.f = reinterpret_cast<decltype(g_kz.f)>(dlsym(RTLD_DEFAULT, name))
    KZ(child_count, "kzuObjectNodeGetChildCount");
    KZ(child_at, "kzuObjectNodeGetChildAtIndex");
    KZ(get_name, "kzuObjectNodeGetName");
    KZ(get_float, "kzuObjectNodeGetFloatProperty");
    KZ(set_float, "kzuObjectNodeSetFloatProperty");
    KZ(invalidate_layout, "kzuObjectNodeInvalidateLayout");
    KZ(layout_width, "KZU_PROPERTY_TYPE_LAYOUT_WIDTH");
#undef KZ
    uint32_t item = func("item_ctrl")->addr;
    bool kanzi = g_kz.child_count && g_kz.child_at && g_kz.get_name && g_kz.get_float && g_kz.set_float &&
                 g_kz.invalidate_layout && g_kz.layout_width;
    g_orig_item = kanzi ? reinterpret_cast<ItemCtrl>(aa_hook_trampoline(item)) : 0;
    g_wide = g_orig_item && aa_hook_jump(item, reinterpret_cast<const void*>(hook_item_ctrl));
    syslog(LOG_INFO, "aa-radio: station names on the FM presets (%s)%s", AA_RADIO_STATE,
           g_wide ? "" : "; the names go in both fields");
}
