// SPDX-License-Identifier: GPL-3.0-or-later
//
// Our libUIPublic.so: the unit's phrase book, in the language the user picked.
//
// The stock library has five exports, and this one has the same five with the same behaviour:
//   s_Items          N entries {int id, const char** row}; row points at {Chinese, English}
//   GetStringItems() &s_Items. Every app hands it to the UI framework's string translator
//                    (SetStirngTable(items, N, 2)), which reads items[id].row[column] afresh on
//                    every lookup.
//   GetVWString(id)  sets the translator's column from the car's language code (1: Chinese
//                    column, else English), then asks it for the text.
//   GetVWStringEx(id) the same choice, read from the rows directly (the launcher uses it).
//   string_replace   replaces every occurrence of one string by another.
//
// What is new: the language comes from a state file, not from the car. For language L every
// row is {L, L}: both columns hold L's text (English where L has none), so the column the car's
// code selects, and the apps that set a column themselves, no longer matter. Switching language
// repoints each s_Items[i].row at another prebuilt row array, an aligned word write per entry,
// so a reader always sees a whole row of one language or the other.
//
// The state file (AA_LANG_STATE, /storage/data/data/aa/lang): 32 bytes, "AALANG1 " + the code
// padded with spaces to 23 characters + "\n" (the shell can write it: printf + dd conv=notrunc).
// Every process maps it MAP_SHARED and reads it on each lookup, so a change written in place
// reaches every app at once. It is never truncated (a mapped page past the end of a file
// faults). Missing: the first process creates it as "en" and, if the car's code is Chinese,
// asks the car for English once, so the parts of the UI that read the car's code follow too.
// Unreadable or invalid: English. "off": exactly the stock behaviour, Settings untouched. The
// software update app always gets the stock behaviour, whatever the state.
//
// The other languages' texts (AA_LANG_DATA, /svp/share/aa_lang.bin, made by
// i18n/build_table.py from the sheet): checked whole before use; a file built for another
// phrase book (AA_EN_MD5), or damaged, leaves only English and Chinese. It also holds the apps'
// own texts (outside the phrase book), which literals.cpp points the apps at.
//
// The phrase book (Chinese and English, byte for byte as the stock or rebranded library has
// them) comes from i18n/gen_table.py at build time (AA_TABLE). AA_REBRAND (the Android Auto
// variant) also renames CarLife in the other languages' texts, as kzb-re/rebrand_strings.py
// does in the stock texts.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include <string>

#include "aa_lang.h"

#ifndef AA_LANG_STATE
#define AA_LANG_STATE "/storage/data/data/aa/lang"
#endif
#ifndef AA_LANG_DATA
#define AA_LANG_DATA "/svp/share/aa_lang.bin"
#endif

#define EXPORT __attribute__((visibility("default")))

struct StringItem {
    int id;
    const char** row;
};

extern "C++" {
EXPORT extern StringItem s_Items[];
}
#include AA_TABLE

// From the app process (libapp, libUIFrame-kanzi), as the stock library uses them.
int SVP_GetLanguage(unsigned int& lang);
void* App();
// Used only when present.
int SVP_ChangeLanguage(unsigned int lang) __attribute__((weak));

namespace {

const int kRecord = 32;
const char kMagic[] = "AALANG1 ";
const int kMaxLangs = 32;

struct DataLang {
    char code[8];
    uint32_t name;
    uint32_t flag;
    uint32_t table;
    uint32_t lit;  // its table of the apps' own texts
};

const uint8_t* g_data;            // aa_lang.bin, checked
uint32_t g_data_size;
const DataLang* g_data_langs;
int g_data_nlangs;
uint32_t g_lit_count;             // the apps' own texts in it, and the md5 of their English
const char* g_lit_md5;

const volatile char* g_state;     // the mapped state record, or 0
unsigned g_retry;                 // lookups since the last attempt to map it
int g_created;                    // 1: this process created the state; 2: the car was asked
char g_code[8];                   // the code in force here
const char** g_rows[kMaxLangs + 3];  // prebuilt rows per language (see rows_for)
volatile int g_busy;
bool g_frozen;  // the software update app: always the stock behaviour

const char* const kBuiltinCodes[2] = {"en", "zh"};
const char* const kBuiltinNames[2] = {"English (US)", "\344\270\255\346\226\207"};

bool valid_code(const char* c, size_t n) {
    size_t k = 0;
    while (k < n && c[k] >= 'a' && c[k] <= 'z')
        ++k;
    return k >= 2 && k <= 3 && (k == n || c[k] == 0 || c[k] == ' ');
}

// The data file: header, language entries, per-language offset tables, NUL-terminated texts.
void load_data() {
    int fd = open(AA_LANG_DATA, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 40 || st.st_size > (64 << 20)) {
        close(fd);
        return;
    }
    void* m = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
        return;
    const uint8_t* d = static_cast<const uint8_t*>(m);
    uint32_t size = st.st_size;
    uint32_t version, n, nlangs, lit_head, lits = 0;
    memcpy(&version, d + 8, 4);
    memcpy(&n, d + 12, 4);
    memcpy(&nlangs, d + 16, 4);
    memcpy(&lit_head, d + 52, 4);
    bool ok = memcmp(d, "AALANGS", 8) == 0 && version == 2 && n == AA_N && nlangs <= kMaxLangs &&
              memcmp(d + 20, AA_EN_MD5, 32) == 0 && d[size - 1] == 0 && 56 + nlangs * sizeof(DataLang) <= size;
    if (ok && lit_head) {
        ok = lit_head % 4 == 0 && lit_head + 48ull <= size && memcmp(d + lit_head, "AALITS\0", 8) == 0;
        if (ok)
            memcpy(&lits, d + lit_head + 8, 4);
    }
    const DataLang* langs = reinterpret_cast<const DataLang*>(d + 56);
    for (uint32_t l = 0; ok && l < nlangs; ++l) {
        const DataLang& e = langs[l];
        ok = valid_code(e.code, sizeof e.code) && e.name < size && (e.flag == 0 || e.flag < size) &&
             (e.table == 0 || (e.table % 4 == 0 && e.table + 4ull * AA_N <= size)) &&
             (e.lit == 0 || (lits && e.lit % 4 == 0 && e.lit + 4ull * lits <= size));
        if (ok && e.table) {
            const uint32_t* t = reinterpret_cast<const uint32_t*>(d + e.table);
            for (uint32_t i = 0; ok && i < AA_N; ++i)
                ok = t[i] < size;
        }
        if (ok && e.lit) {
            const uint32_t* t = reinterpret_cast<const uint32_t*>(d + e.lit);
            for (uint32_t i = 0; ok && i < lits; ++i)
                ok = t[i] < size;
        }
    }
    if (!ok) {
        syslog(LOG_WARNING, "aa-lang: %s is not for this phrase book or is damaged; only English and Chinese",
               AA_LANG_DATA);
        munmap(m, st.st_size);
        return;
    }
    g_data = d;
    g_data_size = size;
    g_data_langs = langs;
    g_data_nlangs = nlangs;
    g_lit_count = lits;
    g_lit_md5 = lits ? reinterpret_cast<const char*>(d + lit_head + 12) : 0;
}

const DataLang* data_lang(const char* code) {
    for (int l = 0; l < g_data_nlangs; ++l)
        if (strncmp(g_data_langs[l].code, code, sizeof g_data_langs[l].code) == 0)
            return &g_data_langs[l];
    return 0;
}

// The code in a record, or false if it is not a whole valid record (a write in progress reads
// as invalid and is ignored until it is complete).
bool read_record(const volatile char* r, char* code) {
    char buf[kRecord];
    for (int i = 0; i < kRecord; ++i)
        buf[i] = r[i];
    if (memcmp(buf, kMagic, 8) != 0 || buf[kRecord - 1] != '\n')
        return false;
    int k = 8;
    while (k < kRecord - 1 && buf[k] >= 'a' && buf[k] <= 'z')
        ++k;
    if (k - 8 < 2 || k - 8 > 3)
        return false;
    for (int j = k; j < kRecord - 1; ++j)
        if (buf[j] != ' ')
            return false;
    memset(code, 0, 8);
    memcpy(code, buf + 8, k - 8);
    return true;
}

void make_record(char* r, const char* code) {
    snprintf(r, kRecord + 1, "%s%-23s\n", kMagic, code);
}

void ensure_dir() {
    char dir[sizeof AA_LANG_STATE];
    strcpy(dir, AA_LANG_STATE);
    char* slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        mkdir(dir, 0755);  // its parent exists on the unit; EEXIST is fine
    }
}

int write_record(const char* code, bool create_only) {
    char r[kRecord + 1];
    make_record(r, code);
    int fd = open(AA_LANG_STATE, O_WRONLY | O_CREAT | O_CLOEXEC | (create_only ? O_EXCL : 0), 0644);
    if (fd < 0)
        return -1;
    int ok = pwrite(fd, r, kRecord, 0) == kRecord && fsync(fd) == 0;
    close(fd);
    return ok ? 0 : -1;
}

void map_state(bool create) {
    int fd = open(AA_LANG_STATE, O_RDONLY | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT && create) {
        ensure_dir();
        if (write_record("en", true) == 0)
            g_created = 1;
        fd = open(AA_LANG_STATE, O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0)
        return;
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size >= kRecord) {
        void* m = mmap(0, kRecord, PROT_READ, MAP_SHARED, fd, 0);
        if (m != MAP_FAILED)
            g_state = static_cast<const volatile char*>(m);
    }
    close(fd);
}

#ifdef AA_REBRAND
// CarLife is Android Auto on this unit (kzb-re/rebrand_strings.py, UIPUBLIC).
const char* rebrand(const char* t) {
    static const char* const kRules[][2] = {
        {"Baidu\nCarLife", "Android\nAuto"}, {"Baidu CarLife", "Android Auto"}, {"CarLife", "Android Auto"}};
    if (!strstr(t, "CarLife"))
        return t;
    std::string s(t);
    for (size_t r = 0; r < sizeof kRules / sizeof kRules[0]; ++r) {
        size_t a = strlen(kRules[r][0]), b = strlen(kRules[r][1]);
        for (size_t pos = s.find(kRules[r][0]); pos != std::string::npos; pos = s.find(kRules[r][0], pos + b))
            s.replace(pos, a, kRules[r][1], b);
    }
    char* out = strdup(s.c_str());
    return out ? out : t;
}
#endif

// Rows {L, L} for a language: slot 0 English, 1 Chinese, 2.. strings.bin's languages. English
// and Chinese are the library's own texts, except the few phrases aa_lang.bin's tables for them
// change (i18n/stock_overrides.csv).
const char** rows_for(const char* code) {
    int slot;
    const DataLang* dl = g_data ? data_lang(code) : 0;
    if (strcmp(code, "en") == 0) {
        slot = 0;
    } else if (strcmp(code, "zh") == 0) {
        slot = 1;
    } else {
        if (!dl || !dl->table)
            return 0;
        slot = 2 + (dl - g_data_langs);
    }
    if (g_rows[slot])
        return g_rows[slot];
    const char** rows = static_cast<const char**>(malloc(sizeof(char*) * 2 * AA_N));
    if (!rows)
        return 0;
    const uint32_t* table = dl && dl->table ? reinterpret_cast<const uint32_t*>(g_data + dl->table) : 0;
    for (int i = 0; i < AA_N; ++i) {
        const char* t;
        if (table && table[i])
            t = reinterpret_cast<const char*>(g_data + table[i]);
        else if (slot == 1)
            t = aa_stock_rows[i][0];
        else
            t = aa_stock_rows[i][1];
#ifdef AA_REBRAND
        if (table && table[i])
            t = rebrand(t);
#endif
        rows[2 * i] = rows[2 * i + 1] = t;
    }
    g_rows[slot] = rows;
    return rows;
}

void apply(const char* code) {
    if (!__sync_bool_compare_and_swap(&g_busy, 0, 1))
        return;  // another thread is switching; the next lookup looks again
    if (strcmp(code, "off") == 0) {
        for (int i = 0; i < AA_N; ++i)
            s_Items[i].row = const_cast<const char**>(aa_stock_rows[i]);
    } else {
        const char** rows = rows_for(code);
        if (!rows) {
            code = "en";
            rows = rows_for(code);
        }
        if (rows)
            for (int i = 0; i < AA_N; ++i)
                s_Items[i].row = rows + 2 * i;
    }
    aa_lit_apply(code);
    strncpy(g_code, code, sizeof g_code - 1);
    __sync_synchronize();
    g_busy = 0;
}

// Called on every lookup: one read of the mapped record, a switch only when it changed.
void refresh() {
    if (g_frozen)
        return;
    if (!g_state) {
        if (++g_retry & 0xff)
            return;
        map_state(false);
        if (!g_state)
            return;
    }
    char code[8];
    if (read_record(g_state, code) && strcmp(code, g_code) != 0)
        apply(code);
}

// The first process to create the state, once the app runs: the car's code to English too.
void sync_car() {
    if (g_frozen || g_created != 1)
        return;
    g_created = 2;
    unsigned int lang = 0;
    SVP_GetLanguage(lang);
    if (lang == 1 && SVP_ChangeLanguage) {
        syslog(LOG_INFO, "aa-lang: new state, English; asking the car for English too");
        SVP_ChangeLanguage(0);
    }
}

// The software update app (the way back to a stock image) keeps the stock library's behaviour
// exactly: no state, no data file, the stock rows.
bool is_update_app() {
    char exe[96];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return false;
    exe[n] = 0;
    return strstr(exe, "/sv.g5.mib2.update/") != 0;
}

__attribute__((constructor)) void init() {
    if (is_update_app()) {
        g_frozen = true;
        strcpy(g_code, "off");
        return;
    }
    load_data();
    map_state(true);
    char code[8] = "en";
    if (g_state && !read_record(g_state, code))
        strcpy(code, "en");
    if (strcmp(code, "off") != 0)
        aa_lit_init();
    apply(code);
    syslog(LOG_INFO, "aa-lang: %s (%d languages%s%s)", g_code, aa_lang_count(), g_state ? "" : ", no state file",
           g_created ? ", state created" : "");
    if (strcmp(g_code, "off") != 0) {
        aa_settings_init();
        aa_radio_init();
    }
    aa_bt_init();
    aa_appearance_init();
}

}  // namespace

// ---- the stock interface ----

EXPORT StringItem* GetStringItems() {
    return s_Items;
}

struct Obj {
    void** vtbl;
};

EXPORT const char* GetVWString(int id) {
    refresh();
    sync_car();
    unsigned int lang = 0;
    SVP_GetLanguage(lang);
    typedef Obj* (*GetTranslator)(Obj*);
    typedef void (*SettledColumn)(Obj*, int);
    typedef const char* (*GetString)(Obj*, int);
    Obj* app = *reinterpret_cast<Obj**>(static_cast<char*>(App()) + 0x24);
    Obj* tr = reinterpret_cast<GetTranslator>(app->vtbl[0x4c / 4])(app);
    reinterpret_cast<SettledColumn>(tr->vtbl[0x14 / 4])(tr, lang == 1 ? 0 : 1);
    app = *reinterpret_cast<Obj**>(static_cast<char*>(App()) + 0x24);
    tr = reinterpret_cast<GetTranslator>(app->vtbl[0x4c / 4])(app);
    return reinterpret_cast<GetString>(tr->vtbl[0x18 / 4])(tr, id);
}

EXPORT const char* GetVWStringEx(int id) {
    refresh();
    unsigned int lang = 0;
    SVP_GetLanguage(lang);
    return s_Items[id].row[lang == 1 ? 0 : 1];
}

EXPORT std::string string_replace(const std::string& s, const std::string& from, const std::string& to) {
    std::string r(s);
    for (std::string::size_type pos = r.find(from.data(), 0, from.size()); pos != std::string::npos;
         pos = r.find(from.data(), pos + to.size(), from.size()))
        r.replace(pos, from.size(), to.data(), to.size());
    return r;
}

// ---- the language state, for settings.cpp ----

int aa_lang_count() {
    int n = 0;
    for (int l = 0; l < g_data_nlangs; ++l)
        n += g_data_langs[l].table != 0 || strcmp(g_data_langs[l].code, "en") == 0 || strcmp(g_data_langs[l].code, "zh") == 0;
    return g_data ? n : 2;
}

static int data_index(int i) {
    for (int l = 0; l < g_data_nlangs; ++l) {
        const DataLang& e = g_data_langs[l];
        if (e.table || strcmp(e.code, "en") == 0 || strcmp(e.code, "zh") == 0) {
            if (i-- == 0)
                return l;
        }
    }
    return -1;
}

const char* aa_lang_code(int i) {
    if (!g_data)
        return i >= 0 && i < 2 ? kBuiltinCodes[i] : 0;
    int l = data_index(i);
    return l < 0 ? 0 : g_data_langs[l].code;
}

const char* aa_lang_name(int i) {
    if (!g_data)
        return i >= 0 && i < 2 ? kBuiltinNames[i] : 0;
    int l = data_index(i);
    return l < 0 ? 0 : reinterpret_cast<const char*>(g_data + g_data_langs[l].name);
}

const char* aa_lang_flag(int i) {
    if (!g_data)
        return 0;
    int l = data_index(i);
    return l < 0 || !g_data_langs[l].flag ? 0 : reinterpret_cast<const char*>(g_data + g_data_langs[l].flag);
}

int aa_lang_index(const char* code) {
    for (int i = 0; i < aa_lang_count(); ++i)
        if (strcmp(aa_lang_code(i), code) == 0)
            return i;
    return -1;
}

const char* aa_lang_current() {
    return g_code;
}

void aa_lang_phrase_override(int id, const char* text) {
    static const char* row[2];
    static const char** saved;
    static int saved_id = -1;
    if (text && id >= 0 && id < AA_N) {
        if (saved_id < 0) {
            saved_id = id;
            saved = s_Items[id].row;
        }
        row[0] = row[1] = text;
        s_Items[id].row = row;
    } else if (!text && saved_id >= 0) {
        s_Items[saved_id].row = saved;
        saved_id = -1;
    }
}

void aa_lang_refresh() {
    refresh();
}

uint32_t aa_lang_lit_count(const char** md5) {
    *md5 = g_lit_md5;
    return g_lit_count;
}

const char* aa_lang_lit_text(const char* code, uint32_t entry) {
    const DataLang* dl = g_data ? data_lang(code) : 0;
    if (!dl || !dl->lit || entry >= g_lit_count)
        return 0;
    uint32_t off = reinterpret_cast<const uint32_t*>(g_data + dl->lit)[entry];
    if (!off)
        return 0;
    const char* t = reinterpret_cast<const char*>(g_data + off);
#ifdef AA_REBRAND
    static const char** done[kMaxLangs];  // rebrand() allocates: once per language and text
    const char**& d = done[dl - g_data_langs];
    if (!d)
        d = static_cast<const char**>(calloc(g_lit_count, sizeof(char*)));
    if (!d)
        return t;
    if (!d[entry])
        d[entry] = rebrand(t);
    t = d[entry];
#endif
    return t;
}

void aa_lang_car(unsigned& lang) {
    lang = 0;
    SVP_GetLanguage(lang);
}

int aa_lang_select(const char* code) {
    if (g_frozen || aa_lang_index(code) < 0)
        return -1;
    ensure_dir();
    if (write_record(code, false) != 0) {
        syslog(LOG_WARNING, "aa-lang: cannot write %s: %s", AA_LANG_STATE, strerror(errno));
        return -1;
    }
    if (!g_state)
        map_state(false);
    apply(code);
    syslog(LOG_INFO, "aa-lang: %s selected", code);
    return 0;
}
