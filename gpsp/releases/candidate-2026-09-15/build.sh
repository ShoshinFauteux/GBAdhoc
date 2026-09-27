#!/bin/sh
set -eu
cd /build
make -C psp clean
make platform=psp1 clean
make platform=psp1 GIT_VERSION=ecdd8bc-romload1 SMC_GATES=1 SMC_GATES_SIMPLE=1 SMC_GATES_RANKED=1 GBA_PC_MASK=1 BADJUMP_SAFE=1 -j4
make -C psp EXTRA_DEFS='-DGPSP_PLAYABLE -DVID_TRIPLE -DGPSP_ROMLOAD_DIAGNOSTICS' PSP_EBOOT_TITLE='GBAdhoc Quick Load'
sha256sum psp/EBOOT.PBP psp/me/gbadhoc_me.prx
