// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Settings app's language list, with every language our library offers.
//
// Stock (setting, LanguageSelectDlg; addresses per build in AA_SETTINGS, from
// i18n/gen_settings.py): Init creates four list items from prefabs, Chinese and English, each
// with a "_Disable" twin that marks the current language (dimmed, no click sound), stored as
// SkButtonPtr members {vtable, ISkButton*} at +84/+92 (Chinese), +100/+108 (English). CreateList
// empties the list (ISkListBox vtable slot 184/4) and adds two items (slot 140/4), chosen by the
// car's code. UpdateList sets their texts: phrases 1416 "中文" and 1413 "English (US)". A click
// (OnListClickItemEvent: index at param+8) stores the car code at this+44 and posts event 24,
// whose handler calls SVP_ChangeLanguage.
//
// Here, in the setting process only, and only when every function below has exactly the code
// of a known build (address, size, FNV-1a hash of its bytes; else the stock list stays):
//   CreateList  replaced: one item per language in aa_lang.bin's order (by name), the current
//               one (our state) as its Disable twin. English and Chinese are the stock items; the other
//               languages' items are made once per dialog from the English prefabs, set up as
//               Init sets up the stock ones.
//               The list's node is stock 13.7 units tall, from y 4.53 down to -9.17, below the
//               screen's bottom (about -6): it lays out up to seven items and never scrolls, the
//               seventh off screen. It is made six rows tall (10.62, 6 x 1.77) with the same top,
//               as the main menu's list (10.5), so it scrolls; the current language is scrolled
//               into view (JumpItemToVisibleCenter).
//   UpdateList  replaced: the stock UpdateList (through a trampoline) sets the stock items'
//               texts; then, for each other language, its items take the Chinese slots and
//               phrase 1416 reads that language's own name while the stock UpdateList runs again.
//   OnListClickItemEvent  replaced: the item's language is stored (aa_lang_select). Chinese, or
//               a language other than Chinese while the car says Chinese, goes through the stock
//               event 24 (the car's code follows, and its answer refreshes every app). Otherwise
//               the car is not involved, and every app is told the language changed: message
//               0x203 (OnUpdateLanguage) with code 2 and then 0, so a handler that ignores an
//               unchanged code still runs; every text then reads our rows again.
//   SysInfoDlg::UpdateInfo  wrapped: after the stock rows (part number, hardware, software, then
//               Bluetooth if present, then Apple CarPlay), the next of the screen's six text rows
//               reads "Android Auto: " + /svp/etc/aa_android_auto_version (LIVI-<build date>-<LIVI
//               commit>, written by the update build from the receiver's stamp; a release's label
//               instead: "Public Beta 1 (LIVI)"), as the stock
//               rows join their caption (phrase 1463, "Android Auto:") and value. Init hides the
//               rows the stock code leaves (SetVisible(false), slot 48) and gives the used ones the
//               light font: the row is set up the same way, then shown.
//   SettingMainDlg::UpdateListItemBtn  wrapped (its first two instructions run from a trampoline):
//               after the stock code, the "Language" row's flag
//               (child "Switch_Key_ON&OFF/1", shown for any code but Chinese) shows the current
//               language's flag.
//   ~LanguageSelectDlg  wrapped: the app makes a new dialog each time the screen is opened
//               (CreateObject) and deletes it on leaving; the dialog's slot here, with its items
//               (released as the stock destructor releases its own: ISkButton slots 3 then 1),
//               is freed first. (Without it the four slots filled up: the fifth visit got the
//               stock list and ignored taps, and a new dialog at a freed one's address got its
//               stale items.)
// The screen, made like System settings (SystemSetupLayer_Prefab) once per dialog: that layer
// has the list drawn under a 63-pixel header plane (Mat_Bg2: texture
// Mat_BT_Balance_Background_02, black with the orange rule in its last row; y 5.42, z 0.05,
// in front of the items, whose parts sit up to z 0.017 above the list, and behind the title at
// 0.06) and a scroll bar (the MediaSlider prefab, z 0.08) beside the list moved left (x -0.9).
// The language layer has neither (it never needed to scroll; its background texture draws a
// header bar and rule of its own, behind everything, so dragged rows went over it). The header
// is System settings' own Mat_Bg2 node, taken out of one SystemSetupLayer_Prefab instance per
// process and lent to each language screen in front of the list (the dialog's destructor takes
// it back); the title moves from z 0.02 to 0.06. (A copy of a plane, kzuObjectNodeCopy, is not
// drawn at all.) The scroll bar is a MediaSlider instance tied to the list (SkScrollBarCtrlExPtr::
// CreateInstanceFromObjectNode, ISkListBox slot 332 RelateScrollBar, as SettingMainDlg::Init
// does). They are named AA_Header and AA_MediaSlider, the scroll bar made once per layer. If any
// of it is missing, the list scrolls without them.
// The items: the language prefabs' background plane is 15.05 units wide, System settings'
// 13.6, both stretched by the root's Btn_Plan-Width (1.3, bound to the plane's scale): 19.6 and
// 17.7. Set to 1.3 x 13.6 / 15.05, the items are as wide as System settings' rows, with the
// press highlight (Plane_Fcous, 19.5 wide, unbound) scaled alike and the flag and name moved
// right by the half difference, to where Btn_Language_Prefab has them.
// Flags: every item's "Mat_Icon" plane (and that row's switch child) gets its language's flag
// (aa_lang.bin names the picture, /svp/share/aa_lang_<flag>.png; English and Chinese keep the stock pictures)
// through Kanzi's own calls, as SkStaticPictureCtrlBase::SetTexture does it: the PNG decoded
// (kzcImageLoadFile), made a texture (kzuImageTextureCreateFromImage), set as the node's
// Texture property (type 140, kzuObjectNodeSetResourceIDResourceProperty). One texture per
// picture, made once. If any of it is missing or fails, the items keep the stock pictures.
// Each function is replaced by an 8-byte jump at its entry (ldr pc, [pc, #-4]), written into the
// process's private copy of the text (mprotect), as the shim patches AppConnect.
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <ucontext.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <syslog.h>
#include <unistd.h>

#include "aa_lang.h"

const char* GetVWString(int id);

namespace {

struct SettingsFunc {
    const char* name;
    uint32_t addr;
    uint32_t size;
    uint32_t fnv;
};
#ifdef AA_SETTINGS
#include AA_SETTINGS
#else
const SettingsFunc kSettingsFuncs[] = {{"none", 0, 0, 0}};
#define AA_MAIN_ITEM_BASE 0
#define AA_MAIN_LANGUAGE_ROW 0
#define AA_SYSINFO_ROWS 0
#define AA_SYSINFO_BT 0
#endif
#ifndef AA_VERSION_FILE
#define AA_VERSION_FILE "/svp/etc/aa_android_auto_version"
#endif
const int kPhraseAndroidAuto = 1463;  // "Android Auto:", the caption other variants use there

const int kMaxExtra = 30;  // languages besides English and Chinese
const int kPhraseChinese = 1416;  // the stock list's Chinese item reads this phrase
// The apps that use the phrase book (their manifests' ids): statusbar, launcher, toptip, camera,
// setting, update, engineering, radio, bluetooth, mediaplayer, carmenu, climate, appconnect.
const int kApps[] = {13, 14, 16, 18, 41, 47, 48, 62, 66, 151, 152, 153, 154};

struct ButtonPtr {  // SkButtonPtr: {vtable, ISkButton*}
    uint32_t vtbl;
    void* item;
};
struct Dialog {
    void* self;
    bool fitted;
    bool decorated;
    ButtonPtr bar;     // SkScrollBarCtrlExPtr: {vtable, ISkScrollBarCtrlEx*}
    ButtonPtr slider;  // SkObjectNodePtr: {vtable, ISkObjectNode*}, the scroll bar's node
    ButtonPtr norm[kMaxExtra];
    ButtonPtr dis[kMaxExtra];
    bool ok[kMaxExtra];  // both items made, each with a real Kanzi node
    int shown[kMaxExtra + 2];  // the list as built: position -> language index (or page item)
    int count;
    // Settings > Appearance's pages (modes of this dialog): items per page, made on first use.
    ButtonPtr page_norm[3][13];
    ButtonPtr page_dis[3][13];
    bool page_ok[3][13];
};
Dialog g_dialogs[4];

// Settings > Appearance (appearance.cpp): the main
// list's row opens this dialog in the Appearance mode; its pages are modes of the language
// dialog, switched in place (the dispatcher will not switch to the dialog already shown).
enum Mode { kLanguage, kAppearance, kTheme, kLogo };
Mode g_mode = kLanguage;
const int kPageMax = 13;  // the start screen page: Automatic and the 12 logos
const int kPhraseSound = 1428;  // the main list's row 0, whose stock setup gives ours its text

typedef void (*Method0)(void*);
typedef void (*MainItem)(void*, unsigned char);
Method0 g_orig_update_list;  // trampolines to the stock code
MainItem g_orig_main_item;
Method0 g_orig_sysinfo;
void* (*g_orig_dtor)(void*);
uint32_t g_create_list, g_update_list, g_click, g_main_item, g_sysinfo, g_dtor;

typedef void (*CreateFromPrefab)(void*, const char*, const char*);
typedef void (*Ctl)(void*, int);
typedef void (*CtlEnd)(void*);
typedef void* (*GetWS)();
typedef int (*PostMsgEx)(int, unsigned, unsigned, long, int, int);
typedef void* (*LoadRelativeNode)(void*, const char*);
typedef void* (*InstanceFromPrefab)(const char*, const char*);
typedef void (*BarFromNode)(void*, void*);
typedef void (*NodeAssign)(void*, void*);
CreateFromPrefab g_create;
LoadRelativeNode g_load_rel;     // SVPBaseDlg::LoadRelativeNode
InstanceFromPrefab g_instance;   // SkObjectNodePtr::InstanceFromPrefabTemplate (static)
BarFromNode g_bar_create;        // SkScrollBarCtrlExPtr::CreateInstanceFromObjectNode
NodeAssign g_node_assign;        // SkInterfaceMgr<ISkObjectNode>::Equal (the app's own)
uint32_t g_bar_vtbl, g_node_vtbl;  // SkScrollBarCtrlExPtr's, SkObjectNodePtr's vtables + 8
Ctl g_ctl;
CtlEnd g_ctl_end;
GetWS g_get_ws;
PostMsgEx g_post;

// Kanzi (libUICore-kanzi), for the flags.
typedef void* (*FromPath)(void*, const char*);
typedef int (*ImageLoad)(void*, const char*, void**);
typedef int (*TextureFromImage)(void*, const char*, void*, int, int, float, void**);
typedef void* (*ToResource)(void*);
typedef void* (*NodeGet)(void*);
typedef int (*SetResource)(void*, void*, void*);
typedef void (*Invalidate)(void*);
struct Kanzi {
    FromPath from_path;
    void* (*memory)(const void*);  // kzcMemoryGetManager: the manager stored before any Kanzi allocation
    ImageLoad load;
    TextureFromImage texture;
    ToResource to_resource;
    NodeGet ui_domain;
    NodeGet resource_manager;
    SetResource set;
    Invalidate invalidate;
    void** texture_type;  // KZU_PROPERTY_TYPE_TEXTURE (a variable holding the type)
    // For the list's height (kzuObjectNodeGet/SetFloatProperty, the SRT render transformation).
    int (*get_float)(void*, void*, float*);  // found or not; the value through the pointer
    int (*set_float)(void*, void*, float);
    void (*get_srt)(void*, void*, float*, float*, float*);
    int (*set_srt)(void*, void*, const float*, const float*, const float*);
    void (*invalidate_layout)(void*);
    void** layout_height;     // KZU_PROPERTY_TYPE_LAYOUT_HEIGHT
    void** render_transform;  // KZU_PROPERTY_TYPE_RENDER_TRANSFORMATION
    // For the header plane: the layer's children.
    int (*add_child_at)(void*, unsigned, void*);      // kzuObjectNodeAddChildAtIndex(parent, i, child)
    int (*remove_child)(void*, void*);                // kzuObjectNodeRemoveChild(parent, child)
    void* (*get_parent)(void*);                       // kzuObjectNodeGetParent
    unsigned (*child_count)(void*);
    void* (*child_at)(void*, unsigned);
    const char* (*get_name)(void*);
    int (*set_name)(void*, const char*);
    void* (*find_type)(const char*);                  // kzuPropertyRegistryFindPropertyType
    void* (*float_type)(void*);                       // kzuFloatPropertyTypeFromPropertyType
} g_kz;
bool g_fit;
bool g_decor;
const float kListStockHeight = 13.7f;
const float kListHeight = 10.62f;
bool g_flags;
struct Flag {
    const char* path;
    void* resource;
} g_flag_cache[kMaxExtra + 2 + 8];  // the flags, the theme swatches, the blank picture

uint32_t fnv(const uint8_t* p, uint32_t n) {
    uint32_t h = 0x811C9DC5u;
    while (n--)
        h = (h ^ *p++) * 0x01000193u;
    return h;
}

const SettingsFunc* func(const char* name) {
    for (size_t i = 0; i < sizeof kSettingsFuncs / sizeof kSettingsFuncs[0]; ++i)
        if (strcmp(kSettingsFuncs[i].name, name) == 0)
            return &kSettingsFuncs[i];
    return 0;
}

void* vfn(void* obj, int offset) {
    return (*reinterpret_cast<void***>(obj))[offset / 4];
}

bool write_jump(uint32_t addr, const void* target) {
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t start = addr & ~(page - 1), end = (addr + 8 + page - 1) & ~(page - 1);
    if (mprotect(reinterpret_cast<void*>(start), end - start, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return false;
    uint32_t* p = reinterpret_cast<uint32_t*>(addr);
    p[0] = 0xe51ff004u;  // ldr pc, [pc, #-4]
    p[1] = reinterpret_cast<uint32_t>(target);
    __builtin___clear_cache(reinterpret_cast<char*>(p), reinterpret_cast<char*>(p + 2));
    mprotect(reinterpret_cast<void*>(start), end - start, PROT_READ | PROT_EXEC);
    return true;
}

// A literal load, "ldr rd, [pc, #+-imm]" (always executed).
bool ldr_literal(uint32_t w) {
    return (w & 0xff7f0000u) == 0xe51f0000u && ((w >> 12) & 0xf) != 15;
}

// An instruction that reads or writes pc other than a literal load cannot be moved.
bool uses_pc(uint32_t w) {
    return ((w >> 16) & 0xf) == 15 || ((w >> 12) & 0xf) == 15 || (w & 0xf) == 15;
}

// The stock function's first two instructions, then a jump to the rest. They must not depend on
// where they run (the code is checked through its hash); a literal load as the first one is
// moved with its value: "ldr rd, [pc, #8]" reading a copy of the word placed after the jump.
Method0 trampoline(uint32_t addr) {
    const uint32_t* f = reinterpret_cast<const uint32_t*>(addr);
    bool literal = ldr_literal(f[0]);
    if ((!literal && uses_pc(f[0])) || uses_pc(f[1]) || ldr_literal(f[1]))
        return 0;
    void* m = mmap(0, 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED)
        return 0;
    uint32_t* t = static_cast<uint32_t*>(m);
    if (literal) {
        uint32_t imm = f[0] & 0xfff;
        uint32_t at = (f[0] & (1u << 23)) ? addr + 8 + imm : addr + 8 - imm;
        t[0] = 0xe59f0008u | (f[0] & 0xf000u);  // ldr rd, [pc, #8] -> t[4]
        t[4] = *reinterpret_cast<const uint32_t*>(at);
    } else {
        t[0] = f[0];
    }
    t[1] = f[1];
    t[2] = 0xe51ff004u;
    t[3] = addr + 8;
    __builtin___clear_cache(reinterpret_cast<char*>(t), reinterpret_cast<char*>(t + 5));
    return reinterpret_cast<Method0>(m);
}

// The flag picture as a Kanzi texture resource, made once per picture.
void* flag_resource(void* kz_node, const char* path) {
    for (int i = 0; i < kMaxExtra + 2 + 8; ++i)
        if (g_flag_cache[i].path && strcmp(g_flag_cache[i].path, path) == 0)
            return g_flag_cache[i].resource;
    void* mm = g_kz.memory(kz_node);
    void* image = 0;
    void* texture = 0;
    void* rm = g_kz.resource_manager(g_kz.ui_domain(kz_node));
    if (!mm || !rm || g_kz.load(mm, path, &image) != 0 || !image ||
        g_kz.texture(rm, "aa-lang-flag", image, 2, 2, 0.0f, &texture) != 0 || !texture) {
        syslog(LOG_WARNING, "aa-lang: settings: cannot load the flag %s", path);
        return 0;
    }
    void* res = g_kz.to_resource(texture);
    for (int i = 0; i < kMaxExtra + 2 + 8; ++i)
        if (!g_flag_cache[i].path) {
            g_flag_cache[i].path = strdup(path);  // its own copy: a caller may pass a buffer it reuses
            g_flag_cache[i].resource = res;
            break;
        }
    return res;
}

// Shows the picture at `path` on the plane `child` of an item (an ISkButton*) or of a node.
void set_picture(void* node, const char* child, const char* path) {
    if (!g_flags || !node || !path)
        return;
    void* icon = g_kz.from_path(node, child);
    if (!icon || reinterpret_cast<bool (*)(void*)>(vfn(icon, 0x40))(icon))  // IsNullKanzi
        return;
    void* kz = *reinterpret_cast<void**>(static_cast<char*>(icon) + 0x50);  // its KzuObjectNode*
    void* res = kz ? flag_resource(kz, path) : 0;
    if (!res || g_kz.set(kz, *g_kz.texture_type, res) != 0)
        return;
    g_kz.invalidate(kz);
}

// Language i's flag.
void set_flag(void* node, const char* child, int i) {
    set_picture(node, child, aa_lang_flag(i));
}

void* object_node(void* item) {
    return item ? reinterpret_cast<void* (*)(void*)>(vfn(item, 56))(item) : 0;  // GetObjectNode
}

// An item whose prefab was found: its object node wraps a Kanzi node (ISkObjectNode slot 0x40,
// IsNullKanzi). A prefab URL that resolves to nothing still gives a wrapper, around nothing.
bool real_item(void* item) {
    void* node = object_node(item);
    return node && !reinterpret_cast<bool (*)(void*)>(vfn(node, 0x40))(node);
}

void* member(void* self, int offset) {
    return *reinterpret_cast<void**>(static_cast<char*>(self) + offset);
}

// The slot of language i among the items made from the English prefabs (every language but
// English and Chinese, which are the stock items), or -1. The list is in aa_lang.bin's order.
int extra_slot(int i) {
    const char* code = aa_lang_code(i);
    if (!code || strcmp(code, "en") == 0 || strcmp(code, "zh") == 0)
        return -1;
    int k = 0;
    for (int j = 0; j < i; ++j) {
        const char* c = aa_lang_code(j);
        k += strcmp(c, "en") != 0 && strcmp(c, "zh") != 0;
    }
    return k < kMaxExtra ? k : -1;
}

void narrow_item(void* item);
void bare_item(void* item);

// One list item from the English prefabs and its Disable twin (the current choice: dimmed, no
// click sound), set up as LanguageSelectDlg::Init sets up its own, as wide as System settings'.
bool make_pair(ButtonPtr& norm, ButtonPtr& dis, uint32_t vtbl, const char* name) {
    norm.vtbl = dis.vtbl = vtbl;
    g_create(&norm, "kzb://VW_MP2017/Prefabs/Btn_LanEN_Prefab", name);
    g_create(&dis, "kzb://VW_MP2017/Prefabs/Btn_LanEN_Disable_Prefab", name);
    if (!real_item(norm.item) || !real_item(dis.item))
        return false;
    typedef void (*SetInt)(void*, int);
    typedef void (*Sound)(void*, int*, int);
    reinterpret_cast<SetInt>(vfn(norm.item, 180))(norm.item, 1);
    int flags = 4;
    reinterpret_cast<Sound>(vfn(dis.item, 84))(dis.item, &flags, 0);
    flags = 1;
    reinterpret_cast<Sound>(vfn(dis.item, 84))(dis.item, &flags, 0);
    reinterpret_cast<SetInt>(vfn(dis.item, 180))(dis.item, 1);
    if (g_decor) {
        narrow_item(norm.item);
        narrow_item(dis.item);
    }
    return true;
}

// The other languages' items of a dialog, made on first use.
Dialog* dialog(void* self) {
    for (int d = 0; d < 4; ++d)
        if (g_dialogs[d].self == self)
            return &g_dialogs[d];
    for (int d = 0; d < 4; ++d) {
        Dialog& x = g_dialogs[d];
        if (x.self)
            continue;
        x.self = self;
        uint32_t vtbl = *reinterpret_cast<uint32_t*>(static_cast<char*>(self) + 84);  // an SkButtonPtr's
        for (int i = 0; i < aa_lang_count(); ++i) {
            int k = extra_slot(i);
            if (k < 0)
                continue;
            const char* code = aa_lang_code(i);
            if (!make_pair(x.norm[k], x.dis[k], vtbl, code)) {
                syslog(LOG_WARNING, "aa-lang: settings: no list item for %s", code);
                continue;
            }
            x.ok[k] = true;
            set_flag(object_node(x.norm[k].item), "Mat_Icon", i);
            set_flag(object_node(x.dis[k].item), "Mat_Icon", i);
        }
        if (g_decor)  // the stock items: Chinese, English, each with its Disable twin
            for (int off = 88; off <= 112; off += 8)
                narrow_item(member(self, off));
        return &x;
    }
    return 0;
}

// Releases a smart pointer's object as the stock destructor does (slot 3 with the holder, then
// slot 1), and empties it.
void release(ButtonPtr& p) {
    if (!p.item)
        return;
    reinterpret_cast<void (*)(void*, void*)>(vfn(p.item, 12))(p.item, &p);
    reinterpret_cast<void (*)(void*)>(vfn(p.item, 4))(p.item);
    p.item = 0;
}

// The dialog being deleted: its slot freed, its items released, then the stock destructor.
void* kz_of(void* node);
extern void* g_header;

void* hook_dtor(void* self) {
    void* parent = g_header ? g_kz.get_parent(g_header) : 0;
    if (parent && parent == kz_of(g_load_rel(self, "Allobject")))
        g_kz.remove_child(parent, g_header);  // lent by decorate; the next screen gets it again
    for (int d = 0; d < 4; ++d) {
        Dialog& x = g_dialogs[d];
        if (x.self != self)
            continue;
        release(x.bar);
        if (x.slider.item)
            g_node_assign(&x.slider, 0);
        for (int k = 0; k < kMaxExtra; ++k) {
            release(x.norm[k]);
            release(x.dis[k]);
        }
        for (int m = 0; m < 3; ++m)
            for (int k = 0; k < kPageMax; ++k) {
                release(x.page_norm[m][k]);
                release(x.page_dis[m][k]);
            }
        memset(&x, 0, sizeof x);
    }
    g_mode = kLanguage;  // the next entry is the Language row's, unless the Appearance row says so
    return g_orig_dtor(self);
}

void* kz_of(void* node) {  // an ISkObjectNode's KzuObjectNode*, or 0
    if (!node || reinterpret_cast<bool (*)(void*)>(vfn(node, 0x40))(node))  // IsNullKanzi
        return 0;
    return *reinterpret_cast<void**>(static_cast<char*>(node) + 0x50);
}

int child_index(void* parent, const char* name, void* node) {
    unsigned n = g_kz.child_count(parent);
    for (unsigned i = 0; i < n; ++i) {
        void* c = g_kz.child_at(parent, i);
        if (c && (c == node || (name && g_kz.get_name(c) && strcmp(g_kz.get_name(c), name) == 0)))
            return static_cast<int>(i);
    }
    return -1;
}

bool near(float a, float b) {
    return fabsf(a - b) < 0.005f;
}

const float kPlanWidth = 1.3f * 13.6f / 15.05f;  // Btn_Plan-Width giving System settings' width
const float kFocusScale = 13.6f * 1.3f / 19.5f;  // Plane_Fcous alike
const float kShift = (15.05f - 13.6f) * 1.3f / 2;  // the flag and name, to the right
// System settings' rows without a picture (Sound, Bluetooth...) start their name 46 px left of
// where the rows with a flag do (47 against 93, measured on screen). The rows are 677 px and
// 1.3 x 13.6 units wide: 38.3 px per unit.
const float kBareShift = -46.0f * 1.3f * 13.6f / 677.0f;

// The Btn_Plan-Width float property type, looked up on first use: Kanzi's property registry exists
// only once the app's engine runs (kzuPropertyRegistryCreate), never in the library constructor,
// where a lookup reads a null map and kills the app.
void* plan_width() {
    static void* type;
    static bool warned;
    if (!type && g_kz.find_type && g_kz.float_type) {
        void* plan = g_kz.find_type("Btn_Plan-Width");
        type = plan ? g_kz.float_type(plan) : 0;
        if (!type && !warned) {
            warned = true;
            syslog(LOG_WARNING, "aa-lang: settings: no Btn_Plan-Width property; the items keep their width");
        }
    }
    return type;
}

// One language item as wide as System settings' rows (see the top of this file).
void narrow_item(void* item) {
    void* kz = kz_of(object_node(item));
    void* width = kz ? plan_width() : 0;
    if (!width)
        return;
    float w = 0;
    if (!g_kz.get_float(kz, width, &w) || !near(w, 1.3f))
        return;  // not the stock item, or done already
    g_kz.set_float(kz, width, kPlanWidth);
    float scale[3], rotation[3], translation[3];
    const char* parts[] = {"Plane_Fcous", "Mat_Icon", "CellX3Text_Node"};
    for (size_t p = 0; p < 3; ++p) {
        int i = child_index(kz, parts[p], 0);
        if (i < 0)
            continue;
        void* c = g_kz.child_at(kz, i);
        g_kz.get_srt(c, *g_kz.render_transform, scale, rotation, translation);
        if (p == 0)
            scale[0] *= kFocusScale;
        else
            translation[0] += kShift;
        g_kz.set_srt(c, *g_kz.render_transform, scale, rotation, translation);
    }
    g_kz.invalidate_layout(kz);
}

// An item with no picture: its name moved into the picture's place, as System settings' plain
// rows have it. Once per item (the items are kept); the picture plane stays, blank.
void bare_item(void* item) {
    void* kz = kz_of(object_node(item));
    int i = kz ? child_index(kz, "CellX3Text_Node", 0) : -1;
    if (i < 0)
        return;
    void* c = g_kz.child_at(kz, i);
    float scale[3], rotation[3], translation[3];
    g_kz.get_srt(c, *g_kz.render_transform, scale, rotation, translation);
    translation[0] += kBareShift;
    g_kz.set_srt(c, *g_kz.render_transform, scale, rotation, translation);
    g_kz.invalidate_layout(kz);
}

// A node named `name` at most `depth` levels below `node`, or 0.
void* find_node(void* node, const char* name, int depth) {
    unsigned n = g_kz.child_count(node);
    for (unsigned i = 0; i < n; ++i) {
        void* c = g_kz.child_at(node, i);
        if (c && g_kz.get_name(c) && strcmp(g_kz.get_name(c), name) == 0)
            return c;
    }
    for (unsigned i = 0; depth > 0 && i < n; ++i) {
        void* c = g_kz.child_at(node, i);
        void* f = c ? find_node(c, name, depth - 1) : 0;
        if (f)
            return f;
    }
    return 0;
}

// System settings' own header plane (Mat_Bg2: HeadLine material, the theme-coloured rule), taken
// out of one instance of its layer prefab per process, never shown, and lent to each language
// screen: added in front of the list here, taken back by the dialog's destructor (hook_dtor). A
// copy (kzuObjectNodeCopy) of a plane is not drawn at all: the screen's own background texture
// has a header bar and rule of its own at the bottom of the scene, so the rows went over it while
// the list was dragged.
void* g_header;

void* stock_header() {
    static bool tried;
    if (!tried) {
        tried = true;
        void* layer = kz_of(g_instance("kzb://VW_MP2017/Prefabs/SystemSetupLayer_Prefab", "AA_SystemSetupSource"));
        void* node = layer ? find_node(layer, "Mat_Bg2", 2) : 0;
        void* parent = node ? g_kz.get_parent(node) : 0;
        if (parent && g_kz.remove_child(parent, node) == 0) {
            g_kz.set_name(node, "AA_Header");
            g_header = node;
        } else {
            syslog(LOG_WARNING, "aa-lang: settings: no Mat_Bg2 in SystemSetupLayer_Prefab; the list scrolls without a header");
        }
    }
    return g_header;
}

// The header plane and the scroll bar (see the top of this file).
void decorate(void* self, Dialog* x, void* list) {
    void* all = g_load_rel(self, "Allobject");
    void* kz_all = kz_of(all);
    void* kz_list = kz_of(object_node(list));
    if (!kz_all || !kz_list)
        return;
    float scale[3], rotation[3], translation[3];
    void* head = stock_header();
    bool header = head && g_kz.get_parent(head) == kz_all;
    if (head && !g_kz.get_parent(head))
        header = g_kz.add_child_at(kz_all, child_index(kz_all, 0, kz_list) + 1, head) == 0;
    int title = child_index(kz_all, "Text_Title", 0);
    if (header && title >= 0) {  // in front of the header, as in System settings
        void* t = g_kz.child_at(kz_all, title);
        g_kz.get_srt(t, *g_kz.render_transform, scale, rotation, translation);
        if (near(translation[2], 0.02f)) {
            translation[2] = 0.06f;
            g_kz.set_srt(t, *g_kz.render_transform, scale, rotation, translation);
        }
    }
    void* slider = 0;
    if (child_index(kz_all, "AA_MediaSlider", 0) >= 0) {
        slider = g_load_rel(self, "Allobject/AA_MediaSlider");
    } else {
        slider = g_instance("kzb://VW_MP2017/Prefabs/MediaSlider", "AA_MediaSlider");
        void* kz = kz_of(slider);
        if (kz) {
            reinterpret_cast<void (*)(void*, void*)>(vfn(all, 68))(all, slider);  // AddChild
            g_kz.set_name(kz, "AA_MediaSlider");
            g_kz.get_srt(kz, *g_kz.render_transform, scale, rotation, translation);
            translation[0] = -0.03f;
            translation[1] = 0;
            translation[2] = 0.08f;
            g_kz.set_srt(kz, *g_kz.render_transform, scale, rotation, translation);
        }
    }
    if (kz_of(slider)) {
        x->slider.vtbl = g_node_vtbl;
        g_node_assign(&x->slider, slider);
        x->bar.vtbl = g_bar_vtbl;
        g_bar_create(&x->bar, slider);
    }
    if (x->bar.item) {
        reinterpret_cast<void (*)(void*, void*)>(vfn(list, 332))(list, x->bar.item);  // RelateScrollBar
        g_kz.get_srt(kz_list, *g_kz.render_transform, scale, rotation, translation);
        if (near(translation[0], -0.01f)) {
            translation[0] = -0.9f;
            g_kz.set_srt(kz_list, *g_kz.render_transform, scale, rotation, translation);
        }
    }
    g_kz.invalidate_layout(kz_all);
    syslog(LOG_INFO, "aa-lang: settings: language screen: header %s, scroll bar %s", header ? "yes" : "no",
           x->bar.item ? "yes" : "no");
}

void* item_for(void* self, Dialog* x, int i, bool current) {
    const char* code = aa_lang_code(i);
    if (strcmp(code, "en") == 0)
        return member(self, current ? 112 : 104);
    if (strcmp(code, "zh") == 0)
        return member(self, current ? 96 : 88);
    int k = extra_slot(i);
    if (!x || k < 0 || !x->ok[k])
        return 0;
    return current ? x->dis[k].item : x->norm[k].item;
}

// The list made six rows tall, its top kept (see the top of this file). Only from the stock height.
void fit_list(void* list) {
    void* node = object_node(list);
    void* kz = node ? *reinterpret_cast<void**>(static_cast<char*>(node) + 0x50) : 0;
    if (!g_fit || !kz)
        return;
    float h = 0;
    if (!g_kz.get_float(kz, *g_kz.layout_height, &h) || fabsf(h - kListStockHeight) > 0.01f) {
        syslog(LOG_INFO, "aa-lang: settings: the list is %.2f tall, not the stock %.2f; left as it is", h, kListStockHeight);
        return;
    }
    float scale[3], rotation[3], translation[3];
    g_kz.get_srt(kz, *g_kz.render_transform, scale, rotation, translation);
    translation[1] += (h - kListHeight) / 2;
    if (g_kz.set_srt(kz, *g_kz.render_transform, scale, rotation, translation) != 0 ||
        g_kz.set_float(kz, *g_kz.layout_height, kListHeight) != 0) {
        syslog(LOG_WARNING, "aa-lang: settings: cannot resize the list");
        return;
    }
    g_kz.invalidate_layout(kz);
}

// ---- Settings > Appearance's pages ----

int page_count(Mode m) {
    return m == kAppearance ? 2 : m == kTheme ? aa_theme_count() : m == kLogo ? aa_logo_count() : 0;
}

int page_current(Mode m) {  // the item shown as current (its Disable twin), -1 none
    return m == kTheme ? aa_theme_current() : m == kLogo ? aa_logo_current() : -1;
}

const char* page_text(Mode m, int k) {
    if (m == kAppearance)
        return aa_text(k == 0 ? "theme" : "logo");
    return m == kTheme ? aa_theme_name(k) : aa_logo_name(k);
}

const char* page_title(Mode m) {
    return aa_text(m == kAppearance ? "appearance" : m == kTheme ? "theme" : "logo");
}

// Whether page item k of mode m shows a picture: the colour rows only.
bool page_has_picture(Mode m, int k) {
    return m == kTheme || (m == kAppearance && k == 0);
}

// The item's picture: the theme's swatch (on the Appearance page, the current theme's), else none
// (the prefab's own picture is the US flag; such an item has its name where the picture would be).
const char* page_picture(Mode m, int k, char* buf, size_t n) {
    if (page_has_picture(m, k)) {
        snprintf(buf, n, "/svp/share/aa_theme_%s.png", aa_theme_key(m == kTheme ? k : aa_theme_current()));
        return buf;
    }
    return "/svp/share/aa_blank.png";
}

void set_title(void* self, const char* text) {
    void* title = member(self, 72);  // ISkStaticTextCtrl*
    if (title && text)
        reinterpret_cast<void (*)(void*, const char*, int)>(vfn(title, 64))(title, text, 0);  // SetText
}

// Page item k of mode m, made on first use (the current choice as the Disable twin).
void* page_item(void* self, Dialog* x, Mode m, int k, bool current) {
    int p = m - 1;
    if (!x->page_ok[p][k]) {
        char name[16];
        snprintf(name, sizeof name, "aa-page-%d-%d", m, k);
        uint32_t vtbl = *reinterpret_cast<uint32_t*>(static_cast<char*>(self) + 84);  // an SkButtonPtr's
        if (!make_pair(x->page_norm[p][k], x->page_dis[p][k], vtbl, name))
            return 0;
        x->page_ok[p][k] = true;
        if (!page_has_picture(m, k)) {
            bare_item(x->page_norm[p][k].item);
            bare_item(x->page_dis[p][k].item);
        }
    }
    return current ? x->page_dis[p][k].item : x->page_norm[p][k].item;
}

void build_page(void* self, Dialog* x, void* list) {
    int cur = page_current(g_mode);
    for (int k = 0; k < page_count(g_mode) && k < kPageMax; ++k) {
        void* item = page_item(self, x, g_mode, k, k == cur);
        if (!item)
            continue;
        char buf[64];
        set_picture(object_node(item), "Mat_Icon", page_picture(g_mode, k, buf, sizeof buf));
        reinterpret_cast<void (*)(void*, void*)>(vfn(list, 140))(list, item);  // AddItem
        x->shown[x->count++] = k;
    }
}

void hook_create_list(void* self) {
    Dialog* x = dialog(self);
    char ctl[16];
    g_ctl(ctl, 3);
    void* list = member(self, 80);
    if (x && !x->fitted) {
        x->fitted = true;
        fit_list(list);
    }
    if (x && !x->decorated && g_decor) {
        x->decorated = true;
        decorate(self, x, list);
    }
    reinterpret_cast<Method0>(vfn(list, 184))(list);  // RemoveAllItems
    const char* cur = aa_lang_current();
    if (x)
        x->count = 0;
    if (g_mode != kLanguage && x) {
        build_page(self, x, list);
        g_ctl_end(ctl);
        // The current choice into view (a page without one: rebuild()).
        int c = page_current(g_mode), at = -1;
        for (int k = 0; k < x->count; ++k)
            if (x->shown[k] == c)
                at = k;
        if (at >= 0)
            reinterpret_cast<void (*)(void*, int)>(vfn(list, 232))(list, at);  // JumpItemToVisibleCenter
        return;
    }
    for (int i = 0; i < aa_lang_count() && i < kMaxExtra + 2; ++i) {
        void* item = item_for(self, x, i, strcmp(aa_lang_code(i), cur) == 0);
        if (!item)
            continue;
        reinterpret_cast<void (*)(void*, void*)>(vfn(list, 140))(list, item);  // AddItem
        if (x)
            x->shown[x->count++] = i;
    }
    g_ctl_end(ctl);
    for (int k = 0; x && k < x->count; ++k)
        if (strcmp(aa_lang_code(x->shown[k]), cur) == 0)
            reinterpret_cast<void (*)(void*, int)>(vfn(list, 232))(list, k);  // JumpItemToVisibleCenter
}

void hook_update_list(void* self) {
    g_orig_update_list(self);
    Dialog* x = dialog(self);
    if (!x)
        return;
    void** ch = reinterpret_cast<void**>(static_cast<char*>(self) + 88);
    void** chd = reinterpret_cast<void**>(static_cast<char*>(self) + 96);
    void* save_ch = *ch;
    void* save_chd = *chd;
    if (g_mode != kLanguage) {  // the page's texts, through the Chinese items' phrase as below
        int p = g_mode - 1;
        for (int k = 0; k < page_count(g_mode) && k < kPageMax; ++k) {
            if (!x->page_ok[p][k])
                continue;
            *ch = x->page_norm[p][k].item;
            *chd = x->page_dis[p][k].item;
            aa_lang_phrase_override(kPhraseChinese, page_text(g_mode, k));
            g_orig_update_list(self);
        }
        aa_lang_phrase_override(kPhraseChinese, 0);
        *ch = save_ch;
        *chd = save_chd;
        set_title(self, page_title(g_mode));
        return;
    }
    for (int i = 0; i < aa_lang_count(); ++i) {
        int k = extra_slot(i);
        if (k < 0 || !x->ok[k])
            continue;
        *ch = x->norm[k].item;
        *chd = x->dis[k].item;
        const char* name = aa_lang_name(i);
        aa_lang_phrase_override(kPhraseChinese, name);
        g_orig_update_list(self);
    }
    aa_lang_phrase_override(kPhraseChinese, 0);
    *ch = save_ch;
    *chd = save_chd;
}

// The main menu's "Language" row (AA_MAIN_LANGUAGE_ROW): the current language's flag on the
// switch's child 1, the one the stock code shows for any code but Chinese.
void hook_main_item(void* self, unsigned char index) {
    g_orig_main_item(self, index);
    if (index != AA_MAIN_LANGUAGE_ROW)
        return;
    const char* cur = aa_lang_current();
    if (strcmp(cur, "zh") == 0 || strcmp(cur, "off") == 0)
        return;
    void* item = *reinterpret_cast<void**>(static_cast<char*>(self) + AA_MAIN_ITEM_BASE + index * 8);
    set_flag(object_node(item), "Switch_Key_ON&OFF/1", aa_lang_index(cur));
}

// Settings > System information: the Android Auto version on the first row the stock code left.
void hook_sysinfo(void* self) {
    g_orig_sysinfo(self);
    char version[64] = "";
    int fd = open(AA_VERSION_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    ssize_t n = read(fd, version, sizeof version - 1);
    close(fd);
    if (n <= 0)
        return;
    version[n] = 0;
    for (char* c = version; *c; ++c)
        if (*c == '\n' || *c == '\r') {
            *c = 0;
            break;
        } else if (!((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || *c == '-' ||
                     *c == '.' || *c == '_' || *c == ' ' || *c == '(' || *c == ')')) {  // "Public Beta 1 (LIVI-...)"
            return;
        }
    int row = *reinterpret_cast<int*>(static_cast<char*>(self) + AA_SYSINFO_BT) ? 5 : 4;
    void* ctrl = *reinterpret_cast<void**>(static_cast<char*>(self) + AA_SYSINFO_ROWS + 8 * row);
    if (!ctrl || !version[0])
        return;
    char text[128];
    snprintf(text, sizeof text, "%s %s", GetVWString(kPhraseAndroidAuto), version);
    // As SysInfoDlg::Init sets up the rows it uses: the light font on the row's node (slot 56, then
    // slot 360 with the font's name), then the text; then shown (Init hid the rows left over).
    void* node = reinterpret_cast<void* (*)(void*)>(vfn(ctrl, 56))(ctrl);
    if (node)
        reinterpret_cast<void (*)(void*, const char*)>(vfn(node, 360))(node, "FZHT_GB18030-Light");
    reinterpret_cast<void (*)(void*, const char*, int)>(vfn(ctrl, 64))(ctrl, text, 0);  // SetText
    reinterpret_cast<void (*)(void*, int)>(vfn(ctrl, 48))(ctrl, 1);                      // SetVisible
    static bool logged;
    if (!logged) {
        logged = true;
        syslog(LOG_INFO, "aa-lang: settings: system information row %d: %s", row, text);
    }
}

void post_event_24(void* self, int code) {
    *reinterpret_cast<int*>(static_cast<char*>(self) + 44) = code;
    void* ws = g_get_ws();
    if (ws)
        reinterpret_cast<void (*)(void*, int, int, int, int, int)>(vfn(ws, 16))(ws, 24, 0, 0, 0, 0);
}

void broadcast() {
    for (size_t a = 0; a < sizeof kApps / sizeof kApps[0]; ++a) {
        g_post(kApps[a], 0x203, 2, 0, 0, -1);
        g_post(kApps[a], 0x203, 0, 0, 0, -1);
    }
}

// The page built again in place (a mode change or a new choice).
void rebuild(void* self) {
    hook_create_list(self);
    hook_update_list(self);
}

// The theme to every app at once (they also take it at their next activation): message 0x206
// with the colour, which each app's view hands to its OnUpdateAmbientColour.
void broadcast_colour(uint32_t rgb) {
    for (size_t a = 0; a < sizeof kApps / sizeof kApps[0]; ++a)
        g_post(kApps[a], 0x206, rgb, 0, 0, -1);
}

void page_click(void* self, Dialog* x, int index) {
    int k = x->shown[index];
    if (g_mode == kAppearance) {
        g_mode = k == 0 ? kTheme : kLogo;
    } else if (g_mode == kTheme) {
        if (k != aa_theme_current() && aa_theme_select(k) == 0)
            broadcast_colour(aa_theme_rgb(k));
    } else if (g_mode == kLogo) {
        if (k != aa_logo_current())
            aa_logo_select(k);
    }
    rebuild(self);
}

void hook_click(void* self, void* list, void* param) {
    int index = *reinterpret_cast<int*>(static_cast<char*>(param) + 8);
    Dialog* x = dialog(self);
    if (!x || index < 0 || index >= x->count)
        return;
    if (g_mode != kLanguage) {
        page_click(self, x, index);
        return;
    }
    const char* code = aa_lang_code(x->shown[index]);
    if (!code || strcmp(code, aa_lang_current()) == 0)
        return;
    unsigned car = 0;
    aa_lang_car(car);
    if (aa_lang_select(code) != 0)
        return;
    bool zh = strcmp(code, "zh") == 0;
    if (zh && car != 1) {
        post_event_24(self, 1);
    } else if (!zh && car == 1) {
        post_event_24(self, 0);
    } else {
        syslog(LOG_INFO, "aa-lang: settings: %s, the car's code stays %u; telling the apps", code, car);
        broadcast();
    }
}

// ---- Settings > Appearance: Back, the title, and the main list's row ----

#ifndef AA_APPEARANCE
const SettingsFunc kAppearanceFuncs[] = {{"none", 0, 0, 0}};
#define AA_MAIN_LAST_KEY 0
#define AA_MAIN_FACTORY_KEY 0
#define AA_MAIN_FIRST_INDEX 0
#endif

typedef void (*Button)(void*, void*, void*);
typedef void (*Click)(void*, void*, void*);
Button g_orig_lang_button;
Method0 g_orig_update_language;
Method0 g_orig_main_create_list;
Click g_orig_main_click;
void* (*g_orig_main_dtor)(void*);
bool (*g_click_before)(void*);                 // SettingMainDlg::ButtonClickBefore
int (*g_key2index)(void*, unsigned char);      // DyListBox::Key2Index
const void* (*g_lang_key)();                   // LanguageSelectDlg::Key
void (*g_work)(const void*, long);            // SVPBaseDlg::_Work (static: the key, a parameter)

const SettingsFunc* appearance_func(const char* name) {
    for (size_t i = 0; i < sizeof kAppearanceFuncs / sizeof kAppearanceFuncs[0]; ++i)
        if (strcmp(kAppearanceFuncs[i].name, name) == 0)
            return &kAppearanceFuncs[i];
    return 0;
}

// Back inside the section: from a choice page to the Appearance page; from there (and from the
// language list) the stock code goes to System settings.
void hook_lang_button(void* self, void* button, void* param) {
    if ((g_mode == kTheme || g_mode == kLogo) && button == member(self, 64)) {
        // The list to its top while it still holds this page's rows: the Appearance page's two
        // would keep the start screen list's scroll and be out of view. On a list this long
        // JumpItemToVisibleCenter(0) stops at the top; on the two-row page it scrolls past it.
        void* list = member(self, 80);
        reinterpret_cast<void (*)(void*, int)>(vfn(list, 232))(list, 0);  // JumpItemToVisibleCenter
        g_mode = kAppearance;
        rebuild(self);
        return;
    }
    g_orig_lang_button(self, button, param);
}

// A language change rebuilds the dialog and sets the language list's title (phrase 1404): a page
// keeps its own.
void hook_update_language(void* self) {
    g_orig_update_language(self);
    if (g_mode != kLanguage)
        set_title(self, page_title(g_mode));
}

// The Appearance row of a SettingMainDlg (one per dialog; the app makes a new one each visit).
struct MainRow {
    void* self;
    ButtonPtr item;
};
MainRow g_rows[2];

void* main_list(void* self) {  // the DyListBox (this+48)'s ISkListBox*
    return *reinterpret_cast<void**>(static_cast<char*>(self) + 56);
}

// Where the row is: before Factory settings, which is always shown, and every row the stock code
// shows, hides or finds at run time comes before it. -1 if it is not in the list.
int row_pos(void* self) {
    return g_key2index(static_cast<char*>(self) + 48, AA_MAIN_FACTORY_KEY);
}

MainRow* main_row(void* self) {
    for (int i = 0; i < 2; ++i)
        if (g_rows[i].self == self)
            return &g_rows[i];
    for (int i = 0; i < 2; ++i)
        if (!g_rows[i].self) {
            MainRow& r = g_rows[i];
            r.self = self;
            r.item.vtbl = *reinterpret_cast<uint32_t*>(static_cast<char*>(self) + AA_MAIN_ITEM_BASE - 4);
            g_create(&r.item, "kzb://VW_MP2017/Prefabs/Btn_Sound_Small_Prefab", "aa-appearance");
            if (!real_item(r.item.item)) {
                syslog(LOG_WARNING, "aa-appearance: settings: no row");
                r.item.item = 0;
                return &r;
            }
            reinterpret_cast<void (*)(void*, int)>(vfn(r.item.item, 180))(r.item.item, 1);
            return &r;
        }
    return 0;
}

// The row's text, set up by the stock code as row 0's (Sound: a plain row) is, phrase 1428 reading
// "Appearance" for the time.
void row_text(void* self, void* item) {
    void** slot = reinterpret_cast<void**>(static_cast<char*>(self) + AA_MAIN_ITEM_BASE);
    void* save = *slot;
    *slot = item;
    aa_lang_phrase_override(kPhraseSound, aa_text("appearance"));
    g_orig_main_item(self, 0);
    aa_lang_phrase_override(kPhraseSound, 0);
    *slot = save;
}

void hook_main_create_list(void* self) {
    g_orig_main_create_list(self);
    MainRow* r = main_row(self);
    if (!r || !r->item.item)
        return;
    void* list = main_list(self);
    row_text(self, r->item.item);
    int pos = row_pos(self);
    if (pos >= 0)
        reinterpret_cast<void (*)(void*, int, void*)>(vfn(list, 160))(list, pos, r->item.item);  // InsertItem
    else
        reinterpret_cast<void (*)(void*, void*)>(vfn(list, 140))(list, r->item.item);  // AddItem
    unsigned char first = *reinterpret_cast<unsigned char*>(AA_MAIN_FIRST_INDEX);
    reinterpret_cast<void (*)(void*, int)>(vfn(list, 348))(list, first);  // the scroll position, as CreateList restores it
}

// A click on the main list: ours opens the Appearance page; one below it is the stock row one
// further up (the stock code counts the rows it made).
void hook_main_click(void* self, void* list, void* param) {
    MainRow* r = main_row(self);
    int* index = reinterpret_cast<int*>(static_cast<char*>(param) + 8);
    int pos = r && r->item.item ? row_pos(self) : -1;
    if (pos >= 0 && *index == pos) {
        unsigned char* last = static_cast<unsigned char*>(self) + AA_MAIN_LAST_KEY;
        if (g_click_before(self)) {  // a pop-up was open: this click closes it, as for the stock rows
            *last = 0xFF;
            return;
        }
        *reinterpret_cast<unsigned char*>(AA_MAIN_FIRST_INDEX) =
            static_cast<unsigned char>(reinterpret_cast<int (*)(void*)>(vfn(list, 320))(list));  // the scroll position
        *last = 0xFE;
        g_mode = kAppearance;
        g_work(g_lang_key(), 0);
        return;
    }
    g_mode = kLanguage;
    if (pos >= 0 && *index > pos) {
        --*index;
        g_orig_main_click(self, list, param);
        ++*index;
        return;
    }
    g_orig_main_click(self, list, param);
}

void* hook_main_dtor(void* self) {
    for (int i = 0; i < 2; ++i)
        if (g_rows[i].self == self) {
            release(g_rows[i].item);
            memset(&g_rows[i], 0, sizeof g_rows[i]);
        }
    return g_orig_main_dtor(self);
}

// The Appearance hooks, a group of their own: a build that differs leaves only the row out.
void appearance_init() {
    for (size_t i = 0; i < sizeof kAppearanceFuncs / sizeof kAppearanceFuncs[0]; ++i) {
        const SettingsFunc& f = kAppearanceFuncs[i];
        if (!f.addr || fnv(reinterpret_cast<const uint8_t*>(f.addr), f.size) != f.fnv) {
            syslog(LOG_WARNING, "aa-appearance: settings: %s is not the known code; no Appearance row", f.name);
            return;
        }
    }
    g_work = reinterpret_cast<void (*)(const void*, long)>(
        dlsym(RTLD_DEFAULT, "_ZN10SVPBaseDlg5_WorkERKN14ClassesFactory3KeyEl"));
    g_click_before = reinterpret_cast<bool (*)(void*)>(appearance_func("click_before")->addr);
    g_key2index = reinterpret_cast<int (*)(void*, unsigned char)>(appearance_func("key2index")->addr);
    g_lang_key = reinterpret_cast<const void* (*)()>(appearance_func("lang_key")->addr);
    uint32_t button = appearance_func("lang_button")->addr, update_language = func("update_language")->addr;
    uint32_t create = appearance_func("main_create_list")->addr, click = appearance_func("main_click")->addr;
    uint32_t dtor = appearance_func("main_dtor")->addr;
    g_orig_lang_button = reinterpret_cast<Button>(trampoline(button));
    g_orig_update_language = trampoline(update_language);
    g_orig_main_create_list = trampoline(create);
    g_orig_main_click = reinterpret_cast<Click>(trampoline(click));
    g_orig_main_dtor = reinterpret_cast<void* (*)(void*)>(trampoline(dtor));
    if (!g_work || !g_orig_lang_button || !g_orig_update_language || !g_orig_main_create_list || !g_orig_main_click ||
        !g_orig_main_dtor || !write_jump(dtor, reinterpret_cast<const void*>(hook_main_dtor)) ||
        !write_jump(button, reinterpret_cast<const void*>(hook_lang_button)) ||
        !write_jump(update_language, reinterpret_cast<const void*>(hook_update_language)) ||
        !write_jump(click, reinterpret_cast<const void*>(hook_main_click)) ||
        !write_jump(create, reinterpret_cast<const void*>(hook_main_create_list))) {
        syslog(LOG_ERR, "aa-appearance: settings: patching failed");
        return;
    }
    syslog(LOG_INFO, "aa-appearance: settings: the Appearance row is in");
}

// A fault in the setting process: one syslog line with where it happened, then the handler that
// was there before (by default the process dies as it would have).
struct sigaction g_old_actions[32];

void on_fault(int sig, siginfo_t* info, void* context) {
    const ucontext_t* uc = static_cast<const ucontext_t*>(context);
    const mcontext_t& m = uc->uc_mcontext;
    syslog(LOG_ERR, "aa-lang: settings: signal %d at %p: pc %08lx lr %08lx sp %08lx r0 %08lx r1 %08lx r2 %08lx r3 %08lx",
           sig, info ? info->si_addr : 0, m.arm_pc, m.arm_lr, m.arm_sp, m.arm_r0, m.arm_r1, m.arm_r2, m.arm_r3);
    sigaction(sig, &g_old_actions[sig], 0);
    raise(sig);
}

void watch_faults() {
    const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; ++i) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
        sigemptyset(&sa.sa_mask);
        sigaction(sigs[i], &sa, &g_old_actions[sigs[i]]);
    }
}

}  // namespace

void* aa_hook_trampoline(uint32_t addr) {
    return reinterpret_cast<void*>(trampoline(addr));
}

bool aa_hook_jump(uint32_t addr, const void* target) {
    return write_jump(addr, target);
}

#ifdef AA_LANG_TEST
// For i18n/test_lib.py: the trampoline and the jump, on a function of the test program.
AA_LANG_API void* aa_test_trampoline(uint32_t addr) {
    return reinterpret_cast<void*>(trampoline(addr));
}
AA_LANG_API int aa_test_write_jump(uint32_t addr, const void* target) {
    return write_jump(addr, target) ? 0 : -1;
}
#endif

void aa_settings_init() {
    char exe[64];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return;
    exe[n] = 0;
    if (!strstr(exe, "/sv.g5.mib2.setting/bin/setting"))
        return;
    watch_faults();
    for (size_t i = 0; i < sizeof kSettingsFuncs / sizeof kSettingsFuncs[0]; ++i) {
        const SettingsFunc& f = kSettingsFuncs[i];
        if (!f.addr || fnv(reinterpret_cast<const uint8_t*>(f.addr), f.size) != f.fnv) {
            syslog(LOG_WARNING, "aa-lang: settings: %s is not the known code; the stock language list stays", f.name);
            return;
        }
    }
    g_create = reinterpret_cast<CreateFromPrefab>(dlsym(RTLD_DEFAULT, "_ZN11SkButtonPtr32CreateInstanceFromPrefabTemplateEPKcS1_"));
    g_ctl = reinterpret_cast<Ctl>(dlsym(RTLD_DEFAULT, "_ZN31SkProperiesUpdateModeControllerC1E17SkPropertySetMode"));
    g_ctl_end = reinterpret_cast<CtlEnd>(dlsym(RTLD_DEFAULT, "_ZN31SkProperiesUpdateModeControllerD1Ev"));
    g_get_ws = reinterpret_cast<GetWS>(dlsym(RTLD_DEFAULT, "_ZN9ISVPAppFw5GetWSEv"));
    g_post = reinterpret_cast<PostMsgEx>(dlsym(RTLD_DEFAULT, "_ZN10SVPMessage9PostMsgExEijjlii"));
    if (!g_create || !g_ctl || !g_ctl_end || !g_get_ws || !g_post) {
        syslog(LOG_WARNING, "aa-lang: settings: a framework function is missing; the stock language list stays");
        return;
    }
    g_kz.from_path = reinterpret_cast<FromPath>(dlsym(RTLD_DEFAULT, "_ZN15SkObjectNodePtr16InstanceFromPathEP13ISkObjectNodePKc"));
    g_kz.memory = reinterpret_cast<void* (*)(const void*)>(dlsym(RTLD_DEFAULT, "kzcMemoryGetManager"));
    g_kz.load = reinterpret_cast<ImageLoad>(dlsym(RTLD_DEFAULT, "kzcImageLoadFile"));
    g_kz.texture = reinterpret_cast<TextureFromImage>(dlsym(RTLD_DEFAULT, "kzuImageTextureCreateFromImage"));
    g_kz.to_resource = reinterpret_cast<ToResource>(dlsym(RTLD_DEFAULT, "kzuImageTextureToResource"));
    g_kz.ui_domain = reinterpret_cast<NodeGet>(dlsym(RTLD_DEFAULT, "kzuObjectNodeGetUIDomain"));
    g_kz.resource_manager = reinterpret_cast<NodeGet>(dlsym(RTLD_DEFAULT, "kzuUIDomainGetResourceManager"));
    g_kz.set = reinterpret_cast<SetResource>(dlsym(RTLD_DEFAULT, "kzuObjectNodeSetResourceIDResourceProperty"));
    g_kz.invalidate = reinterpret_cast<Invalidate>(dlsym(RTLD_DEFAULT, "kzuObjectNodeInvalidateRender"));
    g_kz.texture_type = reinterpret_cast<void**>(dlsym(RTLD_DEFAULT, "KZU_PROPERTY_TYPE_TEXTURE"));
    g_kz.get_float = reinterpret_cast<int (*)(void*, void*, float*)>(dlsym(RTLD_DEFAULT, "kzuObjectNodeGetFloatProperty"));
    g_kz.set_float = reinterpret_cast<int (*)(void*, void*, float)>(dlsym(RTLD_DEFAULT, "kzuObjectNodeSetFloatProperty"));
    g_kz.get_srt = reinterpret_cast<void (*)(void*, void*, float*, float*, float*)>(
        dlsym(RTLD_DEFAULT, "kzuObjectNodeGetMatrix4x4PropertyDefaultSRT"));
    g_kz.set_srt = reinterpret_cast<int (*)(void*, void*, const float*, const float*, const float*)>(
        dlsym(RTLD_DEFAULT, "kzuObjectNodeSetMatrix4x4SRTProperty"));
    g_kz.invalidate_layout = reinterpret_cast<Invalidate>(dlsym(RTLD_DEFAULT, "kzuObjectNodeInvalidateLayout"));
    g_kz.layout_height = reinterpret_cast<void**>(dlsym(RTLD_DEFAULT, "KZU_PROPERTY_TYPE_LAYOUT_HEIGHT"));
    g_kz.render_transform = reinterpret_cast<void**>(dlsym(RTLD_DEFAULT, "KZU_PROPERTY_TYPE_RENDER_TRANSFORMATION"));
    g_kz.add_child_at = reinterpret_cast<int (*)(void*, unsigned, void*)>(dlsym(RTLD_DEFAULT, "kzuObjectNodeAddChildAtIndex"));
    g_kz.remove_child = reinterpret_cast<int (*)(void*, void*)>(dlsym(RTLD_DEFAULT, "kzuObjectNodeRemoveChild"));
    g_kz.get_parent = reinterpret_cast<void* (*)(void*)>(dlsym(RTLD_DEFAULT, "kzuObjectNodeGetParent"));
    g_kz.child_count = reinterpret_cast<unsigned (*)(void*)>(dlsym(RTLD_DEFAULT, "kzuObjectNodeGetChildCount"));
    g_kz.child_at = reinterpret_cast<void* (*)(void*, unsigned)>(dlsym(RTLD_DEFAULT, "kzuObjectNodeGetChildAtIndex"));
    g_kz.get_name = reinterpret_cast<const char* (*)(void*)>(dlsym(RTLD_DEFAULT, "kzuObjectNodeGetName"));
    g_kz.set_name = reinterpret_cast<int (*)(void*, const char*)>(dlsym(RTLD_DEFAULT, "kzuObjectNodeSetName"));
    g_load_rel = reinterpret_cast<LoadRelativeNode>(dlsym(RTLD_DEFAULT, "_ZN10SVPBaseDlg16LoadRelativeNodeEPKc"));
    g_instance = reinterpret_cast<InstanceFromPrefab>(dlsym(RTLD_DEFAULT, "_ZN15SkObjectNodePtr26InstanceFromPrefabTemplateEPKcS1_"));
    g_bar_create = reinterpret_cast<BarFromNode>(
        dlsym(RTLD_DEFAULT, "_ZN20SkScrollBarCtrlExPtr28CreateInstanceFromObjectNodeEP13ISkObjectNode"));
    g_node_assign = reinterpret_cast<NodeAssign>(dlsym(RTLD_DEFAULT, "_ZN14SkInterfaceMgrI13ISkObjectNodeE5EqualEPS0_"));
    g_kz.find_type = reinterpret_cast<void* (*)(const char*)>(dlsym(RTLD_DEFAULT, "kzuPropertyRegistryFindPropertyType"));
    g_kz.float_type = reinterpret_cast<void* (*)(void*)>(dlsym(RTLD_DEFAULT, "kzuFloatPropertyTypeFromPropertyType"));
    void* bar_vtbl = dlsym(RTLD_DEFAULT, "_ZTV20SkScrollBarCtrlExPtr");
    void* node_vtbl = dlsym(RTLD_DEFAULT, "_ZTV15SkObjectNodePtr");
    g_bar_vtbl = bar_vtbl ? reinterpret_cast<uint32_t>(bar_vtbl) + 8 : 0;
    g_node_vtbl = node_vtbl ? reinterpret_cast<uint32_t>(node_vtbl) + 8 : 0;
    g_fit = g_kz.get_float && g_kz.set_float && g_kz.get_srt && g_kz.set_srt && g_kz.invalidate_layout &&
            g_kz.layout_height && g_kz.render_transform;
    g_flags = g_kz.from_path && g_kz.memory && g_kz.load && g_kz.texture && g_kz.to_resource &&
              g_kz.ui_domain && g_kz.resource_manager && g_kz.set && g_kz.invalidate && g_kz.texture_type;
    if (!g_flags)
        syslog(LOG_WARNING, "aa-lang: settings: a Kanzi function is missing; the list keeps the stock flags");
    g_decor = g_fit && g_kz.add_child_at && g_kz.remove_child && g_kz.get_parent && g_kz.child_count &&
              g_kz.child_at && g_kz.get_name && g_kz.set_name && g_load_rel && g_instance && g_bar_create &&
              g_node_assign && g_bar_vtbl && g_node_vtbl;
    if (!g_decor)
        syslog(LOG_WARNING, "aa-lang: settings: a function for the header or the scroll bar is missing; the list goes without");
    g_create_list = func("create_list")->addr;
    g_update_list = func("update_list")->addr;
    g_click = func("click")->addr;
    g_main_item = func("main_item")->addr;
    g_sysinfo = func("sysinfo")->addr;
    g_dtor = func("dtor")->addr;
    g_orig_update_list = trampoline(g_update_list);
    g_orig_main_item = reinterpret_cast<MainItem>(trampoline(g_main_item));
    g_orig_sysinfo = trampoline(g_sysinfo);
    g_orig_dtor = reinterpret_cast<void* (*)(void*)>(trampoline(g_dtor));
    if (!g_orig_update_list || !g_orig_main_item || !g_orig_sysinfo || !g_orig_dtor ||
        !write_jump(g_dtor, reinterpret_cast<const void*>(hook_dtor)) ||
        !write_jump(g_update_list, reinterpret_cast<const void*>(hook_update_list)) ||
        !write_jump(g_create_list, reinterpret_cast<const void*>(hook_create_list)) ||
        !write_jump(g_click, reinterpret_cast<const void*>(hook_click)) ||
        !write_jump(g_main_item, reinterpret_cast<const void*>(hook_main_item)) ||
        !write_jump(g_sysinfo, reinterpret_cast<const void*>(hook_sysinfo))) {
        syslog(LOG_ERR, "aa-lang: settings: patching failed");
        return;
    }
    syslog(LOG_INFO, "aa-lang: settings: the language list offers %d languages%s", aa_lang_count(),
           g_flags ? "" : " (stock flags)");
    if (g_decor)
        appearance_init();
}
