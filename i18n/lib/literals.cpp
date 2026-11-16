// SPDX-License-Identifier: GPL-3.0-or-later
//
// The apps' own texts: texts an app holds itself instead of a phrase (AppConnect's pop-ups and
// screens, carmenu's service-interval pieces, "SD card" in the launcher and the media player, a
// toptip pop-up), in the language the user picked.
//
// Stock, the app reads such a text as "ldr rX, [pc, #n]" (a word of the literal pool next to the
// code) then "add rX, pc, rX": the word is the text's offset from the add's pc (its address + 8).
// The English and the Chinese text each have their own words; the app picks one by the car's code
// (1: Chinese). i18n/literals.py lists, per base and app, every such word that reaches one of the
// sheet's "lit:" texts in the app's UI functions (AA_LITERALS: {pool word, stock word, add, add
// instruction, entry}), and aa_lang.bin holds every language's text of each entry.
//
// Here, in those apps only, and only when every word and add instruction is the stock one (else
// the app's texts stay as they are): the words are rewritten in the process's private copy of
// its code (mprotect), to reach the chosen language's text in aa_lang.bin, or set back to the
// stock word for English, Chinese and "off", and wherever a language has no text. Chinese texts
// are never touched. Done once at start and on every switch (aa_lit_apply, from apply()).
//
// So that a switch is seen before the app picks a text, the app's own calls to SVP_GetLanguage
// (made right before every such choice) go through lit_get_language, which looks at the state
// first: its GOT slot (AA_LITERALS, R_ARM_JUMP_SLOT; the apps have no RELRO) is pointed at it.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <syslog.h>
#include <unistd.h>

#include "aa_lang.h"

int SVP_GetLanguage(unsigned int& lang);

namespace {

struct LitSite {
    uint32_t pool;      // the literal-pool word
    uint32_t word;      // its stock value
    uint32_t add;       // the "add rX, pc, rX" that uses it
    uint32_t insn;      // that instruction, stock
    uint32_t entry;     // the text: literals.entries() index
    uint32_t len;       // the "mov r2, #n" giving a (const char*, size_t) call the text's byte length
    uint32_t len_insn;  // that instruction, stock (len 0: the text goes alone, by strlen)
};

struct LitApp {
    const char* name;  // the package is sv.g5.mib2.<name>, the program bin/<name>
    uint32_t got;      // SVP_GetLanguage's GOT slot, 0: none
    uint32_t plt;      // .plt, the slot's value before the first call
    uint32_t first, count;
};

#ifdef AA_LITERALS
#include AA_LITERALS
#define AA_LIT_APPS (sizeof kLitApps / sizeof kLitApps[0])
#else
#define AA_LIT_COUNT 0
#define AA_LIT_MD5 ""
const LitSite kLitSites[1] = {};
const LitApp kLitApps[1] = {};
#define AA_LIT_APPS 0
#endif

const LitSite* g_sites;  // this app's, checked; 0: nothing to do
uint32_t g_count;
uintptr_t g_lo, g_hi;    // the pages they are on

int lit_get_language(unsigned int& lang) {
    aa_lang_refresh();
    return SVP_GetLanguage(lang);
}

bool is_app(const char* exe, const char* name) {
    char tail[96];
    snprintf(tail, sizeof tail, "/sv.g5.mib2.%s/bin/%s", name, name);
    size_t a = strlen(exe), b = strlen(tail);
    return a >= b && strcmp(exe + a - b, tail) == 0;
}

// The sites, if every one is stock and aa_lang.bin numbers the texts as this table does.
bool take(const LitSite* sites, uint32_t count, const char* app) {
    const char* md5 = 0;
    uint32_t n = aa_lang_lit_count(&md5);
    if (n != AA_LIT_COUNT || !md5 || memcmp(md5, AA_LIT_MD5, 32) != 0) {
        syslog(LOG_WARNING, "aa-lang: %s: aa_lang.bin has other own texts (%u, want %u); they stay English", app, n,
               AA_LIT_COUNT);
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const LitSite& s = sites[i];
        if (s.entry >= n || *reinterpret_cast<const uint32_t*>(s.pool) != s.word ||
            *reinterpret_cast<const uint32_t*>(s.add) != s.insn ||
            (s.len && *reinterpret_cast<const uint32_t*>(s.len) != s.len_insn)) {
            syslog(LOG_WARNING, "aa-lang: %s: the code at 0x%x is not the known one; its own texts stay English", app,
                   s.add);
            return false;
        }
    }
    long page = sysconf(_SC_PAGESIZE);
    g_lo = ~uintptr_t(0);
    g_hi = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (sites[i].pool < g_lo)
            g_lo = sites[i].pool;
        if (sites[i].pool + 4 > g_hi)
            g_hi = sites[i].pool + 4;
        if (sites[i].len && sites[i].len < g_lo)
            g_lo = sites[i].len;
        if (sites[i].len && sites[i].len + 4 > g_hi)
            g_hi = sites[i].len + 4;
    }
    g_lo &= ~uintptr_t(page - 1);
    g_hi = (g_hi + page - 1) & ~uintptr_t(page - 1);
    g_sites = sites;
    g_count = count;
    return true;
}

void hook_get_language(const LitApp& a) {
    if (!a.got)
        return;
    uint32_t* slot = reinterpret_cast<uint32_t*>(a.got);
    uint32_t real = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&SVP_GetLanguage));
    if (*slot != a.plt && *slot != real) {
        syslog(LOG_WARNING, "aa-lang: %s: SVP_GetLanguage's slot holds 0x%x; texts switch at the next phrase lookup",
               a.name, *slot);
        return;
    }
    *slot = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&lit_get_language));
}

}  // namespace

void aa_lit_init() {
    char exe[128];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return;
    exe[n] = 0;
    for (uint32_t i = 0; i < AA_LIT_APPS; ++i) {
        const LitApp& a = kLitApps[i];
        if (!is_app(exe, a.name))
            continue;
        if (take(kLitSites + a.first, a.count, a.name)) {
            hook_get_language(a);
            syslog(LOG_INFO, "aa-lang: %s: %u own texts follow the language", a.name, a.count);
        }
        return;
    }
}

// "mov r2, #n" as the stock instruction has it (its condition and register), for the text's byte
// length n: the 8-bit immediate up to 255, movw above (ARMv7).
uint32_t mov_length(uint32_t stock, size_t n) {
    uint32_t cond_rd = stock & 0xF000F000;
    if (n <= 0xFF)
        return cond_rd | 0x03A00000 | static_cast<uint32_t>(n);
    return cond_rd | 0x03000000 | ((static_cast<uint32_t>(n) & 0xF000) << 4) | (static_cast<uint32_t>(n) & 0x0FFF);
}

void aa_lit_apply(const char* code) {
    if (!g_sites)
        return;
    bool stock = strcmp(code, "en") == 0 || strcmp(code, "zh") == 0 || strcmp(code, "off") == 0;
    void* lo = reinterpret_cast<void*>(g_lo);
    if (mprotect(lo, g_hi - g_lo, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        syslog(LOG_ERR, "aa-lang: own texts: mprotect failed; they stay as they are");
        g_sites = 0;
        return;
    }
    for (uint32_t i = 0; i < g_count; ++i) {
        const LitSite& s = g_sites[i];
        const char* t = stock ? 0 : aa_lang_lit_text(code, s.entry);
        uint32_t w = t ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(t)) - (s.add + 8) : s.word;
        *reinterpret_cast<volatile uint32_t*>(s.pool) = w;
        if (s.len)  // a call that copies the given byte count: the text's own length
            *reinterpret_cast<volatile uint32_t*>(s.len) = t ? mov_length(s.len_insn, strlen(t)) : s.len_insn;
    }
    __builtin___clear_cache(reinterpret_cast<char*>(g_lo), reinterpret_cast<char*>(g_hi));
    mprotect(lo, g_hi - g_lo, PROT_READ | PROT_EXEC);
}

#ifdef AA_LANG_TEST
// For i18n/test_lib.py: one site of the test program (an "ldr r0, [pc]; add r0, pc, r0" pair
// reading entry `entry`, with the "mov r2, #n" of its length at `len` if not 0), checked and
// taken as aa_lit_init takes an app's. Returns 0 if taken.
AA_LANG_API int aa_test_lit(uint32_t pool, uint32_t add, uint32_t entry, uint32_t word, uint32_t insn, uint32_t len,
                            uint32_t len_insn) {
    static LitSite site;
    site.pool = pool;
    site.word = word;
    site.add = add;
    site.insn = insn;
    site.entry = entry;
    site.len = len;
    site.len_insn = len_insn;
    if (!take(&site, 1, "test"))
        return -1;
    aa_lit_apply(aa_lang_current());
    return 0;
}
#endif
