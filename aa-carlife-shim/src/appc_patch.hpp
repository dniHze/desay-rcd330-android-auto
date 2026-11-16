// SPDX-License-Identifier: GPL-2.0-only
//
// Changes to appconnect's code, made in the running process (and baked into the
// rebranded appconnect by kzb-re/patch_appconnect.py, which reads the tables below).
//
// 1. Outgoing calls stay on Android Auto ("callfix"). For an outgoing call (combined call
//    status 1), CAppconnectDataMgr::OnCallStatusChanged (unit appconnect, md5
//    6ccd07279f03657e447aaff81eab5ad0, 0x33590) asks our GetCarLifeConnectStatus(). If the
//    answer is 3 and appconnect is in front, it sleeps 1 s and starts the Bluetooth app in
//    "bt_carlife_temp_call" mode, which covers Android Auto for the whole call. Answering 2 at the
//    right moment raced the event by milliseconds and lost (step 3 run 41). So the branch after
//    that check,
//        336cc: cmp r0, #3          e3500003
//        336d0: bne 33630           1affffd6   -> b 33630 (eaffffd6): always skip the launch
//    0x33630 is the function's normal return, which stock also takes when AA is not connected or
//    appconnect is not in front. The path is CarLife-only (it asks the CarLife service).
//
// 2. An iPhone goes straight to CarPlay ("iphone"). With CarLife coded, connectionservice reports
//    a newly seen Apple device as CONNECTION_TYPE 1 or 4 with ACTIVATION_TYPE -1 (no choice stored
//    for it), and CAppconnectDataMgr::On_Connection_Changed (0x35de0) then sleeps 1 s and posts
//    event 6, which AppconncetPopUpMgr::OnEvent turns into the "connect to Apple CarPlay or Baidu
//    CarLife?" pop-up (OnPOAPPCONNECTNewAppleDeviceTipShow). CarLife here is Android Auto, which an
//    iPhone cannot run. So after the same 1 s, the post becomes what the pop-up's CarPlay button
//    does: AppconncetPopUpMgr::UserSelectEstablishSession(1). Its AppConnectType follows
//    ACTIVATION_TYPE: 1 CarPlay, 2 CarLife, 3 MirrorLink (step 3 run 46: tapping the Android
//    Auto tile logged "UserSelectEstablishSession ===[AppConnectType =2]"; run 10: the user's
//    iPhone went through Choose_Technology(1) to CarPlay). The first build passed 2 (CarLife) by
//    mistake; it was never reached (a known iPhone has a stored choice and skips the path).
//        36068: bl GetWS        ebffc590 -> bl SVPSingleton<AppconncetPopUpMgr>::GetInstance  ebffedfd
//        3606c: ldr lr, [r0]    e590e000 -> mov r1, #1                                        e3a01001
//        36070: mov ip, #0      e3a0c000 -> bl AppconncetPopUpMgr::UserSelectEstablishSession  eb000736
//        36074: mov r1, #6      e3a01006 -> b 35f18 (the function's normal return)             eaffffa7
//    Apple only: the path needs type 1 or 4 (CAppconnectDataMgr::IsAppleDevice's types; Android
//    is 5), and a phone with a stored choice never reaches it. Android phones keep their choice
//    screen (Android Auto or MirrorLink).
//
// 3. An iPhone is not offered Android Auto on the connect screen ("iphonetile", "iphonetile2").
//    With CarPlay, MirrorLink and CarLife all coded (this unit), CAppconnectMainDlg::ShowConnectUI
//    (0x2ea0c) and its twin UpdateShowConnectUI (0x2e028) show an Apple device two small tiles,
//    Btn_CarApple_Change and Btn_Carlife_Change: "Apple CarPlay | Android Auto" (step 3 run 45;
//    stock Baidu CarLife ran on iPhones too). With MirrorLink not coded, the same functions take
//    another branch, which hides every small tile (its "visible" register r6 is 0 there) and,
//    with CarPlay and CarLife coded, shows an Apple device the large CarPlay tile alone. So the
//    Apple block of the first branch now enters that branch with r6 = 0:
//        2ed44: ldr r0, [r4, #88]  e5940058 -> mov r6, #0  e3a06000
//        2ed48: mov r1, #0         e3a01000 -> b 2ea5c     eaffff43   (ShowConnectUI)
//        2e52c: ldr r0, [r4, #88]  e5940058 -> mov r6, #0  e3a06000
//        2e530: mov r1, #0         e3a01000 -> b 2e0b8     eafffee0   (UpdateShowConnectUI)
//    Both blocks are reached only after SVPIsAppleDevice said yes (and with CarPlay and CarLife
//    coded, which the branch tests again); Android phones keep Android Auto and MirrorLink.
//
// 4. Holding a seek key seeks ("seekhold"). CCarlifeDataMgr::OnCarLifeVKeyEvent (0x48b3c) gets
//    every key action from the platform (3 press, 1 long press, 2 click, 4 release; libappfw's
//    SVPKeyEventProcessor), but passes the seek keys (vkeys 780 / 781) on only for the press,
//    as RotateKeyEvent(15 / 16, 1): Android Auto never learns that the key is held. (CarPlay's
//    handler, CCarplayDataMgr::CarplayVKeyRegister, sends the iPhone the press and the release,
//    and the iPhone seeks by itself while the key is held.) So every action now goes through,
//    as the value 16 + action (19 press, 17 long press, 18 click, 20 release); the receiver
//    (ctl.rs) skips a track on the click and fast-forwards or rewinds while the key is held.
//    A stock appconnect still sends 1, which the receiver takes as a skip, as before.
//        48bfc: bne 48bd0     1afffff3 -> nop                  e1a00000   (seek -)
//        48c18: mov r2, #1    e3a02001 -> add r2, r6, #16      e2862010
//        48d5c: bne 48bd0     1affff9b -> nop                  e1a00000   (seek +)
//        48d78: mov r2, #1    e3a02001 -> add r2, r6, #16      e2862010
//    The voice-session check in front of the call (GetModuleStatus(4) while appconnect is in
//    front) is unchanged and runs for every action.
//
// 5. The phone key reaches Android Auto ("callkey"). For vkey 805 the same function asks
//    NeedBlockBTPopup() (1 while Android Auto is on screen, see the shim) and then hands the key
//    to MFLPhoneFunction, which answers or ends a call through the car's Bluetooth; it never
//    tells CarLife. Now, after MFLPhoneFunction, the key also goes to RotateKeyEvent(2, 16 +
//    action) (2 is CarLife's phone key): with no call, the receiver opens Android Auto's phone
//    screen on a click. The call is made in the function's log block at 0x48e08, which only the
//    voice key's release reached (to log, then send VR stop at 0x48e2c); that branch now goes
//    straight to 0x48e2c, so only the log line is lost. Written in this order:
//        48e08..48e20: bl SVP_GetCarLifeService; ldr r3, [r0]; mov r1, #2; add r2, r6, #16;
//                      ldr r3, [r3, #100] (RotateKeyEvent); blx r3; b 48bd0
//        48cb8: bne 48e08     1a000052 -> bne 48e2c            1a00005b
//        48c94: b 48bd0       eaffffcd -> b 48e08              ea00005b
//
// Three builds of appconnect are known: the PQ unit's 0358 (md5 6ccd0727…, the addresses above),
// the MQB 0394 (5GG 035 280 D, md5 51441f56…) and the later PQ 0317 (6RD 035 187 B, 2020, md5
// 6e2d0e16…). The five functions are the same code there, moved; kzb-re/port_appc_patch.py moved
// each table by its offset inside the same named function and checked every instruction (the
// 0394 and 0317 tables below keep the 0358 comments, with 0358 addresses).
// For each change the variant whose guard words all match is used; none matching leaves
// appconnect alone.
//
// Each change is made only when every guard word around it (the code it relies on, the
// functions it calls) is exactly the unit's, and its edit words are all stock or all patched.
// The edits are written run by run (a run: consecutive words, one write), in table order.
// The text is a private mapping, so writing it through /proc/<pid>/mem copies the page for that
// one process: the file on the unit is never written; a new appconnect process or a reboot runs
// the file's code again until the shim patches it again. The rebranded appconnect
// (kzb-re/rebrand_strings.py) changes only texts, so its code words are the same.
#ifndef AA_CARLIFE_SHIM_APPC_PATCH_HPP
#define AA_CARLIFE_SHIM_APPC_PATCH_HPP

#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

namespace appc_patch {

struct Word {
    unsigned long addr;
    uint32_t word;
};
struct Edit {
    unsigned long addr;
    uint32_t stock, patched;
};

// --- patch callfix -------------------------------------------------------------------------
const Word kCallfixGuards[] = {
    {0x336bc, 0xebffce71}, // bl   SVP_GetCarLifeService
    {0x336c0, 0xe5903000}, // ldr  r3, [r0]
    {0x336c4, 0xe5933068}, // ldr  r3, [r3, #104]      GetCarLifeConnectStatus
    {0x336c8, 0xe12fff33}, // blx  r3
    {0x336cc, 0xe3500003}, // cmp  r0, #3
    {0x336d4, 0xe1a00009}, // mov  r0, r9
    {0x336d8, 0xebffff8f}, // bl   CAppconnectDataMgr::APPConnectIsFG
    {0x336dc, 0xe3500000}, // cmp  r0, #0
    {0x336e0, 0x0affffd2}, // beq  33630
    {0x336ec, 0xebffce2c}, // bl   sleep
    {0x3371c, 0xe3a00042}, // mov  r0, #66             the Bluetooth app
    {0x33720, 0xe28d1010}, // add  r1, sp, #16         "bt_carlife_temp_call"
    {0x33724, 0xe3a0200c}, // mov  r2, #12
    {0x33728, 0xe3a03001}, // mov  r3, #1
    {0x3372c, 0xebffce58}, // bl   SVP_StartApp
};
const Edit kCallfixEdits[] = {
    {0x336d0, 0x1affffd6, 0xeaffffd6}, // bne 33630 -> b 33630
};

// --- patch iphone --------------------------------------------------------------------------
const Word kIphoneGuards[] = {
    {0x35f04, 0xe3550001}, // cmp   r5, #1               type 1 ...
    {0x35f08, 0x13550004}, // cmpne r5, #4               ... or 4 (Apple)
    {0x35f10, 0xe3760001}, // cmn   r6, #1               no stored choice
    {0x35f14, 0x0a000041}, // beq   36020                -> the pop-up path
    {0x35f18, 0xe28dd014}, // add   sp, sp, #20          the normal return
    {0x3605c, 0xebffc3df}, // bl    printf               its log line
    {0x36060, 0xe3a00001}, // mov   r0, #1
    {0x36064, 0xebffc3ce}, // bl    sleep
    {0x36078, 0xe1a0200c}, // mov   r2, ip               (rest of the post, now unreached)
    {0x3608c, 0xe12fff3c}, // blx   ip
    {0x31864, 0xe92d4070}, // SVPSingleton<AppconncetPopUpMgr>::GetInstance: push {r4-r6, lr}
    {0x37d50, 0xe92d4ff0}, // AppconncetPopUpMgr::UserSelectEstablishSession: push {r4-fp, lr}
    {0x37e08, 0xe3540002}, // ... cmp r4, #2             2 = CarLife (its own branch)
    {0x37dc4, 0xe3a07001}, // ... mov r7, #1
    {0x37e1c, 0xe1540007}, // ... cmp r4, r7             r7 = 1: CarPlay
    {0x37e20, 0x0a000018}, // ... beq 37e88              the CarPlay branch
};
const Edit kIphoneEdits[] = {
    {0x36068, 0xebffc590, 0xebffedfd}, // bl GetWS     -> bl GetInstance (0x31864)
    {0x3606c, 0xe590e000, 0xe3a01001}, // ldr lr, [r0] -> mov r1, #1 (CarPlay)
    {0x36070, 0xe3a0c000, 0xeb000736}, // mov ip, #0   -> bl UserSelectEstablishSession (0x37d50)
    {0x36074, 0xe3a01006, 0xeaffffa7}, // mov r1, #6   -> b 35f18
};

// --- patch iphonetile (ShowConnectUI) -----------------------------------------------------
const Word kIphonetileGuards[] = {
    {0x2ea50, 0xe5d3604d}, // ldrb r6, [r3, #77]         MirrorLink coded? (the other branch
    {0x2ea54, 0xe3560000}, // cmp r6, #0                 when not)
    {0x2ea58, 0x1a000048}, // bne 2eb80
    {0x2ea5c, 0xe5940058}, // ldr r0, [r4, #88]          the other branch: hide the small tiles
    {0x2ea60, 0xe1a01006}, // mov r1, r6                 ... with r6
    {0x2eac0, 0xebffef29}, // bl DataMgr::GetInstance
    {0x2eac4, 0xe5d0304c}, // ldrb r3, [r0, #76]
    {0x2eac8, 0xe3530000}, // cmp r3, #0
    {0x2eacc, 0x1a0000ae}, // bne 2ed8c
    {0x2ed38, 0xebfff675}, // bl SVPIsAppleDevice        the Apple block's entry
    {0x2ed3c, 0xe2506000}, // subs r6, r0, #0
    {0x2ed40, 0x0a000035}, // beq 2ee1c                  (not Apple)
    {0x2ed8c, 0xebffee76}, // bl DataMgr::GetInstance
    {0x2ed90, 0xe5d0304e}, // ldrb r3, [r0, #78]
    {0x2ed94, 0xe3530000}, // cmp r3, #0
    {0x2ed98, 0x0affff4c}, // beq 2ead0
    {0x2eda0, 0xebfff65b}, // bl SVPIsAppleDevice
    {0x2eda4, 0xe2507000}, // subs r7, r0, #0
    {0x2eda8, 0x0a00000f}, // beq 2edec
    {0x2edc0, 0xe5940068}, // ldr r0, [r4, #104]         the large CarPlay tile ...
    {0x2edc4, 0xe3a01001}, // mov r1, #1                 ... visible
    {0x2ede8, 0xeaffff4f}, // b 2eb2c                    the function's tail
};
const Edit kIphonetileEdits[] = {
    {0x2ed44, 0xe5940058, 0xe3a06000}, // ldr r0, [r4, #88] -> mov r6, #0
    {0x2ed48, 0xe3a01000, 0xeaffff43}, // mov r1, #0        -> b 2ea5c
};

// --- patch iphonetile2 (UpdateShowConnectUI) ---------------------------------------------
const Word kIphonetile2Guards[] = {
    {0x2e0ac, 0xe5d3604d}, // ldrb r6, [r3, #77]
    {0x2e0b0, 0xe3560000}, // cmp r6, #0
    {0x2e0b4, 0x1a000057}, // bne 2e218
    {0x2e0b8, 0xe5940058}, // ldr r0, [r4, #88]
    {0x2e0bc, 0xe1a01006}, // mov r1, r6
    {0x2e130, 0xebfff18d}, // bl DataMgr::GetInstance
    {0x2e134, 0xe5d0304c}, // ldrb r3, [r0, #76]
    {0x2e138, 0xe3530000}, // cmp r3, #0
    {0x2e13c, 0x1a0000ce}, // bne 2e47c
    {0x2e3e4, 0xebfff8ca}, // bl SVPIsAppleDevice
    {0x2e3e8, 0xe2506000}, // subs r6, r0, #0
    {0x2e3ec, 0x1a00004e}, // bne 2e52c                  the Apple block's entry
    {0x2e47c, 0xebfff0ba}, // bl DataMgr::GetInstance
    {0x2e480, 0xe5d0304e}, // ldrb r3, [r0, #78]
    {0x2e484, 0xe3530000}, // cmp r3, #0
    {0x2e488, 0x0affff2c}, // beq 2e140
    {0x2e490, 0xebfff89f}, // bl SVPIsAppleDevice
    {0x2e494, 0xe2507000}, // subs r7, r0, #0
    {0x2e498, 0x1a000013}, // bne 2e4ec
    {0x2e500, 0xe5940068}, // ldr r0, [r4, #104]
    {0x2e504, 0xe3a01001}, // mov r1, #1
    {0x2e528, 0xeaffff25}, // b 2e1c4
};
const Edit kIphonetile2Edits[] = {
    {0x2e52c, 0xe5940058, 0xe3a06000}, // ldr r0, [r4, #88] -> mov r6, #0
    {0x2e530, 0xe3a01000, 0xeafffee0}, // mov r1, #0        -> b 2e0b8
};

// --- patch seekhold (OnCarLifeVKeyEvent) --------------------------------------------------
const Word kSeekholdGuards[] = {
    {0x48bd0, 0xe28dd020}, // add  sp, sp, #32          the function's return
    {0x48bd4, 0xe8bd87f0}, // pop  {r4-sl, pc}
    {0x48bd8, 0xe300330d}, // movw r3, #781
    {0x48be0, 0x0a00005c}, // beq  48d58                vkey 781: seek +
    {0x48bf0, 0xe3540fc3}, // cmp  r4, #780             vkey 780: seek -
    {0x48bf4, 0x1afffff5}, // bne  48bd0
    {0x48bf8, 0xe3560003}, // cmp  r6, #3               the press only (r6: the action)
    {0x48c00, 0xe59d3010}, // ldr  r3, [sp, #16]        the app in front
    {0x48c0c, 0xebff791d}, // bl   SVP_GetCarLifeService
    {0x48c10, 0xe5903000}, // ldr  r3, [r0]
    {0x48c14, 0xe3a0100f}, // mov  r1, #15              CarLife's seek -
    {0x48c1c, 0xe5933064}, // ldr  r3, [r3, #100]       RotateKeyEvent
    {0x48c20, 0xe12fff33}, // blx  r3
    {0x48c24, 0xeaffffe9}, // b    48bd0
    {0x48d58, 0xe3560003}, // cmp  r6, #3
    {0x48d6c, 0xebff78c5}, // bl   SVP_GetCarLifeService
    {0x48d70, 0xe5903000}, // ldr  r3, [r0]
    {0x48d74, 0xe3a01010}, // mov  r1, #16              CarLife's seek +
    {0x48d7c, 0xe5933064}, // ldr  r3, [r3, #100]       RotateKeyEvent
    {0x48d80, 0xe12fff33}, // blx  r3
    {0x48d84, 0xeaffff91}, // b    48bd0
    {0x48dd4, 0xe5933078}, // ldr  r3, [r3, #120]       GetModuleStatus(4) with appconnect in
    {0x48ddc, 0xe3500001}, // cmp  r0, #1               front: no seek during a voice session
    {0x48de0, 0x1affff89}, // bne  48c0c
    {0x48df4, 0xe5933078}, // ldr  r3, [r3, #120]
    {0x48dfc, 0xe3500001}, // cmp  r0, #1
    {0x48e00, 0x1affffd9}, // bne  48d6c
};
const Edit kSeekholdEdits[] = {
    {0x48bfc, 0x1afffff3, 0xe1a00000}, // bne 48bd0  -> nop: every action, not only the press
    {0x48c18, 0xe3a02001, 0xe2862010}, // mov r2, #1 -> add r2, r6, #16: 16 + the action
    {0x48d5c, 0x1affff9b, 0xe1a00000}, // bne 48bd0  -> nop
    {0x48d78, 0xe3a02001, 0xe2862010}, // mov r2, #1 -> add r2, r6, #16
};

// --- patch callkey (OnCarLifeVKeyEvent) ---------------------------------------------------
const Word kCallkeyGuards[] = {
    {0x48bd0, 0xe28dd020}, // add  sp, sp, #32          the function's return
    {0x48bd4, 0xe8bd87f0}, // pop  {r4-sl, pc}
    {0x48bb0, 0xe3003325}, // movw r3, #805             vkey 805: the phone key
    {0x48bb8, 0x0a00002c}, // beq  48c70
    {0x48c70, 0xebff7904}, // bl   SVP_GetCarLifeService
    {0x48c74, 0xe5903000}, // ldr  r3, [r0]
    {0x48c78, 0xe5933070}, // ldr  r3, [r3, #112]       NeedBlockBTPopup: 1 while AA is on screen
    {0x48c7c, 0xe12fff33}, // blx  r3
    {0x48c80, 0xe3500000}, // cmp  r0, #0
    {0x48c84, 0x0affffd1}, // beq  48bd0
    {0x48c88, 0xe1a0000a}, // mov  r0, sl
    {0x48c8c, 0xe1a01006}, // mov  r1, r6               the action (r6 is kept across calls)
    {0x48c90, 0xebfffdca}, // bl   MFLPhoneFunction     answers / ends a call: unchanged
    {0x48c98, 0xe3560004}, // cmp  r6, #4               vkey 804 (voice), its release ...
    {0x48cb4, 0xe3500000}, // cmp  r0, #0               ... during a voice session:
    {0x48e24, 0xe08f0000}, // add  r0, pc, r0           (the rest of the log line, now unreached)
    {0x48e28, 0xebff786c}, // bl   printf
    {0x48e2c, 0xebff7895}, // bl   SVP_GetCarLifeService   ... VR stop
    {0x48e30, 0xe5903000}, // ldr  r3, [r0]
    {0x48e34, 0xe3a01022}, // mov  r1, #34
    {0x48e38, 0xe3a02001}, // mov  r2, #1
    {0x48e3c, 0xe5933064}, // ldr  r3, [r3, #100]       RotateKeyEvent
    {0x48e40, 0xe12fff33}, // blx  r3
    {0x48e44, 0xeaffff61}, // b    48bd0
};
const Edit kCallkeyEdits[] = {
    {0x48e08, 0xebff7980, 0xebff789e}, // bl getpid        -> bl SVP_GetCarLifeService
    {0x48e0c, 0xe1a04000, 0xe5903000}, // mov r4, r0       -> ldr r3, [r0]
    {0x48e10, 0xebff78ee, 0xe3a01002}, // bl GetCurrentThreadID -> mov r1, #2 (CarLife's phone key)
    {0x48e14, 0xe1a02000, 0xe2862010}, // mov r2, r0       -> add r2, r6, #16: 16 + the action
    {0x48e18, 0xe59f00c4, 0xe5933064}, // ldr r0, [pc, #196] -> ldr r3, [r3, #100] (RotateKeyEvent)
    {0x48e1c, 0xe1a01004, 0xe12fff33}, // mov r1, r4       -> blx r3
    {0x48e20, 0xe1a03007, 0xeaffff6a}, // mov r3, r7       -> b 48bd0
    {0x48cb8, 0x1a000052, 0x1a00005b}, // bne 48e08        -> bne 48e2c: VR stop without its log line
    {0x48c94, 0xeaffffcd, 0xea00005b}, // b 48bd0          -> b 48e08: after MFLPhoneFunction
};

// --- callfix (MQB 0394; ported from the 0358 tables by kzb-re/port_appc_patch.py) ---
const Word kCallfix0394Guards[] = {
    {0x33bc4, 0xebffcda3}, // bl   SVP_GetCarLifeService
    {0x33bc8, 0xe5903000}, // ldr  r3, [r0]
    {0x33bcc, 0xe5933068}, // ldr  r3, [r3, #104]      GetCarLifeConnectStatus
    {0x33bd0, 0xe12fff33}, // blx  r3
    {0x33bd4, 0xe3500003}, // cmp  r0, #3
    {0x33bdc, 0xe1a00009}, // mov  r0, r9
    {0x33be0, 0xebffff8f}, // bl   CAppconnectDataMgr::APPConnectIsFG
    {0x33be4, 0xe3500000}, // cmp  r0, #0
    {0x33be8, 0x0affffd2}, // beq  33630
    {0x33bf4, 0xebffcd5b}, // bl   sleep
    {0x33c24, 0xe3a00042}, // mov  r0, #66             the Bluetooth app
    {0x33c28, 0xe28d1010}, // add  r1, sp, #16         "bt_carlife_temp_call"
    {0x33c2c, 0xe3a0200c}, // mov  r2, #12
    {0x33c30, 0xe3a03001}, // mov  r3, #1
    {0x33c34, 0xebffcd8a}, // bl   SVP_StartApp
};
const Edit kCallfix0394Edits[] = {
    {0x33bd8, 0x1affffd6, 0xeaffffd6}, // bne 33630 -> b 33630
};
// --- iphone (MQB 0394; ported from the 0358 tables by kzb-re/port_appc_patch.py) ---
const Word kIphone0394Guards[] = {
    {0x3640c, 0xe3550001}, // cmp   r5, #1               type 1 ...
    {0x36410, 0x13550004}, // cmpne r5, #4               ... or 4 (Apple)
    {0x36418, 0xe3760001}, // cmn   r6, #1               no stored choice
    {0x3641c, 0x0a000041}, // beq   36020                -> the pop-up path
    {0x36420, 0xe28dd014}, // add   sp, sp, #20          the normal return
    {0x36564, 0xebffc30e}, // bl    printf               its log line
    {0x36568, 0xe3a00001}, // mov   r0, #1
    {0x3656c, 0xebffc2fd}, // bl    sleep
    {0x36580, 0xe1a0200c}, // mov   r2, ip               (rest of the post, now unreached)
    {0x36594, 0xe12fff3c}, // blx   ip
    {0x31d5c, 0xe92d4070}, // SVPSingleton<AppconncetPopUpMgr>::GetInstance: push {r4-r6, lr}
    {0x38258, 0xe92d4ff0}, // AppconncetPopUpMgr::UserSelectEstablishSession: push {r4-fp, lr}
    {0x38310, 0xe3540002}, // ... cmp r4, #2             2 = CarLife (its own branch)
    {0x382cc, 0xe3a07001}, // ... mov r7, #1
    {0x38324, 0xe1540007}, // ... cmp r4, r7             r7 = 1: CarPlay
    {0x38328, 0x0a000018}, // ... beq 37e88              the CarPlay branch
};
const Edit kIphone0394Edits[] = {
    {0x36570, 0xebffc4c2, 0xebffedf9}, // bl GetWS     -> bl GetInstance (0x31864)
    {0x36574, 0xe590e000, 0xe3a01001}, // ldr lr, [r0] -> mov r1, #1 (CarPlay)
    {0x36578, 0xe3a0c000, 0xeb000736}, // mov ip, #0   -> bl UserSelectEstablishSession (0x37d50)
    {0x3657c, 0xe3a01006, 0xeaffffa7}, // mov r1, #6   -> b 35f18
};
// --- iphonetile (MQB 0394; ported from the 0358 tables by kzb-re/port_appc_patch.py) ---
const Word kIphonetile0394Guards[] = {
    {0x2f1ec, 0xe5d3604d}, // ldrb r6, [r3, #77]         MirrorLink coded? (the other branch
    {0x2f1f0, 0xe3560000}, // cmp r6, #0                 when not)
    {0x2f1f4, 0x1a000048}, // bne 2eb80
    {0x2f1f8, 0xe5940058}, // ldr r0, [r4, #88]          the other branch: hide the small tiles
    {0x2f1fc, 0xe1a01006}, // mov r1, r6                 ... with r6
    {0x2f25c, 0xebffedcb}, // bl DataMgr::GetInstance
    {0x2f260, 0xe5d0304c}, // ldrb r3, [r0, #76]
    {0x2f264, 0xe3530000}, // cmp r3, #0
    {0x2f268, 0x1a0000ae}, // bne 2ed8c
    {0x2f4d4, 0xebfff675}, // bl SVPIsAppleDevice        the Apple block's entry
    {0x2f4d8, 0xe2506000}, // subs r6, r0, #0
    {0x2f4dc, 0x0a000035}, // beq 2ee1c                  (not Apple)
    {0x2f528, 0xebffed18}, // bl DataMgr::GetInstance
    {0x2f52c, 0xe5d0304e}, // ldrb r3, [r0, #78]
    {0x2f530, 0xe3530000}, // cmp r3, #0
    {0x2f534, 0x0affff4c}, // beq 2ead0
    {0x2f53c, 0xebfff65b}, // bl SVPIsAppleDevice
    {0x2f540, 0xe2507000}, // subs r7, r0, #0
    {0x2f544, 0x0a00000f}, // beq 2edec
    {0x2f55c, 0xe5940068}, // ldr r0, [r4, #104]         the large CarPlay tile ...
    {0x2f560, 0xe3a01001}, // mov r1, #1                 ... visible
    {0x2f584, 0xeaffff4f}, // b 2eb2c                    the function's tail
};
const Edit kIphonetile0394Edits[] = {
    {0x2f4e0, 0xe5940058, 0xe3a06000}, // ldr r0, [r4, #88] -> mov r6, #0
    {0x2f4e4, 0xe3a01000, 0xeaffff43}, // mov r1, #0        -> b 2ea5c
};
// --- iphonetile2 (MQB 0394; ported from the 0358 tables by kzb-re/port_appc_patch.py) ---
const Word kIphonetile20394Guards[] = {
    {0x2e848, 0xe5d3604d}, // ldrb r6, [r3, #77]
    {0x2e84c, 0xe3560000}, // cmp r6, #0
    {0x2e850, 0x1a000057}, // bne 2e218
    {0x2e854, 0xe5940058}, // ldr r0, [r4, #88]
    {0x2e858, 0xe1a01006}, // mov r1, r6
    {0x2e8cc, 0xebfff02f}, // bl DataMgr::GetInstance
    {0x2e8d0, 0xe5d0304c}, // ldrb r3, [r0, #76]
    {0x2e8d4, 0xe3530000}, // cmp r3, #0
    {0x2e8d8, 0x1a0000ce}, // bne 2e47c
    {0x2eb80, 0xebfff8ca}, // bl SVPIsAppleDevice
    {0x2eb84, 0xe2506000}, // subs r6, r0, #0
    {0x2eb88, 0x1a00004e}, // bne 2e52c                  the Apple block's entry
    {0x2ec18, 0xebffef5c}, // bl DataMgr::GetInstance
    {0x2ec1c, 0xe5d0304e}, // ldrb r3, [r0, #78]
    {0x2ec20, 0xe3530000}, // cmp r3, #0
    {0x2ec24, 0x0affff2c}, // beq 2e140
    {0x2ec2c, 0xebfff89f}, // bl SVPIsAppleDevice
    {0x2ec30, 0xe2507000}, // subs r7, r0, #0
    {0x2ec34, 0x1a000013}, // bne 2e4ec
    {0x2ec9c, 0xe5940068}, // ldr r0, [r4, #104]
    {0x2eca0, 0xe3a01001}, // mov r1, #1
    {0x2ecc4, 0xeaffff25}, // b 2e1c4
};
const Edit kIphonetile20394Edits[] = {
    {0x2ecc8, 0xe5940058, 0xe3a06000}, // ldr r0, [r4, #88] -> mov r6, #0
    {0x2eccc, 0xe3a01000, 0xeafffee0}, // mov r1, #0        -> b 2e0b8
};
// --- seekhold (MQB 0394; ported from the 0358 tables by kzb-re/port_appc_patch.py) ---
const Word kSeekhold0394Guards[] = {
    {0x48f78, 0xe28dd020}, // add  sp, sp, #32          the function's return
    {0x48f7c, 0xe8bd87f0}, // pop  {r4-sl, pc}
    {0x48f80, 0xe300330d}, // movw r3, #781
    {0x48f88, 0x0a00005c}, // beq  48d58                vkey 781: seek +
    {0x48f98, 0xe3540fc3}, // cmp  r4, #780             vkey 780: seek -
    {0x48f9c, 0x1afffff5}, // bne  48bd0
    {0x48fa0, 0xe3560003}, // cmp  r6, #3               the press only (r6: the action)
    {0x48fa8, 0xe59d3010}, // ldr  r3, [sp, #16]        the app in front
    {0x48fb4, 0xebff78a7}, // bl   SVP_GetCarLifeService
    {0x48fb8, 0xe5903000}, // ldr  r3, [r0]
    {0x48fbc, 0xe3a0100f}, // mov  r1, #15              CarLife's seek -
    {0x48fc4, 0xe5933064}, // ldr  r3, [r3, #100]       RotateKeyEvent
    {0x48fc8, 0xe12fff33}, // blx  r3
    {0x48fcc, 0xeaffffe9}, // b    48bd0
    {0x49100, 0xe3560003}, // cmp  r6, #3
    {0x49114, 0xebff784f}, // bl   SVP_GetCarLifeService
    {0x49118, 0xe5903000}, // ldr  r3, [r0]
    {0x4911c, 0xe3a01010}, // mov  r1, #16              CarLife's seek +
    {0x49124, 0xe5933064}, // ldr  r3, [r3, #100]       RotateKeyEvent
    {0x49128, 0xe12fff33}, // blx  r3
    {0x4912c, 0xeaffff91}, // b    48bd0
    {0x4917c, 0xe5933078}, // ldr  r3, [r3, #120]       GetModuleStatus(4) with appconnect in
    {0x49184, 0xe3500001}, // cmp  r0, #1               front: no seek during a voice session
    {0x49188, 0x1affff89}, // bne  48c0c
    {0x4919c, 0xe5933078}, // ldr  r3, [r3, #120]
    {0x491a4, 0xe3500001}, // cmp  r0, #1
    {0x491a8, 0x1affffd9}, // bne  48d6c
};
const Edit kSeekhold0394Edits[] = {
    {0x48fa4, 0x1afffff3, 0xe1a00000}, // bne 48bd0  -> nop: every action, not only the press
    {0x48fc0, 0xe3a02001, 0xe2862010}, // mov r2, #1 -> add r2, r6, #16: 16 + the action
    {0x49104, 0x1affff9b, 0xe1a00000}, // bne 48bd0  -> nop
    {0x49120, 0xe3a02001, 0xe2862010}, // mov r2, #1 -> add r2, r6, #16
};
// --- callkey (MQB 0394; ported from the 0358 tables by kzb-re/port_appc_patch.py) ---
const Word kCallkey0394Guards[] = {
    {0x48f78, 0xe28dd020}, // add  sp, sp, #32          the function's return
    {0x48f7c, 0xe8bd87f0}, // pop  {r4-sl, pc}
    {0x48f58, 0xe3003325}, // movw r3, #805             vkey 805: the phone key
    {0x48f60, 0x0a00002c}, // beq  48c70
    {0x49018, 0xebff788e}, // bl   SVP_GetCarLifeService
    {0x4901c, 0xe5903000}, // ldr  r3, [r0]
    {0x49020, 0xe5933070}, // ldr  r3, [r3, #112]       NeedBlockBTPopup: 1 while AA is on screen
    {0x49024, 0xe12fff33}, // blx  r3
    {0x49028, 0xe3500000}, // cmp  r0, #0
    {0x4902c, 0x0affffd1}, // beq  48bd0
    {0x49030, 0xe1a0000a}, // mov  r0, sl
    {0x49034, 0xe1a01006}, // mov  r1, r6               the action (r6 is kept across calls)
    {0x49038, 0xebfffe22}, // bl   MFLPhoneFunction     answers / ends a call: unchanged
    {0x49040, 0xe3560004}, // cmp  r6, #4               vkey 804 (voice), its release ...
    {0x4905c, 0xe3500000}, // cmp  r0, #0               ... during a voice session:
    {0x491cc, 0xe08f0000}, // add  r0, pc, r0           (the rest of the log line, now unreached)
    {0x491d0, 0xebff77f3}, // bl   printf
    {0x491d4, 0xebff781f}, // bl   SVP_GetCarLifeService   ... VR stop
    {0x491d8, 0xe5903000}, // ldr  r3, [r0]
    {0x491dc, 0xe3a01022}, // mov  r1, #34
    {0x491e0, 0xe3a02001}, // mov  r2, #1
    {0x491e4, 0xe5933064}, // ldr  r3, [r3, #100]       RotateKeyEvent
    {0x491e8, 0xe12fff33}, // blx  r3
    {0x491ec, 0xeaffff61}, // b    48bd0
};
const Edit kCallkey0394Edits[] = {
    {0x491b0, 0xebff790a, 0xebff7828}, // bl getpid        -> bl SVP_GetCarLifeService
    {0x491b4, 0xe1a04000, 0xe5903000}, // mov r4, r0       -> ldr r3, [r0]
    {0x491b8, 0xebff7878, 0xe3a01002}, // bl GetCurrentThreadID -> mov r1, #2 (CarLife's phone key)
    {0x491bc, 0xe1a02000, 0xe2862010}, // mov r2, r0       -> add r2, r6, #16: 16 + the action
    {0x491c0, 0xe59f00c4, 0xe5933064}, // ldr r0, [pc, #196] -> ldr r3, [r3, #100] (RotateKeyEvent)
    {0x491c4, 0xe1a01004, 0xe12fff33}, // mov r1, r4       -> blx r3
    {0x491c8, 0xe1a03007, 0xeaffff6a}, // mov r3, r7       -> b 48bd0
    {0x49060, 0x1a000052, 0x1a00005b}, // bne 48e08        -> bne 48e2c: VR stop without its log line
    {0x4903c, 0xeaffffcd, 0xea00005b}, // b 48bd0          -> b 48e08: after MFLPhoneFunction
};

// --- callfix (PQ 0317; ported from the 0358 tables by kzb-re/port_appc_patch.py) ---
const Word kCallfix0317Guards[] = {
    {0x33aa8, 0xebffcdb8}, // bl   SVP_GetCarLifeService
    {0x33aac, 0xe5903000}, // ldr  r3, [r0]
    {0x33ab0, 0xe5933068}, // ldr  r3, [r3, #104]      GetCarLifeConnectStatus
    {0x33ab4, 0xe12fff33}, // blx  r3
    {0x33ab8, 0xe3500003}, // cmp  r0, #3
    {0x33ac0, 0xe1a00009}, // mov  r0, r9
    {0x33ac4, 0xebffff8f}, // bl   CAppconnectDataMgr::APPConnectIsFG
    {0x33ac8, 0xe3500000}, // cmp  r0, #0
    {0x33acc, 0x0affffd2}, // beq  33630
    {0x33ad8, 0xebffcd70}, // bl   sleep
    {0x33b08, 0xe3a00042}, // mov  r0, #66             the Bluetooth app
    {0x33b0c, 0xe28d1010}, // add  r1, sp, #16         "bt_carlife_temp_call"
    {0x33b10, 0xe3a0200c}, // mov  r2, #12
    {0x33b14, 0xe3a03001}, // mov  r3, #1
    {0x33b18, 0xebffcd9f}, // bl   SVP_StartApp
};
const Edit kCallfix0317Edits[] = {
    {0x33abc, 0x1affffd6, 0xeaffffd6}, // bne 33630 -> b 33630
};
// --- iphone (PQ 0317; ported from the 0358 tables by kzb-re/port_appc_patch.py) ---
const Word kIphone0317Guards[] = {
    {0x362f0, 0xe3550001}, // cmp   r5, #1               type 1 ...
    {0x362f4, 0x13550004}, // cmpne r5, #4               ... or 4 (Apple)
    {0x362fc, 0xe3760001}, // cmn   r6, #1               no stored choice
    {0x36300, 0x0a000041}, // beq   36020                -> the pop-up path
    {0x36304, 0xe28dd014}, // add   sp, sp, #20          the normal return
    {0x36448, 0xebffc323}, // bl    printf               its log line
    {0x3644c, 0xe3a00001}, // mov   r0, #1
    {0x36450, 0xebffc312}, // bl    sleep
    {0x36464, 0xe1a0200c}, // mov   r2, ip               (rest of the post, now unreached)
    {0x36478, 0xe12fff3c}, // blx   ip
    {0x31c40, 0xe92d4070}, // SVPSingleton<AppconncetPopUpMgr>::GetInstance: push {r4-r6, lr}
    {0x3813c, 0xe92d4ff0}, // AppconncetPopUpMgr::UserSelectEstablishSession: push {r4-fp, lr}
    {0x381f4, 0xe3540002}, // ... cmp r4, #2             2 = CarLife (its own branch)
    {0x381b0, 0xe3a07001}, // ... mov r7, #1
    {0x38208, 0xe1540007}, // ... cmp r4, r7             r7 = 1: CarPlay
    {0x3820c, 0x0a000018}, // ... beq 37e88              the CarPlay branch
};
const Edit kIphone0317Edits[] = {
    {0x36454, 0xebffc4d7, 0xebffedf9}, // bl GetWS     -> bl GetInstance (0x31864)
    {0x36458, 0xe590e000, 0xe3a01001}, // ldr lr, [r0] -> mov r1, #1 (CarPlay)
    {0x3645c, 0xe3a0c000, 0xeb000736}, // mov ip, #0   -> bl UserSelectEstablishSession (0x37d50)
    {0x36460, 0xe3a01006, 0xeaffffa7}, // mov r1, #6   -> b 35f18
};
// --- iphonetile (PQ 0317; ported from the 0358 tables by kzb-re/port_appc_patch.py) ---
const Word kIphonetile0317Guards[] = {
    {0x2f0d0, 0xe5d3604d}, // ldrb r6, [r3, #77]         MirrorLink coded? (the other branch
    {0x2f0d4, 0xe3560000}, // cmp r6, #0                 when not)
    {0x2f0d8, 0x1a000048}, // bne 2eb80
    {0x2f0dc, 0xe5940058}, // ldr r0, [r4, #88]          the other branch: hide the small tiles
    {0x2f0e0, 0xe1a01006}, // mov r1, r6                 ... with r6
    {0x2f140, 0xebffedcb}, // bl DataMgr::GetInstance
    {0x2f144, 0xe5d0304c}, // ldrb r3, [r0, #76]
    {0x2f148, 0xe3530000}, // cmp r3, #0
    {0x2f14c, 0x1a0000ae}, // bne 2ed8c
    {0x2f3b8, 0xebfff675}, // bl SVPIsAppleDevice        the Apple block's entry
    {0x2f3bc, 0xe2506000}, // subs r6, r0, #0
    {0x2f3c0, 0x0a000035}, // beq 2ee1c                  (not Apple)
    {0x2f40c, 0xebffed18}, // bl DataMgr::GetInstance
    {0x2f410, 0xe5d0304e}, // ldrb r3, [r0, #78]
    {0x2f414, 0xe3530000}, // cmp r3, #0
    {0x2f418, 0x0affff4c}, // beq 2ead0
    {0x2f420, 0xebfff65b}, // bl SVPIsAppleDevice
    {0x2f424, 0xe2507000}, // subs r7, r0, #0
    {0x2f428, 0x0a00000f}, // beq 2edec
    {0x2f440, 0xe5940068}, // ldr r0, [r4, #104]         the large CarPlay tile ...
    {0x2f444, 0xe3a01001}, // mov r1, #1                 ... visible
    {0x2f468, 0xeaffff4f}, // b 2eb2c                    the function's tail
};
const Edit kIphonetile0317Edits[] = {
    {0x2f3c4, 0xe5940058, 0xe3a06000}, // ldr r0, [r4, #88] -> mov r6, #0
    {0x2f3c8, 0xe3a01000, 0xeaffff43}, // mov r1, #0        -> b 2ea5c
};
// --- iphonetile2 (PQ 0317; ported from the 0358 tables by kzb-re/port_appc_patch.py) ---
const Word kIphonetile20317Guards[] = {
    {0x2e72c, 0xe5d3604d}, // ldrb r6, [r3, #77]
    {0x2e730, 0xe3560000}, // cmp r6, #0
    {0x2e734, 0x1a000057}, // bne 2e218
    {0x2e738, 0xe5940058}, // ldr r0, [r4, #88]
    {0x2e73c, 0xe1a01006}, // mov r1, r6
    {0x2e7b0, 0xebfff02f}, // bl DataMgr::GetInstance
    {0x2e7b4, 0xe5d0304c}, // ldrb r3, [r0, #76]
    {0x2e7b8, 0xe3530000}, // cmp r3, #0
    {0x2e7bc, 0x1a0000ce}, // bne 2e47c
    {0x2ea64, 0xebfff8ca}, // bl SVPIsAppleDevice
    {0x2ea68, 0xe2506000}, // subs r6, r0, #0
    {0x2ea6c, 0x1a00004e}, // bne 2e52c                  the Apple block's entry
    {0x2eafc, 0xebffef5c}, // bl DataMgr::GetInstance
    {0x2eb00, 0xe5d0304e}, // ldrb r3, [r0, #78]
    {0x2eb04, 0xe3530000}, // cmp r3, #0
    {0x2eb08, 0x0affff2c}, // beq 2e140
    {0x2eb10, 0xebfff89f}, // bl SVPIsAppleDevice
    {0x2eb14, 0xe2507000}, // subs r7, r0, #0
    {0x2eb18, 0x1a000013}, // bne 2e4ec
    {0x2eb80, 0xe5940068}, // ldr r0, [r4, #104]
    {0x2eb84, 0xe3a01001}, // mov r1, #1
    {0x2eba8, 0xeaffff25}, // b 2e1c4
};
const Edit kIphonetile20317Edits[] = {
    {0x2ebac, 0xe5940058, 0xe3a06000}, // ldr r0, [r4, #88] -> mov r6, #0
    {0x2ebb0, 0xe3a01000, 0xeafffee0}, // mov r1, #0        -> b 2e0b8
};
// --- seekhold (PQ 0317; ported from the 0358 tables by kzb-re/port_appc_patch.py) ---
const Word kSeekhold0317Guards[] = {
    {0x48fbc, 0xe28dd020}, // add  sp, sp, #32          the function's return
    {0x48fc0, 0xe8bd87f0}, // pop  {r4-sl, pc}
    {0x48fc4, 0xe300330d}, // movw r3, #781
    {0x48fcc, 0x0a00005c}, // beq  48d58                vkey 781: seek +
    {0x48fdc, 0xe3540fc3}, // cmp  r4, #780             vkey 780: seek -
    {0x48fe0, 0x1afffff5}, // bne  48bd0
    {0x48fe4, 0xe3560003}, // cmp  r6, #3               the press only (r6: the action)
    {0x48fec, 0xe59d3010}, // ldr  r3, [sp, #16]        the app in front
    {0x48ff8, 0xebff7864}, // bl   SVP_GetCarLifeService
    {0x48ffc, 0xe5903000}, // ldr  r3, [r0]
    {0x49000, 0xe3a0100f}, // mov  r1, #15              CarLife's seek -
    {0x49008, 0xe5933064}, // ldr  r3, [r3, #100]       RotateKeyEvent
    {0x4900c, 0xe12fff33}, // blx  r3
    {0x49010, 0xeaffffe9}, // b    48bd0
    {0x49144, 0xe3560003}, // cmp  r6, #3
    {0x49158, 0xebff780c}, // bl   SVP_GetCarLifeService
    {0x4915c, 0xe5903000}, // ldr  r3, [r0]
    {0x49160, 0xe3a01010}, // mov  r1, #16              CarLife's seek +
    {0x49168, 0xe5933064}, // ldr  r3, [r3, #100]       RotateKeyEvent
    {0x4916c, 0xe12fff33}, // blx  r3
    {0x49170, 0xeaffff91}, // b    48bd0
    {0x491c0, 0xe5933078}, // ldr  r3, [r3, #120]       GetModuleStatus(4) with appconnect in
    {0x491c8, 0xe3500001}, // cmp  r0, #1               front: no seek during a voice session
    {0x491cc, 0x1affff89}, // bne  48c0c
    {0x491e0, 0xe5933078}, // ldr  r3, [r3, #120]
    {0x491e8, 0xe3500001}, // cmp  r0, #1
    {0x491ec, 0x1affffd9}, // bne  48d6c
};
const Edit kSeekhold0317Edits[] = {
    {0x48fe8, 0x1afffff3, 0xe1a00000}, // bne 48bd0  -> nop: every action, not only the press
    {0x49004, 0xe3a02001, 0xe2862010}, // mov r2, #1 -> add r2, r6, #16: 16 + the action
    {0x49148, 0x1affff9b, 0xe1a00000}, // bne 48bd0  -> nop
    {0x49164, 0xe3a02001, 0xe2862010}, // mov r2, #1 -> add r2, r6, #16
};
// --- callkey (PQ 0317; ported from the 0358 tables by kzb-re/port_appc_patch.py) ---
const Word kCallkey0317Guards[] = {
    {0x48fbc, 0xe28dd020}, // add  sp, sp, #32          the function's return
    {0x48fc0, 0xe8bd87f0}, // pop  {r4-sl, pc}
    {0x48f9c, 0xe3003325}, // movw r3, #805             vkey 805: the phone key
    {0x48fa4, 0x0a00002c}, // beq  48c70
    {0x4905c, 0xebff784b}, // bl   SVP_GetCarLifeService
    {0x49060, 0xe5903000}, // ldr  r3, [r0]
    {0x49064, 0xe5933070}, // ldr  r3, [r3, #112]       NeedBlockBTPopup: 1 while AA is on screen
    {0x49068, 0xe12fff33}, // blx  r3
    {0x4906c, 0xe3500000}, // cmp  r0, #0
    {0x49070, 0x0affffd1}, // beq  48bd0
    {0x49074, 0xe1a0000a}, // mov  r0, sl
    {0x49078, 0xe1a01006}, // mov  r1, r6               the action (r6 is kept across calls)
    {0x4907c, 0xebfffdca}, // bl   MFLPhoneFunction     answers / ends a call: unchanged
    {0x49084, 0xe3560004}, // cmp  r6, #4               vkey 804 (voice), its release ...
    {0x490a0, 0xe3500000}, // cmp  r0, #0               ... during a voice session:
    {0x49210, 0xe08f0000}, // add  r0, pc, r0           (the rest of the log line, now unreached)
    {0x49214, 0xebff77b0}, // bl   printf
    {0x49218, 0xebff77dc}, // bl   SVP_GetCarLifeService   ... VR stop
    {0x4921c, 0xe5903000}, // ldr  r3, [r0]
    {0x49220, 0xe3a01022}, // mov  r1, #34
    {0x49224, 0xe3a02001}, // mov  r2, #1
    {0x49228, 0xe5933064}, // ldr  r3, [r3, #100]       RotateKeyEvent
    {0x4922c, 0xe12fff33}, // blx  r3
    {0x49230, 0xeaffff61}, // b    48bd0
};
const Edit kCallkey0317Edits[] = {
    {0x491f4, 0xebff78c7, 0xebff77e5}, // bl getpid        -> bl SVP_GetCarLifeService
    {0x491f8, 0xe1a04000, 0xe5903000}, // mov r4, r0       -> ldr r3, [r0]
    {0x491fc, 0xebff7835, 0xe3a01002}, // bl GetCurrentThreadID -> mov r1, #2 (CarLife's phone key)
    {0x49200, 0xe1a02000, 0xe2862010}, // mov r2, r0       -> add r2, r6, #16: 16 + the action
    {0x49204, 0xe59f00c4, 0xe5933064}, // ldr r0, [pc, #196] -> ldr r3, [r3, #100] (RotateKeyEvent)
    {0x49208, 0xe1a01004, 0xe12fff33}, // mov r1, r4       -> blx r3
    {0x4920c, 0xe1a03007, 0xeaffff6a}, // mov r3, r7       -> b 48bd0
    {0x490a4, 0x1a000052, 0x1a00005b}, // bne 48e08        -> bne 48e2c: VR stop without its log line
    {0x49080, 0xeaffffcd, 0xea00005b}, // b 48bd0          -> b 48e08: after MFLPhoneFunction
};

struct Patch {
    const char* name;  // the change; its variants (one per appconnect build) share the name
    const char* build; // the appconnect build this variant is for
    const char* what;  // logged when applied
    const Word* guards;
    size_t nGuards;
    const Edit* edits; // runs of consecutive words, written run by run in this order
    size_t nEdits;
};
#define APPC_N(a) (sizeof(a) / sizeof((a)[0]))
#define APPC_P(name, build, what, g, e) {name, build, what, g, APPC_N(g), e, APPC_N(e)}
const char* const kCallfixWhat = "outgoing calls no longer open the phone screen over Android Auto";
const char* const kIphoneWhat = "an iPhone goes straight to CarPlay (no CarPlay-or-Android-Auto question)";
const char* const kIphonetileWhat = "an iPhone sees the CarPlay tile alone (connect screen)";
const char* const kIphonetile2What = "an iPhone sees the CarPlay tile alone (connect screen updates)";
const char* const kSeekholdWhat = "the seek keys tell Android Auto press, hold and release (holding one seeks)";
const char* const kCallkeyWhat = "the phone key also reaches Android Auto (with no call it opens AA's phone screen)";
const Patch kPatches[] = {
    APPC_P("callfix", "0358", kCallfixWhat, kCallfixGuards, kCallfixEdits),
    APPC_P("callfix", "0394", kCallfixWhat, kCallfix0394Guards, kCallfix0394Edits),
    APPC_P("callfix", "0317", kCallfixWhat, kCallfix0317Guards, kCallfix0317Edits),
    APPC_P("iphone", "0358", kIphoneWhat, kIphoneGuards, kIphoneEdits),
    APPC_P("iphone", "0394", kIphoneWhat, kIphone0394Guards, kIphone0394Edits),
    APPC_P("iphone", "0317", kIphoneWhat, kIphone0317Guards, kIphone0317Edits),
    APPC_P("iphonetile", "0358", kIphonetileWhat, kIphonetileGuards, kIphonetileEdits),
    APPC_P("iphonetile", "0394", kIphonetileWhat, kIphonetile0394Guards, kIphonetile0394Edits),
    APPC_P("iphonetile", "0317", kIphonetileWhat, kIphonetile0317Guards, kIphonetile0317Edits),
    APPC_P("iphonetile2", "0358", kIphonetile2What, kIphonetile2Guards, kIphonetile2Edits),
    APPC_P("iphonetile2", "0394", kIphonetile2What, kIphonetile20394Guards, kIphonetile20394Edits),
    APPC_P("iphonetile2", "0317", kIphonetile2What, kIphonetile20317Guards, kIphonetile20317Edits),
    APPC_P("seekhold", "0358", kSeekholdWhat, kSeekholdGuards, kSeekholdEdits),
    APPC_P("seekhold", "0394", kSeekholdWhat, kSeekhold0394Guards, kSeekhold0394Edits),
    APPC_P("seekhold", "0317", kSeekholdWhat, kSeekhold0317Guards, kSeekhold0317Edits),
    APPC_P("callkey", "0358", kCallkeyWhat, kCallkeyGuards, kCallkeyEdits),
    APPC_P("callkey", "0394", kCallkeyWhat, kCallkey0394Guards, kCallkey0394Edits),
    APPC_P("callkey", "0317", kCallkeyWhat, kCallkey0317Guards, kCallkey0317Edits),
};
#undef APPC_P
const size_t kNumPatches = APPC_N(kPatches);
#undef APPC_N

inline std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline std::string format(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

inline bool readWord(int fd, unsigned long addr, uint32_t& w) {
    return ::pread(fd, &w, sizeof w, (off_t)addr) == (ssize_t)sizeof w;
}

// When process `pid` started (/proc/<pid>/stat field 22, clock ticks since boot); 0 if gone. With
// the pid it names one process: a pid can be reused, a pid and its start time cannot.
inline unsigned long long startTime(int pid) {
    char path[64], buf[512];
    std::snprintf(path, sizeof path, "/proc/%d/stat", pid);
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    const ssize_t n = ::read(fd, buf, sizeof buf - 1);
    ::close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    const char* p = std::strrchr(buf, ')'); // the name may hold spaces and parentheses
    if (!p) return 0;
    unsigned long long start = 0;
    // after ")": state (3), ppid (4) ... starttime (22)
    if (std::sscanf(p + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu",
                    &start) != 1)
        return 0;
    return start;
}

// The processes already found with every change on, so they are not opened again; filled and
// pruned by apply(). `found` is how many processes the last apply() saw.
struct Seen {
    std::map<int, unsigned long long> on; // pid -> start time
    int found = 0;
};

// Whether every guard word of variant `p` is the process's.
inline bool guardsMatch(int rfd, const Patch& p) {
    uint32_t w = 0;
    for (size_t i = 0; i < p.nGuards; ++i)
        if (!readWord(rfd, p.guards[i].addr, w) || w != p.guards[i].word) return false;
    return true;
}

// One patch in one process, checked through `rfd` (read-only); `wfd` is opened read-write on
// `path` only when a change is to be written. True when it is on (now or before); `msg` says
// what was done or why not, and stays empty when it was already on.
inline bool applyOne(int rfd, int& wfd, const char* path, int pid, const Patch& p, std::string& msg) {
    uint32_t w = 0;
    for (size_t i = 0; i < p.nGuards; ++i) {
        const Word& g = p.guards[i];
        if (!readWord(rfd, g.addr, w) || w != g.word) {
            msg = format("%s: pid %d is not the expected appconnect (0x%lx: 0x%08x, expected 0x%08x); left alone",
                         p.name, pid, g.addr, (unsigned)w, (unsigned)g.word);
            return false;
        }
    }
    size_t stock = 0, patched = 0;
    uint32_t want[16];
    if (p.nEdits > 16) return false;
    for (size_t i = 0; i < p.nEdits; ++i) {
        const Edit& e = p.edits[i];
        if (!readWord(rfd, e.addr, w)) {
            msg = format("%s: pid %d: reading 0x%lx: %s", p.name, pid, e.addr, std::strerror(errno));
            return false;
        }
        if (w == e.stock) ++stock;
        if (w == e.patched) ++patched;
        want[i] = e.patched;
    }
    if (patched == p.nEdits) return true;
    if (stock != p.nEdits) {
        msg = format("%s: pid %d: the code to change is neither stock nor patched; left alone", p.name, pid);
        return false;
    }
    if (wfd < 0) wfd = ::open(path, O_RDWR | O_CLOEXEC);
    for (size_t i = 0, end; i < p.nEdits; i = end) {
        for (end = i + 1; end < p.nEdits && p.edits[end].addr == p.edits[end - 1].addr + 4; ++end) {}
        const size_t n = (end - i) * sizeof(uint32_t);
        uint32_t back[16];
        if (wfd < 0 || ::pwrite(wfd, want + i, n, (off_t)p.edits[i].addr) != (ssize_t)n ||
            ::pread(rfd, back, n, (off_t)p.edits[i].addr) != (ssize_t)n || std::memcmp(back, want + i, n) != 0) {
            msg = format("%s: pid %d: writing 0x%lx failed: %s", p.name, pid, p.edits[i].addr, std::strerror(errno));
            return false;
        }
    }
    msg = format("%s: pid %d: %s (in memory only)", p.name, pid, p.what);
    return true;
}

// Every patch in one process. True when all are on.
inline bool patchProcess(int pid, std::string& msg) {
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d/mem", pid);
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        msg = format("%s: %s", path, std::strerror(errno));
        return false;
    }
    int wfd = -1;
    bool all = true;
    for (size_t i = 0; i < kNumPatches; ++i) {
        if (i > 0 && std::strcmp(kPatches[i].name, kPatches[i - 1].name) == 0) continue; // a variant, done
        // The variant for this appconnect build: the first whose guard words all match.
        const Patch* use = nullptr;
        for (size_t j = i; j < kNumPatches && std::strcmp(kPatches[j].name, kPatches[i].name) == 0; ++j)
            if (guardsMatch(fd, kPatches[j])) {
                use = &kPatches[j];
                break;
            }
        std::string one;
        if (!use) {
            one = format("%s: pid %d is not a known appconnect build; left alone", kPatches[i].name, pid);
            all = false;
        } else if (!applyOne(fd, wfd, path, pid, *use, one)) {
            all = false;
        }
        if (!one.empty()) msg += (msg.empty() ? "" : "; ") + one;
    }
    if (wfd >= 0) ::close(wfd);
    ::close(fd);
    return all;
}

// Patch every process named `comm` (appconnect). True when all of them, and at least one, have
// every patch on; `msg` collects what was done, empty when there was nothing new. With `seen`,
// a process found with every change on is not opened again while it lives, and the first time
// it is found that way (nothing to write: the patched file) that is said in `msg`.
inline bool apply(std::string& msg, const char* comm = "appconnect", Seen* seen = nullptr) {
    msg.clear();
    DIR* d = opendir("/proc");
    if (!d) {
        msg = format("/proc: %s", std::strerror(errno));
        return false;
    }
    int found = 0;
    bool all = true;
    std::map<int, unsigned long long> alive;
    while (dirent* e = readdir(d)) {
        char* end = nullptr;
        const long pid = std::strtol(e->d_name, &end, 10);
        if (!end || *end != '\0' || pid <= 0) continue;
        char path[64], name[32] = {0};
        std::snprintf(path, sizeof path, "/proc/%ld/comm", pid);
        FILE* f = std::fopen(path, "r");
        if (!f) continue;
        if (std::fgets(name, sizeof name, f)) name[std::strcspn(name, "\n")] = '\0';
        std::fclose(f);
        if (std::strcmp(name, comm) != 0) continue;
        ++found;
        const unsigned long long start = seen ? startTime(static_cast<int>(pid)) : 0;
        if (seen) {
            std::map<int, unsigned long long>::const_iterator it = seen->on.find(static_cast<int>(pid));
            if (it != seen->on.end() && it->second == start && start) {
                alive[static_cast<int>(pid)] = start;
                continue;
            }
        }
        std::string one;
        const bool ok = patchProcess(static_cast<int>(pid), one);
        if (!ok) all = false;
        if (ok && seen && start) {
            alive[static_cast<int>(pid)] = start;
            if (one.empty()) one = format("pid %ld: every change is in place already (the patched appconnect)", pid);
        }
        if (!one.empty()) msg += (msg.empty() ? "" : "; ") + one;
    }
    closedir(d);
    if (seen) {
        seen->on.swap(alive);
        seen->found = found;
    }
    if (!found) msg = format("no %s process", comm);
    return found > 0 && all;
}

} // namespace appc_patch

#endif // AA_CARLIFE_SHIM_APPC_PATCH_HPP
