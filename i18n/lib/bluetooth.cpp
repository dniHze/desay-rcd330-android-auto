// SPDX-License-Identifier: GPL-3.0-or-later
//
// No Bluetooth pop-ups over Android Auto. The Bluetooth app (sv.g5.mib2.bluetooth) shows every
// pop-up of its own ("device connected", "contacts available", pairing, connecting, call
// warnings: BTPopupMgr::_Create<SVPTip...Dlg>) through the pop-up's Create(), which first asks
// BTPopupMgr::AddUserID(id) and makes nothing when it answers 0 (it does when a pop-up with that
// id is up already). Stock, the app asks the CarLife service whether to hold back only its
// call pop-ups (NeedBlockBTPopup, in ShowCallPop).
//
// Here, in the Bluetooth app only: AddUserID's entry jumps to hook_add_user_id (as the Settings
// hooks do, settings.cpp), which answers 0, so the pop-up is not made, while Android Auto is on
// screen, and otherwise runs the stock code. "On screen" is what the CarLife shim writes to
// AA_ONTOP_FILE: "<its pid> 1" while its status is 3 (video) and appconnect shows it (Show(1)),
// the same as NeedBlockBTPopup; anything else, no file, or a shim that is gone (its pid not
// running) lets pop-ups through. A file, not a call to the shim: it is read in the app's own
// thread at each pop-up, with nothing to wait for.
//
// AddUserID is found by its symbol (the app exports its functions) and hooked only when its
// first two instructions are the known ones (the same in the 0394 and 0317 apps), which the
// trampoline runs.
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "aa_lang.h"

#ifndef AA_ONTOP_FILE
#define AA_ONTOP_FILE "/tmp/run/aa/ontop"
#endif

namespace {

typedef int (*AddUserID)(void*, int);
AddUserID g_orig;
const char* g_ontop = AA_ONTOP_FILE;
const uint32_t kPrologue[2] = {0xe92d4070u,   // push {r4, r5, r6, lr}
                               0xe1a04000u};  // mov r4, r0

// Whether the shim says Android Auto is on screen, and still runs.
bool on_screen() {
    int fd = open(g_ontop, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    char buf[32];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return false;
    buf[n] = 0;
    int pid = 0, on = 0;
    if (sscanf(buf, "%d %d", &pid, &on) != 2 || on != 1 || pid <= 0)
        return false;
    char proc[24];
    snprintf(proc, sizeof proc, "/proc/%d", pid);
    return access(proc, F_OK) == 0;
}

int hook_add_user_id(void* self, int id) {
    if (on_screen()) {
        syslog(LOG_INFO, "aa-lang: bluetooth: pop-up %d not shown: Android Auto is on screen", id);
        return 0;
    }
    return g_orig(self, id);
}

bool hook(uint32_t addr) {
    const uint32_t* f = reinterpret_cast<const uint32_t*>(addr);
    if (f[0] != kPrologue[0] || f[1] != kPrologue[1])
        return false;
    g_orig = reinterpret_cast<AddUserID>(aa_hook_trampoline(addr));
    return g_orig && aa_hook_jump(addr, reinterpret_cast<const void*>(hook_add_user_id));
}

}  // namespace

void aa_bt_init() {
    char exe[96];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return;
    exe[n] = 0;
    if (!strstr(exe, "/sv.g5.mib2.bluetooth/bin/bluetooth"))
        return;
    void* f = dlsym(RTLD_DEFAULT, "_ZN10BTPopupMgr9AddUserIDEi");
    if (!f || !hook(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(f)))) {
        syslog(LOG_WARNING, "aa-lang: bluetooth: BTPopupMgr::AddUserID %s; its pop-ups show over Android Auto as before",
               f ? "is not the known code" : "not found");
        return;
    }
    syslog(LOG_INFO, "aa-lang: bluetooth: no pop-ups while Android Auto is on screen (%s)", g_ontop);
}

#ifdef AA_LANG_TEST
// For i18n/test_lib.py: the hook on a function of the test program with AddUserID's first two
// instructions, reading `ontop` for "on screen". Returns 0 if hooked.
AA_LANG_API int aa_test_bt(uint32_t fn, const char* ontop) {
    g_ontop = ontop;
    return hook(fn) ? 0 : -1;
}
#endif
