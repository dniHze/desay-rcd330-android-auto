// Generated from aa-carlife-shim/src/appc_patch.hpp by gen_tables.py: the guard and edit
// words of each change, per appconnect build (the header's comments kept). The header stays
// the source of truth (kzb-re/patch_appconnect.py reads it); tests/appc_tables.rs checks that
// these equal it.

use super::{Edit, Patch, Word};

const CALLFIX_GUARDS: &[Word] = &[
    Word { addr: 0x336bc, word: 0xebffce71 }, // bl   SVP_GetCarLifeService
    Word { addr: 0x336c0, word: 0xe5903000 }, // ldr  r3, [r0]
    Word { addr: 0x336c4, word: 0xe5933068 }, // ldr  r3, [r3, #104]      GetCarLifeConnectStatus
    Word { addr: 0x336c8, word: 0xe12fff33 }, // blx  r3
    Word { addr: 0x336cc, word: 0xe3500003 }, // cmp  r0, #3
    Word { addr: 0x336d4, word: 0xe1a00009 }, // mov  r0, r9
    Word { addr: 0x336d8, word: 0xebffff8f }, // bl   CAppconnectDataMgr::APPConnectIsFG
    Word { addr: 0x336dc, word: 0xe3500000 }, // cmp  r0, #0
    Word { addr: 0x336e0, word: 0x0affffd2 }, // beq  33630
    Word { addr: 0x336ec, word: 0xebffce2c }, // bl   sleep
    Word { addr: 0x3371c, word: 0xe3a00042 }, // mov  r0, #66             the Bluetooth app
    Word { addr: 0x33720, word: 0xe28d1010 }, // add  r1, sp, #16         "bt_carlife_temp_call"
    Word { addr: 0x33724, word: 0xe3a0200c }, // mov  r2, #12
    Word { addr: 0x33728, word: 0xe3a03001 }, // mov  r3, #1
    Word { addr: 0x3372c, word: 0xebffce58 }, // bl   SVP_StartApp
];
const CALLFIX_EDITS: &[Edit] = &[
    Edit { addr: 0x336d0, stock: 0x1affffd6, patched: 0xeaffffd6 }, // bne 33630 -> b 33630
];
const IPHONE_GUARDS: &[Word] = &[
    Word { addr: 0x35f04, word: 0xe3550001 }, // cmp   r5, #1               type 1 ...
    Word { addr: 0x35f08, word: 0x13550004 }, // cmpne r5, #4               ... or 4 (Apple)
    Word { addr: 0x35f10, word: 0xe3760001 }, // cmn   r6, #1               no stored choice
    Word { addr: 0x35f14, word: 0x0a000041 }, // beq   36020                -> the pop-up path
    Word { addr: 0x35f18, word: 0xe28dd014 }, // add   sp, sp, #20          the normal return
    Word { addr: 0x3605c, word: 0xebffc3df }, // bl    printf               its log line
    Word { addr: 0x36060, word: 0xe3a00001 }, // mov   r0, #1
    Word { addr: 0x36064, word: 0xebffc3ce }, // bl    sleep
    Word { addr: 0x36078, word: 0xe1a0200c }, // mov   r2, ip               (rest of the post, now unreached)
    Word { addr: 0x3608c, word: 0xe12fff3c }, // blx   ip
    Word { addr: 0x31864, word: 0xe92d4070 }, // SVPSingleton<AppconncetPopUpMgr>::GetInstance: push {r4-r6, lr}
    Word { addr: 0x37d50, word: 0xe92d4ff0 }, // AppconncetPopUpMgr::UserSelectEstablishSession: push {r4-fp, lr}
    Word { addr: 0x37e08, word: 0xe3540002 }, // ... cmp r4, #2             2 = CarLife (its own branch)
    Word { addr: 0x37dc4, word: 0xe3a07001 }, // ... mov r7, #1
    Word { addr: 0x37e1c, word: 0xe1540007 }, // ... cmp r4, r7             r7 = 1: CarPlay
    Word { addr: 0x37e20, word: 0x0a000018 }, // ... beq 37e88              the CarPlay branch
];
const IPHONE_EDITS: &[Edit] = &[
    Edit { addr: 0x36068, stock: 0xebffc590, patched: 0xebffedfd }, // bl GetWS     -> bl GetInstance (0x31864)
    Edit { addr: 0x3606c, stock: 0xe590e000, patched: 0xe3a01001 }, // ldr lr, [r0] -> mov r1, #1 (CarPlay)
    Edit { addr: 0x36070, stock: 0xe3a0c000, patched: 0xeb000736 }, // mov ip, #0   -> bl UserSelectEstablishSession (0x37d50)
    Edit { addr: 0x36074, stock: 0xe3a01006, patched: 0xeaffffa7 }, // mov r1, #6   -> b 35f18
];
const IPHONETILE_GUARDS: &[Word] = &[
    Word { addr: 0x2ea50, word: 0xe5d3604d }, // ldrb r6, [r3, #77]         MirrorLink coded? (the other branch
    Word { addr: 0x2ea54, word: 0xe3560000 }, // cmp r6, #0                 when not)
    Word { addr: 0x2ea58, word: 0x1a000048 }, // bne 2eb80
    Word { addr: 0x2ea5c, word: 0xe5940058 }, // ldr r0, [r4, #88]          the other branch: hide the small tiles
    Word { addr: 0x2ea60, word: 0xe1a01006 }, // mov r1, r6                 ... with r6
    Word { addr: 0x2eac0, word: 0xebffef29 }, // bl DataMgr::GetInstance
    Word { addr: 0x2eac4, word: 0xe5d0304c }, // ldrb r3, [r0, #76]
    Word { addr: 0x2eac8, word: 0xe3530000 }, // cmp r3, #0
    Word { addr: 0x2eacc, word: 0x1a0000ae }, // bne 2ed8c
    Word { addr: 0x2ed38, word: 0xebfff675 }, // bl SVPIsAppleDevice        the Apple block's entry
    Word { addr: 0x2ed3c, word: 0xe2506000 }, // subs r6, r0, #0
    Word { addr: 0x2ed40, word: 0x0a000035 }, // beq 2ee1c                  (not Apple)
    Word { addr: 0x2ed8c, word: 0xebffee76 }, // bl DataMgr::GetInstance
    Word { addr: 0x2ed90, word: 0xe5d0304e }, // ldrb r3, [r0, #78]
    Word { addr: 0x2ed94, word: 0xe3530000 }, // cmp r3, #0
    Word { addr: 0x2ed98, word: 0x0affff4c }, // beq 2ead0
    Word { addr: 0x2eda0, word: 0xebfff65b }, // bl SVPIsAppleDevice
    Word { addr: 0x2eda4, word: 0xe2507000 }, // subs r7, r0, #0
    Word { addr: 0x2eda8, word: 0x0a00000f }, // beq 2edec
    Word { addr: 0x2edc0, word: 0xe5940068 }, // ldr r0, [r4, #104]         the large CarPlay tile ...
    Word { addr: 0x2edc4, word: 0xe3a01001 }, // mov r1, #1                 ... visible
    Word { addr: 0x2ede8, word: 0xeaffff4f }, // b 2eb2c                    the function's tail
];
const IPHONETILE_EDITS: &[Edit] = &[
    Edit { addr: 0x2ed44, stock: 0xe5940058, patched: 0xe3a06000 }, // ldr r0, [r4, #88] -> mov r6, #0
    Edit { addr: 0x2ed48, stock: 0xe3a01000, patched: 0xeaffff43 }, // mov r1, #0        -> b 2ea5c
];
const IPHONETILE2_GUARDS: &[Word] = &[
    Word { addr: 0x2e0ac, word: 0xe5d3604d }, // ldrb r6, [r3, #77]
    Word { addr: 0x2e0b0, word: 0xe3560000 }, // cmp r6, #0
    Word { addr: 0x2e0b4, word: 0x1a000057 }, // bne 2e218
    Word { addr: 0x2e0b8, word: 0xe5940058 }, // ldr r0, [r4, #88]
    Word { addr: 0x2e0bc, word: 0xe1a01006 }, // mov r1, r6
    Word { addr: 0x2e130, word: 0xebfff18d }, // bl DataMgr::GetInstance
    Word { addr: 0x2e134, word: 0xe5d0304c }, // ldrb r3, [r0, #76]
    Word { addr: 0x2e138, word: 0xe3530000 }, // cmp r3, #0
    Word { addr: 0x2e13c, word: 0x1a0000ce }, // bne 2e47c
    Word { addr: 0x2e3e4, word: 0xebfff8ca }, // bl SVPIsAppleDevice
    Word { addr: 0x2e3e8, word: 0xe2506000 }, // subs r6, r0, #0
    Word { addr: 0x2e3ec, word: 0x1a00004e }, // bne 2e52c                  the Apple block's entry
    Word { addr: 0x2e47c, word: 0xebfff0ba }, // bl DataMgr::GetInstance
    Word { addr: 0x2e480, word: 0xe5d0304e }, // ldrb r3, [r0, #78]
    Word { addr: 0x2e484, word: 0xe3530000 }, // cmp r3, #0
    Word { addr: 0x2e488, word: 0x0affff2c }, // beq 2e140
    Word { addr: 0x2e490, word: 0xebfff89f }, // bl SVPIsAppleDevice
    Word { addr: 0x2e494, word: 0xe2507000 }, // subs r7, r0, #0
    Word { addr: 0x2e498, word: 0x1a000013 }, // bne 2e4ec
    Word { addr: 0x2e500, word: 0xe5940068 }, // ldr r0, [r4, #104]
    Word { addr: 0x2e504, word: 0xe3a01001 }, // mov r1, #1
    Word { addr: 0x2e528, word: 0xeaffff25 }, // b 2e1c4
];
const IPHONETILE2_EDITS: &[Edit] = &[
    Edit { addr: 0x2e52c, stock: 0xe5940058, patched: 0xe3a06000 }, // ldr r0, [r4, #88] -> mov r6, #0
    Edit { addr: 0x2e530, stock: 0xe3a01000, patched: 0xeafffee0 }, // mov r1, #0        -> b 2e0b8
];
const SEEKHOLD_GUARDS: &[Word] = &[
    Word { addr: 0x48bd0, word: 0xe28dd020 }, // add  sp, sp, #32          the function's return
    Word { addr: 0x48bd4, word: 0xe8bd87f0 }, // pop  {r4-sl, pc}
    Word { addr: 0x48bd8, word: 0xe300330d }, // movw r3, #781
    Word { addr: 0x48be0, word: 0x0a00005c }, // beq  48d58                vkey 781: seek +
    Word { addr: 0x48bf0, word: 0xe3540fc3 }, // cmp  r4, #780             vkey 780: seek -
    Word { addr: 0x48bf4, word: 0x1afffff5 }, // bne  48bd0
    Word { addr: 0x48bf8, word: 0xe3560003 }, // cmp  r6, #3               the press only (r6: the action)
    Word { addr: 0x48c00, word: 0xe59d3010 }, // ldr  r3, [sp, #16]        the app in front
    Word { addr: 0x48c0c, word: 0xebff791d }, // bl   SVP_GetCarLifeService
    Word { addr: 0x48c10, word: 0xe5903000 }, // ldr  r3, [r0]
    Word { addr: 0x48c14, word: 0xe3a0100f }, // mov  r1, #15              CarLife's seek -
    Word { addr: 0x48c1c, word: 0xe5933064 }, // ldr  r3, [r3, #100]       RotateKeyEvent
    Word { addr: 0x48c20, word: 0xe12fff33 }, // blx  r3
    Word { addr: 0x48c24, word: 0xeaffffe9 }, // b    48bd0
    Word { addr: 0x48d58, word: 0xe3560003 }, // cmp  r6, #3
    Word { addr: 0x48d6c, word: 0xebff78c5 }, // bl   SVP_GetCarLifeService
    Word { addr: 0x48d70, word: 0xe5903000 }, // ldr  r3, [r0]
    Word { addr: 0x48d74, word: 0xe3a01010 }, // mov  r1, #16              CarLife's seek +
    Word { addr: 0x48d7c, word: 0xe5933064 }, // ldr  r3, [r3, #100]       RotateKeyEvent
    Word { addr: 0x48d80, word: 0xe12fff33 }, // blx  r3
    Word { addr: 0x48d84, word: 0xeaffff91 }, // b    48bd0
    Word { addr: 0x48dd4, word: 0xe5933078 }, // ldr  r3, [r3, #120]       GetModuleStatus(4) with appconnect in
    Word { addr: 0x48ddc, word: 0xe3500001 }, // cmp  r0, #1               front: no seek during a voice session
    Word { addr: 0x48de0, word: 0x1affff89 }, // bne  48c0c
    Word { addr: 0x48df4, word: 0xe5933078 }, // ldr  r3, [r3, #120]
    Word { addr: 0x48dfc, word: 0xe3500001 }, // cmp  r0, #1
    Word { addr: 0x48e00, word: 0x1affffd9 }, // bne  48d6c
];
const SEEKHOLD_EDITS: &[Edit] = &[
    Edit { addr: 0x48bfc, stock: 0x1afffff3, patched: 0xe1a00000 }, // bne 48bd0  -> nop: every action, not only the press
    Edit { addr: 0x48c18, stock: 0xe3a02001, patched: 0xe2862010 }, // mov r2, #1 -> add r2, r6, #16: 16 + the action
    Edit { addr: 0x48d5c, stock: 0x1affff9b, patched: 0xe1a00000 }, // bne 48bd0  -> nop
    Edit { addr: 0x48d78, stock: 0xe3a02001, patched: 0xe2862010 }, // mov r2, #1 -> add r2, r6, #16
];
const CALLKEY_GUARDS: &[Word] = &[
    Word { addr: 0x48bd0, word: 0xe28dd020 }, // add  sp, sp, #32          the function's return
    Word { addr: 0x48bd4, word: 0xe8bd87f0 }, // pop  {r4-sl, pc}
    Word { addr: 0x48bb0, word: 0xe3003325 }, // movw r3, #805             vkey 805: the phone key
    Word { addr: 0x48bb8, word: 0x0a00002c }, // beq  48c70
    Word { addr: 0x48c70, word: 0xebff7904 }, // bl   SVP_GetCarLifeService
    Word { addr: 0x48c74, word: 0xe5903000 }, // ldr  r3, [r0]
    Word { addr: 0x48c78, word: 0xe5933070 }, // ldr  r3, [r3, #112]       NeedBlockBTPopup: 1 while AA is on screen
    Word { addr: 0x48c7c, word: 0xe12fff33 }, // blx  r3
    Word { addr: 0x48c80, word: 0xe3500000 }, // cmp  r0, #0
    Word { addr: 0x48c84, word: 0x0affffd1 }, // beq  48bd0
    Word { addr: 0x48c88, word: 0xe1a0000a }, // mov  r0, sl
    Word { addr: 0x48c8c, word: 0xe1a01006 }, // mov  r1, r6               the action (r6 is kept across calls)
    Word { addr: 0x48c90, word: 0xebfffdca }, // bl   MFLPhoneFunction     answers / ends a call: unchanged
    Word { addr: 0x48c98, word: 0xe3560004 }, // cmp  r6, #4               vkey 804 (voice), its release ...
    Word { addr: 0x48cb4, word: 0xe3500000 }, // cmp  r0, #0               ... during a voice session:
    Word { addr: 0x48e24, word: 0xe08f0000 }, // add  r0, pc, r0           (the rest of the log line, now unreached)
    Word { addr: 0x48e28, word: 0xebff786c }, // bl   printf
    Word { addr: 0x48e2c, word: 0xebff7895 }, // bl   SVP_GetCarLifeService   ... VR stop
    Word { addr: 0x48e30, word: 0xe5903000 }, // ldr  r3, [r0]
    Word { addr: 0x48e34, word: 0xe3a01022 }, // mov  r1, #34
    Word { addr: 0x48e38, word: 0xe3a02001 }, // mov  r2, #1
    Word { addr: 0x48e3c, word: 0xe5933064 }, // ldr  r3, [r3, #100]       RotateKeyEvent
    Word { addr: 0x48e40, word: 0xe12fff33 }, // blx  r3
    Word { addr: 0x48e44, word: 0xeaffff61 }, // b    48bd0
];
const CALLKEY_EDITS: &[Edit] = &[
    Edit { addr: 0x48e08, stock: 0xebff7980, patched: 0xebff789e }, // bl getpid        -> bl SVP_GetCarLifeService
    Edit { addr: 0x48e0c, stock: 0xe1a04000, patched: 0xe5903000 }, // mov r4, r0       -> ldr r3, [r0]
    Edit { addr: 0x48e10, stock: 0xebff78ee, patched: 0xe3a01002 }, // bl GetCurrentThreadID -> mov r1, #2 (CarLife's phone key)
    Edit { addr: 0x48e14, stock: 0xe1a02000, patched: 0xe2862010 }, // mov r2, r0       -> add r2, r6, #16: 16 + the action
    Edit { addr: 0x48e18, stock: 0xe59f00c4, patched: 0xe5933064 }, // ldr r0, [pc, #196] -> ldr r3, [r3, #100] (RotateKeyEvent)
    Edit { addr: 0x48e1c, stock: 0xe1a01004, patched: 0xe12fff33 }, // mov r1, r4       -> blx r3
    Edit { addr: 0x48e20, stock: 0xe1a03007, patched: 0xeaffff6a }, // mov r3, r7       -> b 48bd0
    Edit { addr: 0x48cb8, stock: 0x1a000052, patched: 0x1a00005b }, // bne 48e08        -> bne 48e2c: VR stop without its log line
    Edit { addr: 0x48c94, stock: 0xeaffffcd, patched: 0xea00005b }, // b 48bd0          -> b 48e08: after MFLPhoneFunction
];
const CALLFIX_0394_GUARDS: &[Word] = &[
    Word { addr: 0x33bc4, word: 0xebffcda3 }, // bl   SVP_GetCarLifeService
    Word { addr: 0x33bc8, word: 0xe5903000 }, // ldr  r3, [r0]
    Word { addr: 0x33bcc, word: 0xe5933068 }, // ldr  r3, [r3, #104]      GetCarLifeConnectStatus
    Word { addr: 0x33bd0, word: 0xe12fff33 }, // blx  r3
    Word { addr: 0x33bd4, word: 0xe3500003 }, // cmp  r0, #3
    Word { addr: 0x33bdc, word: 0xe1a00009 }, // mov  r0, r9
    Word { addr: 0x33be0, word: 0xebffff8f }, // bl   CAppconnectDataMgr::APPConnectIsFG
    Word { addr: 0x33be4, word: 0xe3500000 }, // cmp  r0, #0
    Word { addr: 0x33be8, word: 0x0affffd2 }, // beq  33630
    Word { addr: 0x33bf4, word: 0xebffcd5b }, // bl   sleep
    Word { addr: 0x33c24, word: 0xe3a00042 }, // mov  r0, #66             the Bluetooth app
    Word { addr: 0x33c28, word: 0xe28d1010 }, // add  r1, sp, #16         "bt_carlife_temp_call"
    Word { addr: 0x33c2c, word: 0xe3a0200c }, // mov  r2, #12
    Word { addr: 0x33c30, word: 0xe3a03001 }, // mov  r3, #1
    Word { addr: 0x33c34, word: 0xebffcd8a }, // bl   SVP_StartApp
];
const CALLFIX_0394_EDITS: &[Edit] = &[
    Edit { addr: 0x33bd8, stock: 0x1affffd6, patched: 0xeaffffd6 }, // bne 33630 -> b 33630
];
const IPHONE_0394_GUARDS: &[Word] = &[
    Word { addr: 0x3640c, word: 0xe3550001 }, // cmp   r5, #1               type 1 ...
    Word { addr: 0x36410, word: 0x13550004 }, // cmpne r5, #4               ... or 4 (Apple)
    Word { addr: 0x36418, word: 0xe3760001 }, // cmn   r6, #1               no stored choice
    Word { addr: 0x3641c, word: 0x0a000041 }, // beq   36020                -> the pop-up path
    Word { addr: 0x36420, word: 0xe28dd014 }, // add   sp, sp, #20          the normal return
    Word { addr: 0x36564, word: 0xebffc30e }, // bl    printf               its log line
    Word { addr: 0x36568, word: 0xe3a00001 }, // mov   r0, #1
    Word { addr: 0x3656c, word: 0xebffc2fd }, // bl    sleep
    Word { addr: 0x36580, word: 0xe1a0200c }, // mov   r2, ip               (rest of the post, now unreached)
    Word { addr: 0x36594, word: 0xe12fff3c }, // blx   ip
    Word { addr: 0x31d5c, word: 0xe92d4070 }, // SVPSingleton<AppconncetPopUpMgr>::GetInstance: push {r4-r6, lr}
    Word { addr: 0x38258, word: 0xe92d4ff0 }, // AppconncetPopUpMgr::UserSelectEstablishSession: push {r4-fp, lr}
    Word { addr: 0x38310, word: 0xe3540002 }, // ... cmp r4, #2             2 = CarLife (its own branch)
    Word { addr: 0x382cc, word: 0xe3a07001 }, // ... mov r7, #1
    Word { addr: 0x38324, word: 0xe1540007 }, // ... cmp r4, r7             r7 = 1: CarPlay
    Word { addr: 0x38328, word: 0x0a000018 }, // ... beq 37e88              the CarPlay branch
];
const IPHONE_0394_EDITS: &[Edit] = &[
    Edit { addr: 0x36570, stock: 0xebffc4c2, patched: 0xebffedf9 }, // bl GetWS     -> bl GetInstance (0x31864)
    Edit { addr: 0x36574, stock: 0xe590e000, patched: 0xe3a01001 }, // ldr lr, [r0] -> mov r1, #1 (CarPlay)
    Edit { addr: 0x36578, stock: 0xe3a0c000, patched: 0xeb000736 }, // mov ip, #0   -> bl UserSelectEstablishSession (0x37d50)
    Edit { addr: 0x3657c, stock: 0xe3a01006, patched: 0xeaffffa7 }, // mov r1, #6   -> b 35f18
];
const IPHONETILE_0394_GUARDS: &[Word] = &[
    Word { addr: 0x2f1ec, word: 0xe5d3604d }, // ldrb r6, [r3, #77]         MirrorLink coded? (the other branch
    Word { addr: 0x2f1f0, word: 0xe3560000 }, // cmp r6, #0                 when not)
    Word { addr: 0x2f1f4, word: 0x1a000048 }, // bne 2eb80
    Word { addr: 0x2f1f8, word: 0xe5940058 }, // ldr r0, [r4, #88]          the other branch: hide the small tiles
    Word { addr: 0x2f1fc, word: 0xe1a01006 }, // mov r1, r6                 ... with r6
    Word { addr: 0x2f25c, word: 0xebffedcb }, // bl DataMgr::GetInstance
    Word { addr: 0x2f260, word: 0xe5d0304c }, // ldrb r3, [r0, #76]
    Word { addr: 0x2f264, word: 0xe3530000 }, // cmp r3, #0
    Word { addr: 0x2f268, word: 0x1a0000ae }, // bne 2ed8c
    Word { addr: 0x2f4d4, word: 0xebfff675 }, // bl SVPIsAppleDevice        the Apple block's entry
    Word { addr: 0x2f4d8, word: 0xe2506000 }, // subs r6, r0, #0
    Word { addr: 0x2f4dc, word: 0x0a000035 }, // beq 2ee1c                  (not Apple)
    Word { addr: 0x2f528, word: 0xebffed18 }, // bl DataMgr::GetInstance
    Word { addr: 0x2f52c, word: 0xe5d0304e }, // ldrb r3, [r0, #78]
    Word { addr: 0x2f530, word: 0xe3530000 }, // cmp r3, #0
    Word { addr: 0x2f534, word: 0x0affff4c }, // beq 2ead0
    Word { addr: 0x2f53c, word: 0xebfff65b }, // bl SVPIsAppleDevice
    Word { addr: 0x2f540, word: 0xe2507000 }, // subs r7, r0, #0
    Word { addr: 0x2f544, word: 0x0a00000f }, // beq 2edec
    Word { addr: 0x2f55c, word: 0xe5940068 }, // ldr r0, [r4, #104]         the large CarPlay tile ...
    Word { addr: 0x2f560, word: 0xe3a01001 }, // mov r1, #1                 ... visible
    Word { addr: 0x2f584, word: 0xeaffff4f }, // b 2eb2c                    the function's tail
];
const IPHONETILE_0394_EDITS: &[Edit] = &[
    Edit { addr: 0x2f4e0, stock: 0xe5940058, patched: 0xe3a06000 }, // ldr r0, [r4, #88] -> mov r6, #0
    Edit { addr: 0x2f4e4, stock: 0xe3a01000, patched: 0xeaffff43 }, // mov r1, #0        -> b 2ea5c
];
const IPHONETILE2_0394_GUARDS: &[Word] = &[
    Word { addr: 0x2e848, word: 0xe5d3604d }, // ldrb r6, [r3, #77]
    Word { addr: 0x2e84c, word: 0xe3560000 }, // cmp r6, #0
    Word { addr: 0x2e850, word: 0x1a000057 }, // bne 2e218
    Word { addr: 0x2e854, word: 0xe5940058 }, // ldr r0, [r4, #88]
    Word { addr: 0x2e858, word: 0xe1a01006 }, // mov r1, r6
    Word { addr: 0x2e8cc, word: 0xebfff02f }, // bl DataMgr::GetInstance
    Word { addr: 0x2e8d0, word: 0xe5d0304c }, // ldrb r3, [r0, #76]
    Word { addr: 0x2e8d4, word: 0xe3530000 }, // cmp r3, #0
    Word { addr: 0x2e8d8, word: 0x1a0000ce }, // bne 2e47c
    Word { addr: 0x2eb80, word: 0xebfff8ca }, // bl SVPIsAppleDevice
    Word { addr: 0x2eb84, word: 0xe2506000 }, // subs r6, r0, #0
    Word { addr: 0x2eb88, word: 0x1a00004e }, // bne 2e52c                  the Apple block's entry
    Word { addr: 0x2ec18, word: 0xebffef5c }, // bl DataMgr::GetInstance
    Word { addr: 0x2ec1c, word: 0xe5d0304e }, // ldrb r3, [r0, #78]
    Word { addr: 0x2ec20, word: 0xe3530000 }, // cmp r3, #0
    Word { addr: 0x2ec24, word: 0x0affff2c }, // beq 2e140
    Word { addr: 0x2ec2c, word: 0xebfff89f }, // bl SVPIsAppleDevice
    Word { addr: 0x2ec30, word: 0xe2507000 }, // subs r7, r0, #0
    Word { addr: 0x2ec34, word: 0x1a000013 }, // bne 2e4ec
    Word { addr: 0x2ec9c, word: 0xe5940068 }, // ldr r0, [r4, #104]
    Word { addr: 0x2eca0, word: 0xe3a01001 }, // mov r1, #1
    Word { addr: 0x2ecc4, word: 0xeaffff25 }, // b 2e1c4
];
const IPHONETILE2_0394_EDITS: &[Edit] = &[
    Edit { addr: 0x2ecc8, stock: 0xe5940058, patched: 0xe3a06000 }, // ldr r0, [r4, #88] -> mov r6, #0
    Edit { addr: 0x2eccc, stock: 0xe3a01000, patched: 0xeafffee0 }, // mov r1, #0        -> b 2e0b8
];
const SEEKHOLD_0394_GUARDS: &[Word] = &[
    Word { addr: 0x48f78, word: 0xe28dd020 }, // add  sp, sp, #32          the function's return
    Word { addr: 0x48f7c, word: 0xe8bd87f0 }, // pop  {r4-sl, pc}
    Word { addr: 0x48f80, word: 0xe300330d }, // movw r3, #781
    Word { addr: 0x48f88, word: 0x0a00005c }, // beq  48d58                vkey 781: seek +
    Word { addr: 0x48f98, word: 0xe3540fc3 }, // cmp  r4, #780             vkey 780: seek -
    Word { addr: 0x48f9c, word: 0x1afffff5 }, // bne  48bd0
    Word { addr: 0x48fa0, word: 0xe3560003 }, // cmp  r6, #3               the press only (r6: the action)
    Word { addr: 0x48fa8, word: 0xe59d3010 }, // ldr  r3, [sp, #16]        the app in front
    Word { addr: 0x48fb4, word: 0xebff78a7 }, // bl   SVP_GetCarLifeService
    Word { addr: 0x48fb8, word: 0xe5903000 }, // ldr  r3, [r0]
    Word { addr: 0x48fbc, word: 0xe3a0100f }, // mov  r1, #15              CarLife's seek -
    Word { addr: 0x48fc4, word: 0xe5933064 }, // ldr  r3, [r3, #100]       RotateKeyEvent
    Word { addr: 0x48fc8, word: 0xe12fff33 }, // blx  r3
    Word { addr: 0x48fcc, word: 0xeaffffe9 }, // b    48bd0
    Word { addr: 0x49100, word: 0xe3560003 }, // cmp  r6, #3
    Word { addr: 0x49114, word: 0xebff784f }, // bl   SVP_GetCarLifeService
    Word { addr: 0x49118, word: 0xe5903000 }, // ldr  r3, [r0]
    Word { addr: 0x4911c, word: 0xe3a01010 }, // mov  r1, #16              CarLife's seek +
    Word { addr: 0x49124, word: 0xe5933064 }, // ldr  r3, [r3, #100]       RotateKeyEvent
    Word { addr: 0x49128, word: 0xe12fff33 }, // blx  r3
    Word { addr: 0x4912c, word: 0xeaffff91 }, // b    48bd0
    Word { addr: 0x4917c, word: 0xe5933078 }, // ldr  r3, [r3, #120]       GetModuleStatus(4) with appconnect in
    Word { addr: 0x49184, word: 0xe3500001 }, // cmp  r0, #1               front: no seek during a voice session
    Word { addr: 0x49188, word: 0x1affff89 }, // bne  48c0c
    Word { addr: 0x4919c, word: 0xe5933078 }, // ldr  r3, [r3, #120]
    Word { addr: 0x491a4, word: 0xe3500001 }, // cmp  r0, #1
    Word { addr: 0x491a8, word: 0x1affffd9 }, // bne  48d6c
];
const SEEKHOLD_0394_EDITS: &[Edit] = &[
    Edit { addr: 0x48fa4, stock: 0x1afffff3, patched: 0xe1a00000 }, // bne 48bd0  -> nop: every action, not only the press
    Edit { addr: 0x48fc0, stock: 0xe3a02001, patched: 0xe2862010 }, // mov r2, #1 -> add r2, r6, #16: 16 + the action
    Edit { addr: 0x49104, stock: 0x1affff9b, patched: 0xe1a00000 }, // bne 48bd0  -> nop
    Edit { addr: 0x49120, stock: 0xe3a02001, patched: 0xe2862010 }, // mov r2, #1 -> add r2, r6, #16
];
const CALLKEY_0394_GUARDS: &[Word] = &[
    Word { addr: 0x48f78, word: 0xe28dd020 }, // add  sp, sp, #32          the function's return
    Word { addr: 0x48f7c, word: 0xe8bd87f0 }, // pop  {r4-sl, pc}
    Word { addr: 0x48f58, word: 0xe3003325 }, // movw r3, #805             vkey 805: the phone key
    Word { addr: 0x48f60, word: 0x0a00002c }, // beq  48c70
    Word { addr: 0x49018, word: 0xebff788e }, // bl   SVP_GetCarLifeService
    Word { addr: 0x4901c, word: 0xe5903000 }, // ldr  r3, [r0]
    Word { addr: 0x49020, word: 0xe5933070 }, // ldr  r3, [r3, #112]       NeedBlockBTPopup: 1 while AA is on screen
    Word { addr: 0x49024, word: 0xe12fff33 }, // blx  r3
    Word { addr: 0x49028, word: 0xe3500000 }, // cmp  r0, #0
    Word { addr: 0x4902c, word: 0x0affffd1 }, // beq  48bd0
    Word { addr: 0x49030, word: 0xe1a0000a }, // mov  r0, sl
    Word { addr: 0x49034, word: 0xe1a01006 }, // mov  r1, r6               the action (r6 is kept across calls)
    Word { addr: 0x49038, word: 0xebfffe22 }, // bl   MFLPhoneFunction     answers / ends a call: unchanged
    Word { addr: 0x49040, word: 0xe3560004 }, // cmp  r6, #4               vkey 804 (voice), its release ...
    Word { addr: 0x4905c, word: 0xe3500000 }, // cmp  r0, #0               ... during a voice session:
    Word { addr: 0x491cc, word: 0xe08f0000 }, // add  r0, pc, r0           (the rest of the log line, now unreached)
    Word { addr: 0x491d0, word: 0xebff77f3 }, // bl   printf
    Word { addr: 0x491d4, word: 0xebff781f }, // bl   SVP_GetCarLifeService   ... VR stop
    Word { addr: 0x491d8, word: 0xe5903000 }, // ldr  r3, [r0]
    Word { addr: 0x491dc, word: 0xe3a01022 }, // mov  r1, #34
    Word { addr: 0x491e0, word: 0xe3a02001 }, // mov  r2, #1
    Word { addr: 0x491e4, word: 0xe5933064 }, // ldr  r3, [r3, #100]       RotateKeyEvent
    Word { addr: 0x491e8, word: 0xe12fff33 }, // blx  r3
    Word { addr: 0x491ec, word: 0xeaffff61 }, // b    48bd0
];
const CALLKEY_0394_EDITS: &[Edit] = &[
    Edit { addr: 0x491b0, stock: 0xebff790a, patched: 0xebff7828 }, // bl getpid        -> bl SVP_GetCarLifeService
    Edit { addr: 0x491b4, stock: 0xe1a04000, patched: 0xe5903000 }, // mov r4, r0       -> ldr r3, [r0]
    Edit { addr: 0x491b8, stock: 0xebff7878, patched: 0xe3a01002 }, // bl GetCurrentThreadID -> mov r1, #2 (CarLife's phone key)
    Edit { addr: 0x491bc, stock: 0xe1a02000, patched: 0xe2862010 }, // mov r2, r0       -> add r2, r6, #16: 16 + the action
    Edit { addr: 0x491c0, stock: 0xe59f00c4, patched: 0xe5933064 }, // ldr r0, [pc, #196] -> ldr r3, [r3, #100] (RotateKeyEvent)
    Edit { addr: 0x491c4, stock: 0xe1a01004, patched: 0xe12fff33 }, // mov r1, r4       -> blx r3
    Edit { addr: 0x491c8, stock: 0xe1a03007, patched: 0xeaffff6a }, // mov r3, r7       -> b 48bd0
    Edit { addr: 0x49060, stock: 0x1a000052, patched: 0x1a00005b }, // bne 48e08        -> bne 48e2c: VR stop without its log line
    Edit { addr: 0x4903c, stock: 0xeaffffcd, patched: 0xea00005b }, // b 48bd0          -> b 48e08: after MFLPhoneFunction
];
const CALLFIX_0317_GUARDS: &[Word] = &[
    Word { addr: 0x33aa8, word: 0xebffcdb8 }, // bl   SVP_GetCarLifeService
    Word { addr: 0x33aac, word: 0xe5903000 }, // ldr  r3, [r0]
    Word { addr: 0x33ab0, word: 0xe5933068 }, // ldr  r3, [r3, #104]      GetCarLifeConnectStatus
    Word { addr: 0x33ab4, word: 0xe12fff33 }, // blx  r3
    Word { addr: 0x33ab8, word: 0xe3500003 }, // cmp  r0, #3
    Word { addr: 0x33ac0, word: 0xe1a00009 }, // mov  r0, r9
    Word { addr: 0x33ac4, word: 0xebffff8f }, // bl   CAppconnectDataMgr::APPConnectIsFG
    Word { addr: 0x33ac8, word: 0xe3500000 }, // cmp  r0, #0
    Word { addr: 0x33acc, word: 0x0affffd2 }, // beq  33630
    Word { addr: 0x33ad8, word: 0xebffcd70 }, // bl   sleep
    Word { addr: 0x33b08, word: 0xe3a00042 }, // mov  r0, #66             the Bluetooth app
    Word { addr: 0x33b0c, word: 0xe28d1010 }, // add  r1, sp, #16         "bt_carlife_temp_call"
    Word { addr: 0x33b10, word: 0xe3a0200c }, // mov  r2, #12
    Word { addr: 0x33b14, word: 0xe3a03001 }, // mov  r3, #1
    Word { addr: 0x33b18, word: 0xebffcd9f }, // bl   SVP_StartApp
];
const CALLFIX_0317_EDITS: &[Edit] = &[
    Edit { addr: 0x33abc, stock: 0x1affffd6, patched: 0xeaffffd6 }, // bne 33630 -> b 33630
];
const IPHONE_0317_GUARDS: &[Word] = &[
    Word { addr: 0x362f0, word: 0xe3550001 }, // cmp   r5, #1               type 1 ...
    Word { addr: 0x362f4, word: 0x13550004 }, // cmpne r5, #4               ... or 4 (Apple)
    Word { addr: 0x362fc, word: 0xe3760001 }, // cmn   r6, #1               no stored choice
    Word { addr: 0x36300, word: 0x0a000041 }, // beq   36020                -> the pop-up path
    Word { addr: 0x36304, word: 0xe28dd014 }, // add   sp, sp, #20          the normal return
    Word { addr: 0x36448, word: 0xebffc323 }, // bl    printf               its log line
    Word { addr: 0x3644c, word: 0xe3a00001 }, // mov   r0, #1
    Word { addr: 0x36450, word: 0xebffc312 }, // bl    sleep
    Word { addr: 0x36464, word: 0xe1a0200c }, // mov   r2, ip               (rest of the post, now unreached)
    Word { addr: 0x36478, word: 0xe12fff3c }, // blx   ip
    Word { addr: 0x31c40, word: 0xe92d4070 }, // SVPSingleton<AppconncetPopUpMgr>::GetInstance: push {r4-r6, lr}
    Word { addr: 0x3813c, word: 0xe92d4ff0 }, // AppconncetPopUpMgr::UserSelectEstablishSession: push {r4-fp, lr}
    Word { addr: 0x381f4, word: 0xe3540002 }, // ... cmp r4, #2             2 = CarLife (its own branch)
    Word { addr: 0x381b0, word: 0xe3a07001 }, // ... mov r7, #1
    Word { addr: 0x38208, word: 0xe1540007 }, // ... cmp r4, r7             r7 = 1: CarPlay
    Word { addr: 0x3820c, word: 0x0a000018 }, // ... beq 37e88              the CarPlay branch
];
const IPHONE_0317_EDITS: &[Edit] = &[
    Edit { addr: 0x36454, stock: 0xebffc4d7, patched: 0xebffedf9 }, // bl GetWS     -> bl GetInstance (0x31864)
    Edit { addr: 0x36458, stock: 0xe590e000, patched: 0xe3a01001 }, // ldr lr, [r0] -> mov r1, #1 (CarPlay)
    Edit { addr: 0x3645c, stock: 0xe3a0c000, patched: 0xeb000736 }, // mov ip, #0   -> bl UserSelectEstablishSession (0x37d50)
    Edit { addr: 0x36460, stock: 0xe3a01006, patched: 0xeaffffa7 }, // mov r1, #6   -> b 35f18
];
const IPHONETILE_0317_GUARDS: &[Word] = &[
    Word { addr: 0x2f0d0, word: 0xe5d3604d }, // ldrb r6, [r3, #77]         MirrorLink coded? (the other branch
    Word { addr: 0x2f0d4, word: 0xe3560000 }, // cmp r6, #0                 when not)
    Word { addr: 0x2f0d8, word: 0x1a000048 }, // bne 2eb80
    Word { addr: 0x2f0dc, word: 0xe5940058 }, // ldr r0, [r4, #88]          the other branch: hide the small tiles
    Word { addr: 0x2f0e0, word: 0xe1a01006 }, // mov r1, r6                 ... with r6
    Word { addr: 0x2f140, word: 0xebffedcb }, // bl DataMgr::GetInstance
    Word { addr: 0x2f144, word: 0xe5d0304c }, // ldrb r3, [r0, #76]
    Word { addr: 0x2f148, word: 0xe3530000 }, // cmp r3, #0
    Word { addr: 0x2f14c, word: 0x1a0000ae }, // bne 2ed8c
    Word { addr: 0x2f3b8, word: 0xebfff675 }, // bl SVPIsAppleDevice        the Apple block's entry
    Word { addr: 0x2f3bc, word: 0xe2506000 }, // subs r6, r0, #0
    Word { addr: 0x2f3c0, word: 0x0a000035 }, // beq 2ee1c                  (not Apple)
    Word { addr: 0x2f40c, word: 0xebffed18 }, // bl DataMgr::GetInstance
    Word { addr: 0x2f410, word: 0xe5d0304e }, // ldrb r3, [r0, #78]
    Word { addr: 0x2f414, word: 0xe3530000 }, // cmp r3, #0
    Word { addr: 0x2f418, word: 0x0affff4c }, // beq 2ead0
    Word { addr: 0x2f420, word: 0xebfff65b }, // bl SVPIsAppleDevice
    Word { addr: 0x2f424, word: 0xe2507000 }, // subs r7, r0, #0
    Word { addr: 0x2f428, word: 0x0a00000f }, // beq 2edec
    Word { addr: 0x2f440, word: 0xe5940068 }, // ldr r0, [r4, #104]         the large CarPlay tile ...
    Word { addr: 0x2f444, word: 0xe3a01001 }, // mov r1, #1                 ... visible
    Word { addr: 0x2f468, word: 0xeaffff4f }, // b 2eb2c                    the function's tail
];
const IPHONETILE_0317_EDITS: &[Edit] = &[
    Edit { addr: 0x2f3c4, stock: 0xe5940058, patched: 0xe3a06000 }, // ldr r0, [r4, #88] -> mov r6, #0
    Edit { addr: 0x2f3c8, stock: 0xe3a01000, patched: 0xeaffff43 }, // mov r1, #0        -> b 2ea5c
];
const IPHONETILE2_0317_GUARDS: &[Word] = &[
    Word { addr: 0x2e72c, word: 0xe5d3604d }, // ldrb r6, [r3, #77]
    Word { addr: 0x2e730, word: 0xe3560000 }, // cmp r6, #0
    Word { addr: 0x2e734, word: 0x1a000057 }, // bne 2e218
    Word { addr: 0x2e738, word: 0xe5940058 }, // ldr r0, [r4, #88]
    Word { addr: 0x2e73c, word: 0xe1a01006 }, // mov r1, r6
    Word { addr: 0x2e7b0, word: 0xebfff02f }, // bl DataMgr::GetInstance
    Word { addr: 0x2e7b4, word: 0xe5d0304c }, // ldrb r3, [r0, #76]
    Word { addr: 0x2e7b8, word: 0xe3530000 }, // cmp r3, #0
    Word { addr: 0x2e7bc, word: 0x1a0000ce }, // bne 2e47c
    Word { addr: 0x2ea64, word: 0xebfff8ca }, // bl SVPIsAppleDevice
    Word { addr: 0x2ea68, word: 0xe2506000 }, // subs r6, r0, #0
    Word { addr: 0x2ea6c, word: 0x1a00004e }, // bne 2e52c                  the Apple block's entry
    Word { addr: 0x2eafc, word: 0xebffef5c }, // bl DataMgr::GetInstance
    Word { addr: 0x2eb00, word: 0xe5d0304e }, // ldrb r3, [r0, #78]
    Word { addr: 0x2eb04, word: 0xe3530000 }, // cmp r3, #0
    Word { addr: 0x2eb08, word: 0x0affff2c }, // beq 2e140
    Word { addr: 0x2eb10, word: 0xebfff89f }, // bl SVPIsAppleDevice
    Word { addr: 0x2eb14, word: 0xe2507000 }, // subs r7, r0, #0
    Word { addr: 0x2eb18, word: 0x1a000013 }, // bne 2e4ec
    Word { addr: 0x2eb80, word: 0xe5940068 }, // ldr r0, [r4, #104]
    Word { addr: 0x2eb84, word: 0xe3a01001 }, // mov r1, #1
    Word { addr: 0x2eba8, word: 0xeaffff25 }, // b 2e1c4
];
const IPHONETILE2_0317_EDITS: &[Edit] = &[
    Edit { addr: 0x2ebac, stock: 0xe5940058, patched: 0xe3a06000 }, // ldr r0, [r4, #88] -> mov r6, #0
    Edit { addr: 0x2ebb0, stock: 0xe3a01000, patched: 0xeafffee0 }, // mov r1, #0        -> b 2e0b8
];
const SEEKHOLD_0317_GUARDS: &[Word] = &[
    Word { addr: 0x48fbc, word: 0xe28dd020 }, // add  sp, sp, #32          the function's return
    Word { addr: 0x48fc0, word: 0xe8bd87f0 }, // pop  {r4-sl, pc}
    Word { addr: 0x48fc4, word: 0xe300330d }, // movw r3, #781
    Word { addr: 0x48fcc, word: 0x0a00005c }, // beq  48d58                vkey 781: seek +
    Word { addr: 0x48fdc, word: 0xe3540fc3 }, // cmp  r4, #780             vkey 780: seek -
    Word { addr: 0x48fe0, word: 0x1afffff5 }, // bne  48bd0
    Word { addr: 0x48fe4, word: 0xe3560003 }, // cmp  r6, #3               the press only (r6: the action)
    Word { addr: 0x48fec, word: 0xe59d3010 }, // ldr  r3, [sp, #16]        the app in front
    Word { addr: 0x48ff8, word: 0xebff7864 }, // bl   SVP_GetCarLifeService
    Word { addr: 0x48ffc, word: 0xe5903000 }, // ldr  r3, [r0]
    Word { addr: 0x49000, word: 0xe3a0100f }, // mov  r1, #15              CarLife's seek -
    Word { addr: 0x49008, word: 0xe5933064 }, // ldr  r3, [r3, #100]       RotateKeyEvent
    Word { addr: 0x4900c, word: 0xe12fff33 }, // blx  r3
    Word { addr: 0x49010, word: 0xeaffffe9 }, // b    48bd0
    Word { addr: 0x49144, word: 0xe3560003 }, // cmp  r6, #3
    Word { addr: 0x49158, word: 0xebff780c }, // bl   SVP_GetCarLifeService
    Word { addr: 0x4915c, word: 0xe5903000 }, // ldr  r3, [r0]
    Word { addr: 0x49160, word: 0xe3a01010 }, // mov  r1, #16              CarLife's seek +
    Word { addr: 0x49168, word: 0xe5933064 }, // ldr  r3, [r3, #100]       RotateKeyEvent
    Word { addr: 0x4916c, word: 0xe12fff33 }, // blx  r3
    Word { addr: 0x49170, word: 0xeaffff91 }, // b    48bd0
    Word { addr: 0x491c0, word: 0xe5933078 }, // ldr  r3, [r3, #120]       GetModuleStatus(4) with appconnect in
    Word { addr: 0x491c8, word: 0xe3500001 }, // cmp  r0, #1               front: no seek during a voice session
    Word { addr: 0x491cc, word: 0x1affff89 }, // bne  48c0c
    Word { addr: 0x491e0, word: 0xe5933078 }, // ldr  r3, [r3, #120]
    Word { addr: 0x491e8, word: 0xe3500001 }, // cmp  r0, #1
    Word { addr: 0x491ec, word: 0x1affffd9 }, // bne  48d6c
];
const SEEKHOLD_0317_EDITS: &[Edit] = &[
    Edit { addr: 0x48fe8, stock: 0x1afffff3, patched: 0xe1a00000 }, // bne 48bd0  -> nop: every action, not only the press
    Edit { addr: 0x49004, stock: 0xe3a02001, patched: 0xe2862010 }, // mov r2, #1 -> add r2, r6, #16: 16 + the action
    Edit { addr: 0x49148, stock: 0x1affff9b, patched: 0xe1a00000 }, // bne 48bd0  -> nop
    Edit { addr: 0x49164, stock: 0xe3a02001, patched: 0xe2862010 }, // mov r2, #1 -> add r2, r6, #16
];
const CALLKEY_0317_GUARDS: &[Word] = &[
    Word { addr: 0x48fbc, word: 0xe28dd020 }, // add  sp, sp, #32          the function's return
    Word { addr: 0x48fc0, word: 0xe8bd87f0 }, // pop  {r4-sl, pc}
    Word { addr: 0x48f9c, word: 0xe3003325 }, // movw r3, #805             vkey 805: the phone key
    Word { addr: 0x48fa4, word: 0x0a00002c }, // beq  48c70
    Word { addr: 0x4905c, word: 0xebff784b }, // bl   SVP_GetCarLifeService
    Word { addr: 0x49060, word: 0xe5903000 }, // ldr  r3, [r0]
    Word { addr: 0x49064, word: 0xe5933070 }, // ldr  r3, [r3, #112]       NeedBlockBTPopup: 1 while AA is on screen
    Word { addr: 0x49068, word: 0xe12fff33 }, // blx  r3
    Word { addr: 0x4906c, word: 0xe3500000 }, // cmp  r0, #0
    Word { addr: 0x49070, word: 0x0affffd1 }, // beq  48bd0
    Word { addr: 0x49074, word: 0xe1a0000a }, // mov  r0, sl
    Word { addr: 0x49078, word: 0xe1a01006 }, // mov  r1, r6               the action (r6 is kept across calls)
    Word { addr: 0x4907c, word: 0xebfffdca }, // bl   MFLPhoneFunction     answers / ends a call: unchanged
    Word { addr: 0x49084, word: 0xe3560004 }, // cmp  r6, #4               vkey 804 (voice), its release ...
    Word { addr: 0x490a0, word: 0xe3500000 }, // cmp  r0, #0               ... during a voice session:
    Word { addr: 0x49210, word: 0xe08f0000 }, // add  r0, pc, r0           (the rest of the log line, now unreached)
    Word { addr: 0x49214, word: 0xebff77b0 }, // bl   printf
    Word { addr: 0x49218, word: 0xebff77dc }, // bl   SVP_GetCarLifeService   ... VR stop
    Word { addr: 0x4921c, word: 0xe5903000 }, // ldr  r3, [r0]
    Word { addr: 0x49220, word: 0xe3a01022 }, // mov  r1, #34
    Word { addr: 0x49224, word: 0xe3a02001 }, // mov  r2, #1
    Word { addr: 0x49228, word: 0xe5933064 }, // ldr  r3, [r3, #100]       RotateKeyEvent
    Word { addr: 0x4922c, word: 0xe12fff33 }, // blx  r3
    Word { addr: 0x49230, word: 0xeaffff61 }, // b    48bd0
];
const CALLKEY_0317_EDITS: &[Edit] = &[
    Edit { addr: 0x491f4, stock: 0xebff78c7, patched: 0xebff77e5 }, // bl getpid        -> bl SVP_GetCarLifeService
    Edit { addr: 0x491f8, stock: 0xe1a04000, patched: 0xe5903000 }, // mov r4, r0       -> ldr r3, [r0]
    Edit { addr: 0x491fc, stock: 0xebff7835, patched: 0xe3a01002 }, // bl GetCurrentThreadID -> mov r1, #2 (CarLife's phone key)
    Edit { addr: 0x49200, stock: 0xe1a02000, patched: 0xe2862010 }, // mov r2, r0       -> add r2, r6, #16: 16 + the action
    Edit { addr: 0x49204, stock: 0xe59f00c4, patched: 0xe5933064 }, // ldr r0, [pc, #196] -> ldr r3, [r3, #100] (RotateKeyEvent)
    Edit { addr: 0x49208, stock: 0xe1a01004, patched: 0xe12fff33 }, // mov r1, r4       -> blx r3
    Edit { addr: 0x4920c, stock: 0xe1a03007, patched: 0xeaffff6a }, // mov r3, r7       -> b 48bd0
    Edit { addr: 0x490a4, stock: 0x1a000052, patched: 0x1a00005b }, // bne 48e08        -> bne 48e2c: VR stop without its log line
    Edit { addr: 0x49080, stock: 0xeaffffcd, patched: 0xea00005b }, // b 48bd0          -> b 48e08: after MFLPhoneFunction
];

const CALLFIX_WHAT: &str = "outgoing calls no longer open the phone screen over Android Auto";
const IPHONE_WHAT: &str = "an iPhone goes straight to CarPlay (no CarPlay-or-Android-Auto question)";
const IPHONETILE_WHAT: &str = "an iPhone sees the CarPlay tile alone (connect screen)";
const IPHONETILE2_WHAT: &str = "an iPhone sees the CarPlay tile alone (connect screen updates)";
const SEEKHOLD_WHAT: &str = "the seek keys tell Android Auto press, hold and release (holding one seeks)";
const CALLKEY_WHAT: &str = "the phone key also reaches Android Auto (with no call it opens AA's phone screen)";

pub const PATCHES: &[Patch] = &[
    Patch { name: "callfix", build: "0358", what: CALLFIX_WHAT, guards: CALLFIX_GUARDS, edits: CALLFIX_EDITS },
    Patch { name: "callfix", build: "0394", what: CALLFIX_WHAT, guards: CALLFIX_0394_GUARDS, edits: CALLFIX_0394_EDITS },
    Patch { name: "callfix", build: "0317", what: CALLFIX_WHAT, guards: CALLFIX_0317_GUARDS, edits: CALLFIX_0317_EDITS },
    Patch { name: "iphone", build: "0358", what: IPHONE_WHAT, guards: IPHONE_GUARDS, edits: IPHONE_EDITS },
    Patch { name: "iphone", build: "0394", what: IPHONE_WHAT, guards: IPHONE_0394_GUARDS, edits: IPHONE_0394_EDITS },
    Patch { name: "iphone", build: "0317", what: IPHONE_WHAT, guards: IPHONE_0317_GUARDS, edits: IPHONE_0317_EDITS },
    Patch { name: "iphonetile", build: "0358", what: IPHONETILE_WHAT, guards: IPHONETILE_GUARDS, edits: IPHONETILE_EDITS },
    Patch { name: "iphonetile", build: "0394", what: IPHONETILE_WHAT, guards: IPHONETILE_0394_GUARDS, edits: IPHONETILE_0394_EDITS },
    Patch { name: "iphonetile", build: "0317", what: IPHONETILE_WHAT, guards: IPHONETILE_0317_GUARDS, edits: IPHONETILE_0317_EDITS },
    Patch { name: "iphonetile2", build: "0358", what: IPHONETILE2_WHAT, guards: IPHONETILE2_GUARDS, edits: IPHONETILE2_EDITS },
    Patch { name: "iphonetile2", build: "0394", what: IPHONETILE2_WHAT, guards: IPHONETILE2_0394_GUARDS, edits: IPHONETILE2_0394_EDITS },
    Patch { name: "iphonetile2", build: "0317", what: IPHONETILE2_WHAT, guards: IPHONETILE2_0317_GUARDS, edits: IPHONETILE2_0317_EDITS },
    Patch { name: "seekhold", build: "0358", what: SEEKHOLD_WHAT, guards: SEEKHOLD_GUARDS, edits: SEEKHOLD_EDITS },
    Patch { name: "seekhold", build: "0394", what: SEEKHOLD_WHAT, guards: SEEKHOLD_0394_GUARDS, edits: SEEKHOLD_0394_EDITS },
    Patch { name: "seekhold", build: "0317", what: SEEKHOLD_WHAT, guards: SEEKHOLD_0317_GUARDS, edits: SEEKHOLD_0317_EDITS },
    Patch { name: "callkey", build: "0358", what: CALLKEY_WHAT, guards: CALLKEY_GUARDS, edits: CALLKEY_EDITS },
    Patch { name: "callkey", build: "0394", what: CALLKEY_WHAT, guards: CALLKEY_0394_GUARDS, edits: CALLKEY_0394_EDITS },
    Patch { name: "callkey", build: "0317", what: CALLKEY_WHAT, guards: CALLKEY_0317_GUARDS, edits: CALLKEY_0317_EDITS },
];
