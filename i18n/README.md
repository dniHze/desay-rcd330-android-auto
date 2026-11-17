# UI languages and Appearance

The unit's UI has two languages, Chinese and English. This folder adds fifteen more, picked in
Settings > Language, each with its flag. The choice is stored on the unit, not sent to the car.
The apps' own texts (AppConnect's pop-ups and screens, a few pieces in other apps) follow it
too. The same library adds Settings > Appearance: the color of the menus and the start screen,
kept over the car's coding.

| | |
|---|---|
| Languages, in the list's order (by own name) | Čeština, Dansk, Deutsch, English (US), Español, Hrvatski, Italiano, Magyar, Nederlands, Polski, Português, Română, Suomi, Svenska, Türkçe, Українська, 中文 |
| Default | English (US), also after a factory reset |
| Where the choice lives | `/storage/data/data/aa/lang` on the unit |
| Switch off | an `aa-lang-off` file on a card or stick, or `AA_LANG=0` in `aa.conf`; `aa-lang-on` turns it back on |

## The sheet: `strings.csv`

One row per phrase of the unit's phrase book, which every UI app reads its texts from, plus
the apps' own texts. The file is UTF-8 with a BOM, so Excel, Numbers and Google Sheets open it
as is. Inside a cell, a line break is written `\n`.

| Column | What |
|---|---|
| `id` | The phrase number (0–1652), or `lit:<app>:<n>` for an app's own text |
| `in_0317`, `in_0394` | Which base image has the phrase |
| `group` | Universal (OK, Cancel, Off…), an app, an app's own texts, or "Not found in code" |
| `apps` | The apps whose code reads the phrase (`callsites.py`) |
| `element` | title, button, list item, option, tab, label, message, pop-up title, pop-up button, unit, value, status bar, keyboard, or fragment (a piece the app joins to others) |
| `context` | Where and what, in at most two sentences, for a translator |
| `en`, `zh` | The stock English and Chinese texts (below) |
| `es`, `de`, `pt`, `tr`, `uk`, `nl`, `it`, `pl`, `cs`, `sv`, `da`, `hr`, `hu`, `ro`, `fi` | The translations. An empty cell shows English |
| `reviewed` | The language codes a person has checked |
| `notes` | Where a translation came from: `deepl` (a DeepL draft, `deepl redo` when shortened), `claude: <langs>` (translated by Claude), `post-edit <lang>: …` (changed by hand, `(review)` from a review pass), `hand: …` (written by hand) |

**The stock texts.** In this repository the phrase-book rows have empty `en` and `zh` cells:
those texts are VW's. `extract.py` fills them from your own stock images (both bases), which it
unpacks with `fw.py` into `out/i18n/`. The image build needs them, and refuses a sheet whose
English differs from the image's library. Keep them out of commits. The `lit:<app>:<n>` rows keep
their English, which the build needs to find each text in the apps' code. The `lit:aa:*` rows
are our own texts (Settings > Appearance). To change what the unit shows in English or Chinese,
add a line to `stock_overrides.csv` (id, code, text, why) rather than editing `en` or `zh`.

English is US English ("Color"), in the stock texts and in our own rows alike.

**Which rows matter most.** About 1,000 phrases are read by code in these units, plus the apps'
own texts. The rows in "Not found in code" are used only by other vehicles and markets.

**An app's own text** (`lit:` rows) is one message: a pop-up's lines joined by `\n`, each line
one text of the app. A translation must have exactly the English's number of lines, and a line
with `%1` (filled in by the app) stays a line of its own. A `fragment` is joined to numbers or
names by the app ("in " + "1500 km" + " or " + "30 days"): word it so it reads right wherever it
is used, and keep its spaces.

**Rules for a translation:**
- **Keep as is:** `%1`, `%2`; the terms in `keep_terms.txt` (SD, USB, Bluetooth, CarPlay, OK…);
  the `\n` breaks, about as many as the English.
- **Translate:** units, multi-word feature names (Park Assist…) and Start-Stop. ParkPilot stays
  ParkPilot.
- **Length:** as short as reads well. Use standard abbreviations rather than overflow a button.

## Editing translations

```sh
python3 -I i18n/extract.py            # once: fills en and zh from your stock images
python3 -I i18n/check.py              # errors must be 0; --all lists the warnings, --lang uk one language
python3 -I i18n/public_sheet.py       # before a commit: takes VW's texts out again
```

Edit the cells, run `check.py`, then build an image with `--accept-new`
([`docs/building.md`](../docs/building.md)). A changed sheet changes `aa_lang.bin`. Before you
commit, run `public_sheet.py`: it empties VW's texts again (the phrase book's `en` and `zh`, the
apps' own texts' `zh`), so `git diff` shows only your changes. `check.py` stops early on a sheet
without them and asks for `extract.py`.

`check.py` fails on: placeholders that differ from the English; a protected term changed; too
many line breaks (for `lit:` rows, any count but the English's); a letter the unit's font
(FZHT GB18030, read from the unpacked 0317 image) has no glyph for, or the capital ẞ; and edge
spaces that differ from the English. It warns about texts over 1.4 times the English, empty
cells, and one English text translated two ways in one group.

## Adding a language

1. Add a row to `languages.csv`: the code, its own name, the DeepL code, the flag, and its place
   in the list (`order`: by own name, ignoring accents; Latin, then Cyrillic, then Chinese). Keep
   the existing rows in place: their order is the sheet's column order.
2. Add a column with that code to `strings.csv` (`extract.py` adds it when it next runs).
3. Draw its flag in `flags.py`, in `DRAW`, at the stock geometry.
4. Translate, by any of these routes:
   - **DeepL:** `translate.py prepare --lang xx` writes one request file per batch to
     `out/i18n/translate/`. Send each call to DeepL, and save each answer as
     `<batch>/call<n>.json`. Then run `translate.py collect <batch>` for each batch,
     `translate.py merge`, and `translate.py redo-long` for labels that came out long. The
     glossary in `translate.py` (`GLOSSARY`, `GLOSSARY_LANGS`) maps the protected terms to
     themselves; point it at your own, or empty `GLOSSARY_LANGS` to go without.
   - **Another source:** translations as `{"<English>": {"<lang>": text}}` go into the empty
     cells with `translate.py fill <file.json>`, which applies the same checks.
   - **A review pass** over the short labels (buttons, units, dates, list items, where a word
     without a sentence can take the wrong sense): fixes as `{"<id>": {"<lang>": {"text",
     "why"}}}`, applied with `translate.py review <fixes.json>`.
   - **By hand:** fill the column. The apps' own fragments are best written by hand.
5. Run `check.py`, then build.

The unit's font has Latin (with every Central European, Nordic, Romanian and Turkish letter),
Cyrillic and Greek letters; only the capital ẞ is missing.

## Building

`sysupdate/build_update.py` runs `i18n/build.py` for its base, from that base's own stock files.
It needs Docker with `desay-aa:latest`, the cross toolchain and the sysroot.

| Output | Into the image | What |
|---|---|---|
| `libUIPublic.so` | replaces `/svp/lib/libUIPublic.so` | our library: the stock phrase book, the language state, Settings > Appearance |
| `aa_libUIPublic.so` | `/svp/share/`, bind-mounted over it in Android Auto mode | the same, with CarLife named Android Auto in every language |
| `aa_lang.bin` | `/svp/share/` | the other languages' texts and the apps' own texts (`build_table.py`) |
| `aa_lang_<flag>.png`, `aa_theme_<name>.png`, `aa_blank.png` | `/svp/share/` | the list's flags and Appearance's pictures (`flags.py`) |

The library links as the stock one does (the unit's libstdc++, only the stock exports), and
`lib/build.sh` refuses a build that does not.

```sh
python3 -I i18n/test_lib.py
```

It builds a test variant of the library and runs it on the unit's own loader and libraries
under QEMU, for both bases and the Android Auto variant: the stock behavior with the state `off`,
a missing or damaged state, Chinese and a translated language, a switch written by another
process, a data file for another phrase book, the Settings hooks' trampoline, the Bluetooth
pop-up hook and the apps' own texts.

## How it works

- **The phrase book.** The stock library exports `s_Items`: per phrase, a row of
  `{Chinese, English}` that the UI framework reads on every lookup, picking the column from the
  car's language. Our library exports the same symbols. For the chosen language it points every
  row at that language's text, or English where it has none, so every app follows the choice
  whatever the car says.
- **The state.** `/storage/data/data/aa/lang` is 32 bytes, `AALANG1 <code>` padded. Every app maps
  it shared and reads it on each lookup, so Settings rewrites it in place and all apps switch.
  Missing or invalid means English; `off` is exactly the stock library. The software update app
  always gets the stock behavior.
- **Settings** (`lib/settings.cpp`) hooks the Settings app only when its code matches a known
  build exactly: the language list with flags, the switch (every app is told to redraw), the
  Android Auto row in System information, and the Appearance pages.
- **The apps' own texts** (`lib/literals.cpp`, `literals.py`): where an app picks a text of its
  own, the library rewrites the literal-pool words that reach it, in the app's private copy of its
  code, to the chosen language's text in `aa_lang.bin`, only if every word is the stock one.
- **No Bluetooth pop-ups over Android Auto** (`lib/bluetooth.cpp`): the Bluetooth app's pop-ups
  are held back while the shim's `/tmp/run/aa/ontop` says Android Auto is on screen.
- **Settings > Appearance** (`lib/appearance.cpp`): the color theme (Automatic, Amber, Red, Blue, Green)
  and the start screen (Automatic or one of twelve pictures). The choice lives in
  `/storage/data/data/aa/appearance`; the launcher app's color is hooked while one is chosen, and
  the start screen's index is written to the NOR flash, where the coding keeps its own.
  `aa-appearance-off` or `AA_APPEARANCE=0` sets both back to the coding.
- **Station names on the FM presets** (`lib/radio.cpp`, `gen_radio.py`): the radio app's RDS name
  (PS) is counted per frequency while it plays; the name a frequency sent most often is its name
  (stations also send song titles in PS), kept in `/storage/data/data/aa/stations`. A preset tile
  with a name shows it, centred, in the tile's own centred layout widened to the tile; the others
  stay stock. The tiles show a new name at their next refresh (a station change, a preset stored,
  the app started again).
- **Covers on the USB screen** (`lib/media.cpp`, `gen_media.py`): the media service already hands
  the player each track's cover (`SVPPlayerState::SetID3`); the library loads it with Kanzi's own
  image calls, crops it to a square and puts it where the music note's square is.

Both hook only a build whose functions are byte for byte the known ones, and `aa-lang-off` turns
them off with the languages.

More: [`docs/architecture.md`](../docs/architecture.md#languages-and-appearance).

## Files

| File | What |
|---|---|
| `fw.py` | unpacks the stock images into `out/i18n/`; reads ELF files and the phrase book |
| `callsites.py` | which app and function reads each phrase |
| `extract.py` | builds or refreshes the sheet, keeping what people wrote |
| `public_sheet.py` | takes VW's texts out of the sheet before a commit |
| `context.py` | the translator-context pass |
| `translate.py` | the translation passes: DeepL requests and answers, `fill`, `review`, `redo-long` |
| `check.py`, `keep_terms.txt` | the sheet's checks, and the terms never translated |
| `languages.csv` | the languages: own names, DeepL codes, flags, list order |
| `stock_overrides.csv` | the few English and Chinese texts the unit shows differently from stock |
| `literals.py` | the apps' own texts: where each app reads them |
| `build.py`, `build_table.py`, `gen_table.py`, `gen_settings.py`, `gen_launcher.py`, `gen_texts.py`, `flags.py` | the build |
| `lib/` | the library's source and `build.sh` |
| `test_lib.py`, `test/test_lib.cpp` | the library's tests under QEMU |
