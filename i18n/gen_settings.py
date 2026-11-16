#!/usr/bin/env python3
"""The Settings app's functions our library hooks, as C++ for one base: their addresses, sizes
and an FNV-1a hash of their code bytes.

    python3 -I i18n/gen_settings.py <stock bin/setting> <out .inc>

i18n/lib/settings.cpp hooks a running setting process only when every function below is at its
address with exactly these bytes (the app is ET_EXEC, text from file offset 0 at 0x8000, so the
bytes in memory are the file's). Another build of setting, or a changed one, leaves the stock
language list alone. Generated at build time from the firmware; never committed.
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fw  # noqa: E402

FUNCS = [
    # (C++ name in the table, mangled symbol)
    ("create_list", "_ZN17LanguageSelectDlg10CreateListEv"),
    ("update_list", "_ZN17LanguageSelectDlg10UpdateListEv"),
    ("click", "_ZN17LanguageSelectDlg20OnListClickItemEventEP10ISkListBoxRNS0_19ClickItemEventParamE"),
    ("update_language", "_ZN17LanguageSelectDlg14UpdateLanguageEv"),
    ("init", "_ZN17LanguageSelectDlg4InitEv"),
    ("main_item", "_ZN14SettingMainDlg17UpdateListItemBtnEh"),
    ("sysinfo", "_ZN10SysInfoDlg10UpdateInfoEv"),
    ("dtor", "_ZN17LanguageSelectDlgD1Ev"),
]


# Settings > Appearance (a separate group: when one of these is not the known code, only the
# Appearance row is missing; the language list keeps working).
APPEARANCE_FUNCS = [
    ("main_create_list", "_ZN14SettingMainDlg10CreateListEv"),
    ("main_click", "_ZN14SettingMainDlg20OnListClickItemEventEP10ISkListBoxRNS0_19ClickItemEventParamE"),
    ("main_dtor", "_ZN14SettingMainDlgD1Ev"),
    ("click_before", "_ZN14SettingMainDlg17ButtonClickBeforeEv"),
    ("key2index", "_ZN9DyListBox9Key2IndexEh"),
    ("lang_key", "_ZN17LanguageSelectDlg3KeyEv"),
    ("lang_button", "_ZN17LanguageSelectDlg13OnButtonClickEP9ISkButtonRNS0_15ClickEventParamE"),
]
# Per build, for Appearance (read from the code):
# SettingMainDlg's last clicked key (OnListClickItemEvent stores it), Factory settings' key (the
# Appearance row goes before it: every row the stock code shows, hides or finds at run time comes
# before it), and the static scroll position CreateList restores (SettingMainDlg::
# m_ListFirstItemIndex, from .symtab). The main list's DyListBox is at this+48 in both.
APPEARANCE = {"b43ec55a1ad674df5b2d38a7e86a4e56": (148, 9), "ef39e20f6e43133fbbd3a2f04044be4b": (146, 7)}
# Per build: the main menu's "Language" row (UpdateListItemBtn's switch case) and where that
# function finds a row's ISkButton* (this + base + 8 * row), read from its code by hand:
#   PQ 0317: case 2; r7 = this + (row + 25) * 8, item = [r7 + 4]       -> base 204
#   MQB 0394: case 3; r7 = this + row * 8 + 204, item = [r7 + 4]       -> base 208
# and SysInfoDlg::UpdateInfo's six text rows (Allobject/Text_Enter_x1..x6): row k's
# ISkStaticTextCtrl* at this + 68 + 8 * k; rows 0-2 part number, hardware, software, then
# Bluetooth when this+36 is set, then Apple CarPlay; the next row is free (same in both builds).
MAIN_MENU = {"b43ec55a1ad674df5b2d38a7e86a4e56": (208, 3), "ef39e20f6e43133fbbd3a2f04044be4b": (204, 2)}
SYSINFO = (68, 36)


def fnv(b):
    h = 0x811C9DC5
    for c in b:
        h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
    return h


def symbols(e):
    """{name: (value, size)} from .symtab (the app keeps it) and .dynsym."""
    out = {}
    for tab, strs in ((".symtab", ".strtab"), (".dynsym", ".dynstr")):
        if tab not in e.sections:
            continue
        s, st = e.sections[tab], e.sections[strs]
        for i in range(s["size"] // 16):
            name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", e.data, s["off"] + i * 16)
            if shndx and info & 0xF == 2:  # STT_FUNC
                out.setdefault(e.cstr_at(st["off"] + name).decode("latin1"), (value & ~1, size))
    return out


def data_symbol(e, name):
    """The address of the object `name` in .symtab, or None."""
    s, st = e.sections[".symtab"], e.sections[".strtab"]
    for i in range(s["size"] // 16):
        n, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", e.data, s["off"] + i * 16)
        if shndx and info & 0xF == 1 and e.cstr_at(st["off"] + n).decode("latin1") == name:
            return value
    return None


def table(f, e, syms, name, funcs):
    f.write("static const SettingsFunc %s[] = {\n" % name)
    for key, sym in funcs:
        if sym not in syms:
            sys.exit("setting: %s not found" % sym)
        va, size = syms[sym]
        code = e.data[e.off(va):e.off(va) + size]
        f.write("    {\"%s\", 0x%x, 0x%x, 0x%08xu},\n" % (key, va, size, fnv(code)))
    f.write("};\n")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    e = fw.Elf(sys.argv[1])
    if e.type != 2 or e.off(0x8000) != 0:
        sys.exit("setting: not ET_EXEC with its text at 0x8000")
    syms = symbols(e)
    digest = hashlib.md5(e.data).hexdigest()
    if digest not in MAIN_MENU:
        sys.exit("setting %s: an unknown build; read its UpdateListItemBtn and add it to MAIN_MENU" % digest)
    with open(sys.argv[2], "w") as f:
        f.write("// Generated by i18n/gen_settings.py from setting (md5 %s). Not committed.\n" % digest)
        f.write("#define AA_MAIN_ITEM_BASE %d\n#define AA_MAIN_LANGUAGE_ROW %d\n" % MAIN_MENU[digest])
        f.write("#define AA_SYSINFO_ROWS %d\n#define AA_SYSINFO_BT %d\n" % SYSINFO)
        table(f, e, syms, "kSettingsFuncs", FUNCS)
        first = data_symbol(e, "_ZN14SettingMainDlg20m_ListFirstItemIndexE")
        if digest in APPEARANCE and first:
            f.write("#define AA_APPEARANCE 1\n#define AA_MAIN_LAST_KEY %d\n#define AA_MAIN_FACTORY_KEY %d\n" % APPEARANCE[digest])
            f.write("#define AA_MAIN_FIRST_INDEX 0x%x\n" % first)
            table(f, e, syms, "kAppearanceFuncs", APPEARANCE_FUNCS)
    print("%s: %d functions%s" % (sys.argv[2], len(FUNCS), ", Appearance %d" % len(APPEARANCE_FUNCS) if digest in APPEARANCE else ""))


if __name__ == "__main__":
    main()
