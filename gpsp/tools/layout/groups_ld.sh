#!/bin/sh
# groups_ld.sh [--rodata] [--pins "NAME..."] GROUP... -- the object groups'
# part of the EBOOT's .text, as linker-script statements on stdout (code-health
# C3-2/C3-3; docs/LAYOUT-PINNING.md "Object groups", "Pins").  psp/Makefile
# writes psp/groups.ld with it on every make, and psp/gbadhoc.ld INCLUDEs that
# just before the default script's generic `*(.text .stub .text.* .gnu.linkonce.t.*)`,
# which stays as the catch-all.
#
#   --rodata  the same for .rodata (code-health C3-4: psp/ro.ld, INCLUDEd at
#             the start of the default's `.rodata` statement, which stays the
#             catch-all): `.rodata .rodata.* .gnu.linkonce.r.*` per file, and
#             no ungrouped check (the catch-all is where every other object's
#             tables belong).  psp/Makefile gives it the head group and an
#             empty group `ro` with --pins ro: `pin ro` comes right after the
#             start files, whose merged string table (every object's
#             .rodata.str*, merged into crt0.o's) is linked first.
#
#   GROUP   NAME:FILE...  one argument per group, in link order; the files in
#           command-line order (psp/Makefile's OBJS is the groups joined).  A
#           FILE is matched exactly as ld names it (`main_psp.o`,
#           `../frontend-common/fe_host.o`); a FILE with a wildcard, such as
#           `*/crt0.o` for psp-gcc's start files, as an ld wildcard.
#   --pins  the groups that start at a pin, by name, plus `core` for the
#           catch-all (instance A's core archive).  Each pin is
#           `. = ABSOLUTE(ABSOLUTE(.) + ((__pin_NAME - ABSOLUTE(.)) & 0x1FFF))`,
#           with __pin_NAME from psp/pins.ld (tools/layout/pins.awk,
#           tools/layout/layout.lock).  The outer ABSOLUTE is required: ld
#           2.44 types `. + X`, X absolute, as section-relative with X's
#           absolute value, so `. += (P - ABSOLUTE(.)) & 0x1FFF` lands at
#           P + ADDR(.text) (checked on a small link: 0x11009a80 for
#           0x08805a40).  An absolute right-hand side is taken as an address.
#
# WHY THE SAME BYTES.  The generic statement places its input sections file by
# file, in command-line order, each file's sections in their own order.  One
# statement per file, in the same order, places the same sections at the same
# addresses.  A pin whose address is where a pad object used to end fills the
# same gap with the same zeros (C3-3 recorded each pin from the pad's end).
#
# `:FILE` IS EXACT.  ld reads `:FILE` as "FILE, not a member of an archive".  A
# plain wildcard would be unsafe twice over: `*me_host.o` also matches
# home_host.o, and any plain pattern also matches archive members by their
# member name (`[!/]*.o` took the core archive's main.o).  A plain name with no
# wildcard is worse: ld opens it as an extra input file where the script names
# it.  `:FILE` has neither problem.
#
# THE UNGROUPED CHECK.  After the groups, every relative-path object outside an
# archive is matched once more between two marks (`[!/]` skips the start
# files, the LTO temporaries and crtend/crtn, which are absolute paths).  An
# object of ours that no group named has its text land there instead of in
# the catch-all, and the ASSERT fails the link (checked on the release:
# ui_psp.o and fe_util.o taken out of their group stop the link with this
# text).  A new object added to OBJS outside the group variables therefore
# stops the link instead of linking after instance A's cold core.
#
# THE PIN'S ASSERT (critique C-3 point 1, the overrun policy).  A pin always
# reaches its offset; "overrun" means the code before it grew past the pinned
# address, so the gap wrapped and the region moved 8 KiB up: one PSP-1000 heap
# page spent, silently.  With __pin_strict (the release always; tools/build.sh
# sets it) that fails the link, and only `tools/layout/layout_lock.py regen`
# moves a pin, by an explicit choice: --spend-page, --free-page or --move.
#
# POSIX sh only: it runs in the pspdev image (no python there) and in
# tools/tests/test_layout_groups.py on the host.
set -euf
IN='.text .stub .text.* .gnu.linkonce.t.*'
CHECK=1
if [ "${1:-}" = --rodata ]; then
  IN='.rodata .rodata.* .gnu.linkonce.r.*'
  CHECK=0
  shift
fi
PINS=''
if [ "${1:-}" = --pins ]; then
  PINS=" $2 "
  shift 2
fi

pinned() {
  case "$PINS" in *" $1 "*) return 0 ;; esac
  return 1
}

pin() {   # pin NAME OLD_SYMBOL
  echo "    /* pin $1 (docs/LAYOUT-PINNING.md \"Pins\"); until code-health C-3 the pad $2 */"
  # the pad's old name, at its old start, for nm-reading tools (one release)
  echo "    $2 = ABSOLUTE(.);"
  if [ "$1" = fe_head ]; then
    # layout_pad_cold began `jr $ra; nop`: the same 8 bytes, until C3-7
    echo '    LONG(0x03E00008) LONG(0)'
  fi
  echo "    __pin_from_$1 = ABSOLUTE(.);"
  echo "    ASSERT(__pin_strict == 0 || ABSOLUTE(.) <= __pin_$1, \"layout pin $1: the code before it grew past the pinned address, which would spend a PSP-1000 heap page; tools/layout/layout_lock.py regen decides (docs/LAYOUT-PINNING.md, Pins)\");"
  echo "    . = ABSOLUTE(ABSOLUTE(.) + ((__pin_$1 - ABSOLUTE(.)) & 0x1FFF));"
  echo "    __pin_at_$1 = ABSOLUTE(.);"
}

old_name() {
  case "$1" in
    fe_head) echo layout_pad_cold ;;
    fe_common) echo layout_pad_fe ;;
    fe_tail) echo layout_pad_gb ;;
    core) echo layout_pad_core ;;
    ro) echo layout_pad_ro ;;
    *) echo "layout_pin_$1" ;;
  esac
}

echo '/* groups.ld -- GENERATED by psp/Makefile with tools/layout/groups_ld.sh'
echo ' * (code-health C3-2/C3-3; docs/LAYOUT-PINNING.md "Object groups").  Never edit. */'
for g in "$@"; do
  name=${g%%:*}
  files=${g#*:}
  if pinned "$name"; then
    pin "$name" "$(old_name "$name")"
  fi
  echo "    /* group $name */"
  for f in $files; do
    echo "    :$f($IN)"
  done
done
if [ "$CHECK" = 1 ]; then
  echo '    /* an object outside every group: its text would land here */'
  echo '    __layout_ungrouped = .;'
  echo "    :[!/]*.o($IN)"
  echo '    ASSERT(. == __layout_ungrouped, "layout groups: an object in the link is in no psp/Makefile layout group (LAYOUT_GROUPS); add it to one");'
fi
if pinned core; then
  pin core "$(old_name core)"
fi
