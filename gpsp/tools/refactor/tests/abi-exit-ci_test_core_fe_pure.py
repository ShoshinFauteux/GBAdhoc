#!/usr/bin/env python3
"""abi-exit-ci_test_core_fe_pure.py -- self-test and mutants of ci_greps R8 core_fe_pure
(tier C, C6-1; builds/health-out/design/C-6.md).  No compiler, no tree edits: every case is a
text given to ci_greps.r8_text_hits, plus the rule run on a scratch tree (absent file = no-op,
the tree's own core_fe.h when it has one).

  C1  a clean header (guard, __cplusplus, extern decls, prototypes, constants, typedef struct) -> 0
  C2  the tree's own core_fe.h, if present                                                  -> 0
  C3  a scratch tree without core_fe.h                                                      -> no-op, 0
  M1  #ifdef HAVE_DYNAREC (the core-vs-frontend-defines trap)                               -> FAIL
  M2  #if defined(PSP) / #else                                                             -> FAIL
  M3  a declaration with __attribute__((section(".sbss")))                                 -> FAIL
  M4  a declaration with __attribute__((weak))                                             -> FAIL
  M5  a static variable                                                                    -> FAIL
  M6  an initialised variable (a definition)                                               -> FAIL
  M7  a function body (static inline helper)                                               -> FAIL
  M8  no include guard                                                                     -> FAIL
  M9  #include of a core header                                                            -> FAIL
Exit 0 PASS, 1 FAILED.
"""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import ci_greps as G  # noqa: E402

GOOD = '''/* core_fe.h -- a test */
#ifndef CORE_FE_H
#define CORE_FE_H
#ifdef __cplusplus
extern "C" {
#endif
#define VRAM_DIRTY_SHIFT 10
#define VRAM_DIRTY_PAGES ((1024 * 96) >> VRAM_DIRTY_SHIFT)
struct RFILE;
typedef struct { u32 owner[8]; u32 bytes; } core_fe_stats_t;
extern u32 flush_rom_total;           /* "static" in a comment is fine */
extern struct RFILE *gamepak_file_large;
extern void (*gpsp_rom_load_progress)(u32 loaded, u32 total);
void dynarec_release_translation_caches(void);
extern const char *jit_cache_reason;   /* = "x" in a comment */
#if defined(__cplusplus)
}
#endif
#endif
'''


def main():
    bad = [0]

    def t(cond, msg):
        print('  %s  %s' % ('ok  ' if cond else 'FAIL', msg))
        bad[0] += 0 if cond else 1

    def hits(text):
        return G.r8_text_hits(text)
    h = hits(GOOD)
    t(not h, 'C1 a clean header: %d hits%s' % (len(h), ''.join('; %s:%d %s' % (x.file, x.line, x.msg) for x in h)))
    src = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
    h, present = G.rule_r8(src)
    t(not h, 'C2 the tree\'s core_fe.h: %s, %d hits%s' % ('present' if present else 'absent', len(h),
                                                         ''.join('; %s:%d %s' % (x.file, x.line, x.msg) for x in h)))
    d = tempfile.mkdtemp(prefix='r8_test.')
    try:
        h, present = G.rule_r8(d)
        t(not present and not h, 'C3 no core_fe.h: no-op (%d hits)' % len(h))
        with open(os.path.join(d, 'core_fe.h'), 'w', newline='\n') as f:
            f.write(GOOD.replace('#define VRAM_DIRTY_SHIFT 10', '#ifdef HAVE_DYNAREC\n#define VRAM_DIRTY_SHIFT 10\n#endif'))
        h, present = G.rule_r8(d)
        t(present and len(h) == 1 and h[0].key.startswith('#ifdef HAVE_DYNAREC'),
          'M1 (in a scratch tree) #ifdef HAVE_DYNAREC -> %s' % '; '.join('%s:%d %s' % (x.file, x.line, x.msg) for x in h))
    finally:
        shutil.rmtree(d, ignore_errors=True)
    muts = [
        ('M2 #if defined(PSP) / #else', GOOD.replace('struct RFILE;', '#if defined(PSP)\nstruct RFILE;\n#else\nstruct RFILE;\n#endif'), '#if'),
        ('M3 section(".sbss")', GOOD.replace('extern u32 flush_rom_total;', 'extern u32 flush_rom_total __attribute__((section(".sbss")));'), 'section'),
        ('M4 weak', GOOD.replace('void dynarec_release_translation_caches(void);',
                                 'void dynarec_release_translation_caches(void) __attribute__((weak));'), 'weak'),
        ('M5 static', GOOD.replace('extern u32 flush_rom_total;', 'static u32 flush_rom_total;'), 'static'),
        ('M6 an initialiser', GOOD.replace('extern u32 flush_rom_total;', 'u32 flush_rom_total = 0;'), '='),
        ('M7 a function body', GOOD.replace('void dynarec_release_translation_caches(void);',
                                            'void dynarec_release_translation_caches(void) { }'), '{'),
        ('M8 no guard', GOOD.replace('#ifndef CORE_FE_H\n#define CORE_FE_H\n', '').rsplit('#endif', 1)[0], 'guard'),
        ('M9 #include', GOOD.replace('struct RFILE;', '#include "gba_memory.h"\nstruct RFILE;'), '#include'),
    ]
    for label, text, key in muts:
        h = hits(text)
        t(any(x.key.startswith(key) for x in h), '%s -> %d hit(s): %s' % (
            label, len(h), '; '.join('%d %s' % (x.line, x.msg) for x in h)[:150]))
    print('core_fe_pure selftest: %s' % ('PASS' if not bad[0] else '%d FAILED' % bad[0]))
    return 1 if bad[0] else 0


if __name__ == '__main__':
    sys.exit(main())
