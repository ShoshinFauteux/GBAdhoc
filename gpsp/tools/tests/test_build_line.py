#!/usr/bin/env python3
"""Settings' read-only build line (GitHub issue #31; docs/UI-OVERLAY.md 15.4).

psp/ui_psp.c's build_line_value() and build_line(), as written, against a
stubbed sceIo that serves a VERSION.txt exactly as tools/make_release.sh
writes it (the heredoc there is read from the script, not restated):

  1. A release's file: "GBAdhoc 4.6.0-beta.4 (abc1234)", and a dirty
     build's commit ("abc1234-dirty") whole.
  2. CRLF line ends, extra spaces, keys in another order: the same.
  3. No file, an empty file, no version line: "development build".
  4. Only a version: "GBAdhoc X" with no empty brackets.
  5. It is read once: the second call opens nothing.

Then mutants, each of which must fail.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
UI = (ROOT / "psp/ui_psp.c").read_text(encoding="utf-8")
REL = (ROOT / "tools/make_release.sh").read_text(encoding="utf-8")

TOKENS = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|'
                    r"'(?:\\.|[^'\\])*'|[{}]", re.S)


def braced(source, start):
    depth = 0
    for token in TOKENS.finditer(source, start):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:token.end()]
    raise AssertionError("unclosed block")


def static_fn(source, signature):
    i = source.find(signature + "\n{")
    assert i >= 0, signature + " not found"
    return braced(source, i)


VALUE = ("static void build_line_value(const char *buf, const char *key, "
         "char *out,\n                             size_t cap)")
LINE = "static const char *build_line(void)"


def production(src):
    return "\n".join(("static char g_build_line[64];",
                      static_fn(src, VALUE), static_fn(src, LINE)))


# make_release.sh's VERSION.txt, with its variables filled in.
m = re.search(r'cat > "\$STAGE/VERSION.txt" <<EOF\n(.*?)\nEOF\n', REL, re.S)
assert m, "make_release.sh no longer writes VERSION.txt this way"
RELEASE_FILE = (m.group(1).replace("$VERSION", "4.6.0-beta.4")
                .replace("${COMMIT}${DIRTY}", "abc1234")
                .replace("$BUILT", "2026-11-05 07:00:00 UTC"))
C_FILE = ''.join('"%s\\n"\n' % l.replace('"', '\\"')
                 for l in RELEASE_FILE.split("\n"))

STUB = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { \
   fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef int SceUID;
typedef unsigned SceSize;
#define PSP_O_RDONLY 1
char g_dir_base[128] = "ms0:/PSP/GAME/GBAdhoc";
static const char *file;          /* NULL = no VERSION.txt */
static int opens;
static char opened[200];
static SceUID sceIoOpen(const char *p, int f, int m)
{ (void)f; (void)m; opens++; snprintf(opened, sizeof(opened), "%s", p);
  return file ? 3 : -1; }
static int sceIoRead(SceUID fd, void *b, SceSize n)
{ size_t l = strlen(file); (void)fd; if (l > n) l = n; memcpy(b, file, l);
  return (int)l; }
static int sceIoClose(SceUID fd) { (void)fd; return 0; }

@FUNCTIONS@

static const char *fresh(const char *f)
{
   file = f;
   opens = 0;
   g_build_line[0] = '\0';
   return build_line();
}

int main(void)
{
   static const char rel[] = @RELEASE@;
   CHECK(!strcmp(fresh(rel), "GBAdhoc 4.6.0-beta.4 (abc1234)"));
   CHECK(!strcmp(opened, "ms0:/PSP/GAME/GBAdhoc/VERSION.txt"));
   CHECK(!strcmp(fresh("version : 4.6.0-beta.4\ncommit  : abc1234-dirty\n"),
                 "GBAdhoc 4.6.0-beta.4 (abc1234-dirty)"));
   CHECK(!strcmp(fresh("GBAdhoc\r\ncommit  : 1234567 \r\nversion :  4.6.0  \r\n"),
                 "GBAdhoc 4.6.0 (1234567)"));
   CHECK(!strcmp(fresh(NULL), "development build"));
   CHECK(!strcmp(fresh(""), "development build"));
   CHECK(!strcmp(fresh("GBAdhoc\nlicense : GPL\n"), "development build"));
   CHECK(!strcmp(fresh("version : 4.6.0\n"), "GBAdhoc 4.6.0"));
   CHECK(!strcmp(fresh("versionX : 9\nversion : 1.2.3\n"), "GBAdhoc 1.2.3"));
   fresh(rel);
   CHECK(opens == 1 && !strcmp(build_line(), "GBAdhoc 4.6.0-beta.4 (abc1234)"));
   CHECK(opens == 1);
   puts("build line: VERSION.txt's version and commit, read once; "
        "\"development build\" without it");
   return 0;
}
'''

CC = ["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
      "-Wno-unused-function", "-Wno-format-truncation"]


def run(ui, expect_pass):
    code = (STUB.replace("@FUNCTIONS@", production(ui))
            .replace("@RELEASE@", C_FILE))
    with tempfile.TemporaryDirectory(prefix="gbadhoc-build-") as folder:
        c = Path(folder) / "t.c"
        b = Path(folder) / "t"
        c.write_text(code)
        subprocess.run(CC + [str(c), "-o", str(b)], check=True)
        r = subprocess.run([str(b)], capture_output=True, text=True)
        if (r.returncode == 0) != expect_pass:
            raise AssertionError("expect_pass=%s\n%s%s" % (
                expect_pass, r.stdout, r.stderr))
        return r


def text(old, new):
    def mutate(src):
        assert src.count(old) == 1, old
        return src.replace(old, new)
    return mutate


print(run(UI, True).stdout.strip())
MUTANTS = (
    ("read every frame", text("   if (g_build_line[0])\n      return g_build_line;\n", ""),
     "opens == 1"),
    ("a key's prefix matches", text("(p[kl] == ' ' || p[kl] == ':')", "1"),
     "1.2.3"),
    ("CR kept in the value", text("p[n] != '\\r' && ", ""),
     "1234567"),
    ("the commit dropped", text('"GBAdhoc %s (%s)", ver,\n               com);',
                                '"GBAdhoc %s", ver);'),
     "abc1234"),
)
for label, mutate, expect in MUTANTS:
    bad = run(mutate(UI), False)
    assert expect in bad.stderr, (label, bad.stderr)
    print("mutant (%s): failed as it must" % label)
print("build line: %d mutants fail" % len(MUTANTS))
