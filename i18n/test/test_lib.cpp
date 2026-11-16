// SPDX-License-Identifier: GPL-3.0-or-later
//
// Drives a test build of our libUIPublic.so (i18n/test_lib.sh) as an app would, on the unit's
// own loader under qemu. It plays the app's side: App() with the UI framework's string
// translator (items[id].row[column], as SkStringTranslatorPrivate::GetString reads them),
// SVP_GetLanguage() with the car's code, SVP_ChangeLanguage() logged.
//
//   test_lib <lib.so> [command]...   commands, in order:
//     car=N          the car's language code from now on
//     dump=FILE      every phrase: id, GetVWString, GetVWStringEx, row[0], row[1] (tab-separated,
//                    \ n t escaped), with the translator's table set the way the apps set it
//     write=CODE     writes the state record in place, as another process would
//     select=CODE    aa_lang_select(CODE)
//     info           the library's languages and the code in force
//     tramp          the Settings hooks' trampoline and jump, on lit_func below (a function that
//                    starts with a literal load, as SysInfoDlg::UpdateInfo does): prints
//                    "tramp <lit_func(5) through the trampoline> hooked <lit_func(5) once hooked>"
//     lit=E[,bad]    lit_text below (an app's own text, read as an app reads it: ldr + add pc) is
//                    taken as a site of own text E (literals.entries), as aa_lit_init takes an
//                    app's; ",bad" gives it a wrong stock word, so it must be refused. Prints
//                    "lit taken" or "lit refused"
//     lit=E,len[,badlen]  the same for lit_len_text below, read with its byte length as an
//                    immediate for a (const char*, size_t) call, as AppConnect's pop-ups read
//                    theirs; ",badlen" gives the length instruction a wrong stock value
//     littext        a phrase lookup (which applies a changed state), then "littext <lit_text()>"
//     litlentext     the same with lit_len_text(): the text cut at the length the code passes
//     bt=FILE        the Bluetooth app's pop-up hook on bt_add below (AddUserID's first two
//                    instructions; it answers id + 1000), reading FILE for "on screen": prints
//                    "bt <no file> <this pid, 1> <this pid, 0> <a pid not running, 1> <junk>"
//                    (0 where the pop-up is held back), then "bt refused" or "bt taken" for
//                    lit_func, whose code is not AddUserID's
// SVP_ChangeLanguage calls print "CHANGE n".
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef AA_LANG_STATE
#error AA_LANG_STATE
#endif

struct StringItem {
    int id;
    const char** row;
};

// ---- the app's side ----
static unsigned g_car;

struct Translator {
    void** vtbl;
    StringItem* items;
    int count;
    int cols;
    int col;
};
static void tr_set_table(Translator* t, StringItem* items, int count, int cols) {
    t->items = items;
    t->count = count;
    t->cols = cols;
}
static void tr_settled_column(Translator* t, int col) {
    t->col = col;
}
static const char* tr_get_string(Translator* t, int id) {
    if (t->col < 0 || t->col >= t->cols)
        return "";
    return t->items[id].row[t->col];
}
static void* g_tr_vtbl[8] = {0, 0, 0, 0, (void*)tr_set_table, (void*)tr_settled_column, (void*)tr_get_string, 0};
static Translator g_tr = {g_tr_vtbl, 0, 0, 2, 1};

struct AppIface {
    void** vtbl;
};
static Translator* app_get_translator(AppIface*) {
    return &g_tr;
}
static void* g_app_vtbl[24];
static AppIface g_app_iface = {g_app_vtbl};
static char g_app[0x40];

void* App() {
    return g_app;
}
int SVP_GetLanguage(unsigned int& lang) {
    lang = g_car;
    return 0;
}
int SVP_ChangeLanguage(unsigned int lang) {
    printf("CHANGE %u\n", lang);
    return 0;
}

// lit_func(x) = x + 0x12345, its constant loaded from a literal pool by its first instruction.
extern "C" int lit_func(int x);
asm(".text\n.arm\n.align 2\n.global lit_func\n.type lit_func, %function\nlit_func:\n"
    "\tldr r1, 1f\n\tpush {r4, lr}\n\tadd r0, r0, r1\n\tpop {r4, pc}\n.align 2\n1:\t.word 0x12345\n");
static int (*g_lit_orig)(int);

// lit_text() = an own text of the program, as an app reads one: its offset from the add's pc,
// loaded from the literal pool.
extern "C" const char* lit_text();
extern "C" unsigned lit_pool[], lit_add[];
asm(".text\n.arm\n.align 2\n.global lit_text\n.global lit_pool\n.global lit_add\n.type lit_text, %function\n"
    "lit_text:\n\tldr r0, lit_pool\nlit_add:\n\tadd r0, pc, r0\n\tbx lr\n.align 2\n"
    "lit_pool:\t.word lit_stock - (lit_add + 8)\n"
    ".section .rodata\nlit_stock:\t.asciz \"stock own text\"\n.text\n");
// lit_len_text() = the same own text read as AppConnect's pop-ups read theirs: with its byte
// length as an immediate, for a (const char*, size_t) call (std::string::assign).
extern "C" const char* lit_len_text();
extern "C" unsigned lit_len_pool[], lit_len_add[], lit_len_mov[];
extern "C" const char* lit_copy(int, const char* text, unsigned n) {
    static char buf[1024];
    n = n < sizeof buf - 1 ? n : sizeof buf - 1;
    memcpy(buf, text, n);
    buf[n] = 0;
    return buf;
}
asm(".text\n.arm\n.align 2\n.global lit_len_text\n.global lit_len_pool\n.global lit_len_add\n.global lit_len_mov\n"
    ".type lit_len_text, %function\nlit_len_text:\n\tpush {r4, lr}\n\tldr r1, lit_len_pool\n"
    "lit_len_mov:\n\tmov r2, #14\nlit_len_add:\n\tadd r1, pc, r1\n\tbl lit_copy\n\tpop {r4, pc}\n.align 2\n"
    "lit_len_pool:\t.word lit_stock - (lit_len_add + 8)\n.text\n");
// bt_add(self, id) = id + 1000, starting as BTPopupMgr::AddUserID does.
extern "C" int bt_add(void* self, int id);
asm(".text\n.arm\n.align 2\n.global bt_add\n.type bt_add, %function\nbt_add:\n"
    "\tpush {r4, r5, r6, lr}\n\tmov r4, r0\n\tadd r0, r1, #1000\n\tpop {r4, r5, r6, pc}\n");

static void put_file(const char* path, const char* text) {
    if (!text) {
        unlink(path);
        return;
    }
    FILE* f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static int lit_hook(int x) {
    return g_lit_orig(x) + 1;
}

static void esc(FILE* f, const char* s) {
    for (; *s; ++s) {
        if (*s == '\\')
            fputs("\\\\", f);
        else if (*s == '\n')
            fputs("\\n", f);
        else if (*s == '\t')
            fputs("\\t", f);
        else
            fputc(*s, f);
    }
}

int main(int argc, char** argv) {
    g_app_vtbl[0x4c / 4] = (void*)app_get_translator;
    *reinterpret_cast<AppIface**>(g_app + 0x24) = &g_app_iface;
    void* h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        printf("dlopen: %s\n", dlerror());
        return 1;
    }
    typedef StringItem* (*Items)();
    typedef const char* (*Get)(int);
    Items items = (Items)dlsym(h, "_Z14GetStringItemsv");
    Get vw = (Get)dlsym(h, "_Z11GetVWStringi");
    Get vwex = (Get)dlsym(h, "_Z13GetVWStringExi");
    StringItem* s_items = (StringItem*)dlsym(h, "s_Items");
    int (*count)() = (int (*)())dlsym(h, "_Z13aa_lang_countv");
    const char* (*code)(int) = (const char* (*)(int))dlsym(h, "_Z12aa_lang_codei");
    const char* (*name)(int) = (const char* (*)(int))dlsym(h, "_Z12aa_lang_namei");
    const char* (*flag)(int) = (const char* (*)(int))dlsym(h, "_Z12aa_lang_flagi");
    const char* (*current)() = (const char* (*)())dlsym(h, "_Z15aa_lang_currentv");
    int (*select)(const char*) = (int (*)(const char*))dlsym(h, "_Z14aa_lang_selectPKc");
    if (!items || !vw || !vwex || !s_items || !count || !select) {
        printf("missing symbols\n");
        return 1;
    }
    if (items() != s_items) {
        printf("GetStringItems() != s_Items\n");
        return 1;
    }
    int n = argc > 2 && strncmp(argv[2], "n=", 2) == 0 ? atoi(argv[2] + 2) : 0;
    tr_set_table(&g_tr, items(), n, 2);
    for (int a = 2; a < argc; ++a) {
        const char* c = argv[a];
        if (strncmp(c, "n=", 2) == 0) {
        } else if (strncmp(c, "car=", 4) == 0) {
            g_car = atoi(c + 4);
        } else if (strncmp(c, "dump=", 5) == 0) {
            FILE* f = fopen(c + 5, "w");
            for (int i = 0; i < n; ++i) {
                fprintf(f, "%d\t", i);
                esc(f, vw(i));
                fputc('\t', f);
                esc(f, vwex(i));
                fputc('\t', f);
                esc(f, s_items[i].row[0]);
                fputc('\t', f);
                esc(f, s_items[i].row[1]);
                fputc('\n', f);
            }
            fclose(f);
        } else if (strncmp(c, "write=", 6) == 0) {
            char r[33];
            snprintf(r, sizeof r, "AALANG1 %-23s\n", c + 6);
            int fd = open(AA_LANG_STATE, O_WRONLY);
            if (fd < 0 || pwrite(fd, r, 32, 0) != 32)
                printf("write failed\n");
            close(fd);
        } else if (strncmp(c, "select=", 7) == 0) {
            printf("select %s: %d\n", c + 7, select(c + 7));
        } else if (strcmp(c, "tramp") == 0) {
            void* (*tr)(unsigned) = (void* (*)(unsigned))dlsym(h, "_Z18aa_test_trampolinej");
            int (*wj)(unsigned, const void*) = (int (*)(unsigned, const void*))dlsym(h, "_Z18aa_test_write_jumpjPKv");
            if (!tr || !wj) {
                printf("tramp: missing symbols\n");
                return 1;
            }
            g_lit_orig = (int (*)(int))tr((unsigned)(unsigned long)lit_func);
            int a = g_lit_orig ? g_lit_orig(5) : -1;
            int b = wj((unsigned)(unsigned long)lit_func, (const void*)lit_hook) == 0 ? lit_func(5) : -1;
            printf("tramp %d hooked %d\n", a, b);
        } else if (strncmp(c, "lit=", 4) == 0) {
            typedef int (*Lit)(unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned);
            Lit lit = (Lit)dlsym(h, "_Z11aa_test_litjjjjjjj");
            if (!lit) {
                printf("lit: missing symbol\n");
                return 1;
            }
            int r;
            if (strstr(c, ",len")) {  // the site with its length (lit_len_text)
                unsigned mov = lit_len_mov[0] + (strstr(c, ",badlen") ? 1 : 0);
                r = lit((unsigned)(unsigned long)lit_len_pool, (unsigned)(unsigned long)lit_len_add, atoi(c + 4),
                        lit_len_pool[0], lit_len_add[0], (unsigned)(unsigned long)lit_len_mov, mov);
            } else {
                unsigned word = lit_pool[0] + (strstr(c, ",bad") ? 1 : 0);
                r = lit((unsigned)(unsigned long)lit_pool, (unsigned)(unsigned long)lit_add, atoi(c + 4), word, lit_add[0], 0,
                        0);
            }
            printf("lit %s\n", r == 0 ? "taken" : "refused");
        } else if (strncmp(c, "bt=", 3) == 0) {
            int (*bt)(unsigned, const char*) = (int (*)(unsigned, const char*))dlsym(h, "_Z10aa_test_btjPKc");
            if (!bt) {
                printf("bt: missing symbol\n");
                return 1;
            }
            const char* path = c + 3;
            if (bt((unsigned)(unsigned long)bt_add, path) != 0) {
                printf("bt: not hooked\n");
                return 1;
            }
            char mine[32], gone[32];
            snprintf(mine, sizeof mine, "%d 1\n", (int)getpid());
            snprintf(gone, sizeof gone, "%d 0\n", (int)getpid());
            const char* files[] = {0, mine, gone, "999999 1\n", "on screen\n"};
            printf("bt");
            for (unsigned i = 0; i < sizeof files / sizeof files[0]; ++i) {
                put_file(path, files[i]);
                printf(" %d", bt_add(0, 7));
            }
            printf("\n");
            put_file(path, 0);
            printf("bt %s\n", bt((unsigned)(unsigned long)lit_func, path) == 0 ? "taken" : "refused");
        } else if (strcmp(c, "litlentext") == 0) {
            vwex(0);
            printf("littext ");
            esc(stdout, lit_len_text());
            printf("\n");
        } else if (strcmp(c, "littext") == 0) {
            vwex(0);
            printf("littext ");
            esc(stdout, lit_text());
            printf("\n");
        } else if (strcmp(c, "info") == 0) {
            printf("current %s; languages", current());
            for (int i = 0; i < count(); ++i)
                printf(" %s(%s|%s)", code(i), name(i), flag(i) ? flag(i) : "-");
            printf("\n");
        } else {
            printf("unknown command %s\n", c);
            return 1;
        }
    }
    return 0;
}
