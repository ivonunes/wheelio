// A tiny stand-in for steamclient.dylib with the same shape around the gates.
// Built once per FIXTURE_* variant to exercise each kind of miss.

#define FLAG 0x810

        .section __TEXT,__cstring,cstring_literals
config_string:  .asciz "CCompatManager::OnAppConfigInitialized"
logon_string:   .asciz "CCompatManager::OnPostLogonState"
#ifndef FIXTURE_STRING_MISSING
disable_string: .asciz "Disabling compatibility layer."
#endif

        .text
        .p2align 2

// Callbacks: test the flag first thing, then use their string.
        .private_extern _app_config_handler
_app_config_handler:
        sub     sp, sp, #0x20
        stp     x29, x30, [sp, #0x10]
        add     x29, sp, #0x10
#ifdef FIXTURE_NO_FLAG_TEST
        bl      _helper
#endif
        ldrb    w8, [x0, #FLAG]
        cmp     w8, #1
        b.ne    1f
        adrp    x8, config_string@PAGE
        add     x8, x8, config_string@PAGEOFF
1:      ldp     x29, x30, [sp, #0x10]
        add     sp, sp, #0x20
        ret

        .private_extern _post_logon_handler
_post_logon_handler:
        stp     x29, x30, [sp, #-0x10]!
        mov     x29, sp
#ifdef FIXTURE_FLAG_MISMATCH
        ldrb    w9, [x0, #(FLAG + 1)]
#else
        ldrb    w9, [x0, #FLAG]
#endif
        cmp     w9, #1
        b.ne    1f
        adrp    x9, logon_string@PAGE
        add     x9, x9, logon_string@PAGEOFF
1:      ldp     x29, x30, [sp], #0x10
        ret

// The constructor registers the callbacks and sets the flag from a test.
        .globl  _fake_compat_manager_construct
_fake_compat_manager_construct:
#ifdef FIXTURE_ENTRY_PC_RELATIVE
        adrp    x9, config_string@PAGE
#endif
        sub     sp, sp, #0x20
        stp     x20, x19, [sp]
        stp     x29, x30, [sp, #0x10]
        add     x29, sp, #0x10
        mov     x19, x0
        adrp    x8, _app_config_handler@PAGE
        add     x8, x8, _app_config_handler@PAGEOFF
        str     x8, [x19, #8]
        adrp    x8, _post_logon_handler@PAGE
        add     x8, x8, _post_logon_handler@PAGEOFF
        str     x8, [x19, #16]
        cmp     x19, #0
        cset    w8, eq
        strb    w8, [x19, #FLAG]
        mov     x0, x19
        ldp     x29, x30, [sp, #0x10]
        ldp     x20, x19, [sp]
        add     sp, sp, #0x20
        ret

        .globl  _fake_compat_manager_disable
_fake_compat_manager_disable:
#if !defined(FIXTURE_STRING_MISSING) && !defined(FIXTURE_NOT_REFERENCED)
        adrp    x8, disable_string@PAGE
        add     x8, x8, disable_string@PAGEOFF
#endif
#ifdef FIXTURE_DISABLE_WRONG
        strb    wzr, [x0, #(FLAG + 2)]
#else
        strb    wzr, [x0, #FLAG]
#endif
        ret

        .private_extern _helper
_helper:
#ifdef FIXTURE_STRING_TWICE
        adrp    x8, logon_string@PAGE
        add     x8, x8, logon_string@PAGEOFF
#endif
#ifdef FIXTURE_CONSTRUCTOR_TWICE
        adrp    x8, _app_config_handler@PAGE
        add     x8, x8, _app_config_handler@PAGEOFF
        adrp    x8, _post_logon_handler@PAGE
        add     x8, x8, _post_logon_handler@PAGEOFF
#endif
        ret

.subsections_via_symbols
