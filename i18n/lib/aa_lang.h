// SPDX-License-Identifier: GPL-3.0-or-later
//
// The language state shared by the parts of our libUIPublic.so (uipublic.cpp: the phrase
// book and the state; settings.cpp: the Settings app's language list; literals.cpp: the apps'
// own texts).
#ifndef AA_LANG_H
#define AA_LANG_H

#include <stdint.h>

// Visible only in the test build (i18n/test_lib.sh); the unit's library exports the stock five.
#ifdef AA_LANG_TEST
#define AA_LANG_API __attribute__((visibility("default")))
#else
#define AA_LANG_API
#endif

// The languages the Settings list offers, in its order: English and Chinese always, then the
// ones strings.bin carries. Codes are "en", "zh", "es", ...; names are each language's own name.
AA_LANG_API int aa_lang_count();
AA_LANG_API const char* aa_lang_code(int i);
AA_LANG_API const char* aa_lang_name(int i);
AA_LANG_API const char* aa_lang_flag(int i);  // the flag picture's path, or 0
AA_LANG_API int aa_lang_index(const char* code);  // -1 if not offered

// The code in force in this process ("en" when nothing valid is stored), or "off".
AA_LANG_API const char* aa_lang_current();

// Stores `code` as the unit's language (the state file, for every process) and applies it in
// this process. Returns 0 on success.
AA_LANG_API int aa_lang_select(const char* code);

// For settings.cpp: phrase `id` reads `text` in every column until called with text 0.
void aa_lang_phrase_override(int id, const char* text);
// The car's language code (SVP_GetLanguage; 0 if unknown).
void aa_lang_car(unsigned& lang);

// For literals.cpp: looks at the state now (as a phrase lookup does), switching if it changed.
void aa_lang_refresh();
// The apps' own texts in aa_lang.bin: their number, and *md5 the md5 of their English (32 hex
// characters, not NUL-terminated; 0 without a data file). aa_lang_lit_text: the text of entry
// `entry` in language `code`, or 0 where it has none (the app's English stays).
uint32_t aa_lang_lit_count(const char** md5);
const char* aa_lang_lit_text(const char* code, uint32_t entry);

// The apps' own texts (literals.cpp): aa_lit_init finds this app's sites and checks them, once,
// before the first aa_lit_apply; aa_lit_apply points them at `code`'s texts (stock for en, zh,
// off). Called from the library's constructor and on every switch.
void aa_lit_init();
void aa_lit_apply(const char* code);

// The Settings app's list (settings.cpp): installs its hooks when this process is the Settings
// app of a known build. Called once from the library's constructor, unless the state is "off".
void aa_settings_init();

// A function's entry hooked (settings.cpp): aa_hook_trampoline runs its first two instructions
// and jumps to the rest (0 if they cannot move); aa_hook_jump makes its entry jump to `target`.
void* aa_hook_trampoline(uint32_t addr);
bool aa_hook_jump(uint32_t addr, const void* target);

// Settings > Appearance (appearance.cpp). aa_text: one of our own texts (the sheet's "lit:aa:"
// rows) in the language in force, English where it has none. Themes: 0 Automatic (the coding),
// then amber, red, blue, green; aa_theme_rgb is 0x00RRGGBB (Automatic: the coded colour). Start screens:
// 0 Automatic, then the 12 logos in NOR's order. *_select stores the choice and applies it now;
// 0 on success. aa_appearance_init: the launcher's hooks, from the constructor.
const char* aa_text(const char* key);
int aa_theme_count();
const char* aa_theme_name(int i);
const char* aa_theme_key(int i);
int aa_theme_current();
uint32_t aa_theme_rgb(int i);
int aa_theme_select(int i);
int aa_logo_count();
const char* aa_logo_name(int i);
int aa_logo_current();
int aa_logo_select(int i);
void aa_appearance_init();

// No Bluetooth pop-ups while Android Auto is on screen (bluetooth.cpp): installs its hook when
// this process is the Bluetooth app. Called once from the library's constructor, whatever the
// language state.
void aa_bt_init();

// Station names on the FM preset tiles (radio.cpp): installs its hooks when this process is the
// radio app of a known build. Called once from the library's constructor, unless the state is "off".
void aa_radio_init();

// The track's cover on the media player's USB screen (media.cpp): installs its hooks when this
// process is the media player of a known build. Called once from the library's constructor, unless
// the state is "off".
void aa_media_init();

#endif
