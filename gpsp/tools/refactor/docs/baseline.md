# Phase T clean baseline: 01067e74 (beta.4 base), three flavours, built twice

Built 2026-11-05 by `gpsp/tools/refactor/flavours.sh` (T15) on branch `claude/mainpsp-tools`
(worktree `builds/mainpsp-tools-worktree`, uncommitted tools). No emulator source was edited.

- Source: commit `01067e74970052b141307a1c15130f1dce8bcb35`, tree `ed64f2ba50201812cc5c96e8715f793b324655b4`
- `workingTreeClean: true` in every `flavour.json` and in `runN/flavours.json` (git-archive export of `01067e74:gpsp`, 1,246 files, count checked against `git ls-tree`)
- `SOURCE_DATE_EPOCH=1793908328` (the commit time), `RFU_SCHED=1` (the beta line, as `tools/release.sh` sets for `*-beta*`)
- Layout: the commit's own pads (`LAYOUT_PIN=1`, `LAYOUT_PAD_BYTES=304`, from psp/Makefile). No `FLAV_*_ENV` overrides.

## Command (run from WSL, once per run dir)

```
O=/mnt/c/Users/DrSto/OneDrive/Desktop/GBAdhoc/builds/mainpsp-tools-out
cd /mnt/c/Users/DrSto/OneDrive/Desktop/GBAdhoc/builds/mainpsp-tools-worktree
flock $O/build.lock bash gpsp/tools/refactor/flavours.sh --commit 01067e74 --baseline --out $HOME/mainpsp-tools-out/baseline/run1
flock $O/build.lock bash gpsp/tools/refactor/flavours.sh --commit 01067e74 --baseline --out $HOME/mainpsp-tools-out/baseline/run2
```

The runs were built into a WSL-native dir and then copied here (see "Problems hit").
Each flavour calls the tree's own `tools/build.sh <profile> --out DIR`. build.sh is where the
ROOT vs psp make split lives:

| Flavour | build.sh profile | ROOT make (core) | psp/Makefile (frontend) |
|---|---|---|---|
| release | `release` | `GPSP_PROFILE=release RFU_SCHED=1` + the profile's feature flags | `EXTRA_DEFS=-DGPSP_PLAYABLE -DGPSP_PROFILE_RELEASE=1`, `ME_CATCH=1 PSP_EBOOT_ICON1=... GBA_LINK=1 REWIND=1 ONLINE=1 RFU_SCHED=1 HOT_DATA_GUARD=1` |
| lab | `lab` | `GPSP_PROFILE=harness RFU_SCHED=1` + features | `-DGPSP_PLAYABLE -DGPSP_KEEP_TELEMETRY -DGPSP_PERF_RIG -DGPSP_PROFILE_HARNESS=1`, `PSP_LARGE_MEMORY=0 LAB_HARNESS=1 ...` |
| harness | `harness` | `GPSP_PROFILE=harness RFU_SCHED=1` + features | same defines as lab, `PSP_LARGE_MEMORY=0 ...` (no LAB_HARNESS) |

The full flag lists are in each flavour's `build-manifest.json` (`coreFlags`, `frontendDefines`, `pspMakeFlags`).
Release `coreFlags` are identical to `builds/beta4-out/pin144-rel`'s.

## ELFs

| Flavour | run1 | run2 | sha256 (both runs) |
|---|---|---|---|
| release | `baseline/run1/release/gpsp_adhoc.elf` | `baseline/run2/release/gpsp_adhoc.elf` | `92e484e11f9402f484486e89cf43dbfe3ec956967f528a2c9e0323cfdbd506c1` |
| lab | `baseline/run1/lab/gpsp_adhoc.elf` | `baseline/run2/lab/gpsp_adhoc.elf` | `af9dd3cd262c2e3625be6f18f37267bd2f8aa81b09436870f18a05063a7da4e5` |
| harness | `baseline/run1/harness/gpsp_adhoc.elf` | `baseline/run2/harness/gpsp_adhoc.elf` | `ecd7fd606d43bce70edc6702640d885a22d58d92f7134d0e993ddefa9896ef9f` |

(paths relative to `C:/Users/DrSto/OneDrive/Desktop/GBAdhoc/builds/mainpsp-tools-out/`). The same three hashes
also came out of two earlier, discarded runs on the same day, so the ELFs were reproduced four times.

Each flavour dir also holds: `EBOOT.PBP`, the three PRXs, `build-manifest.json`, `flavour.json`,
`build.log` + `logs/.build-*.log` (make output), `gpsp_adhoc.map`, `su/**.su`, `elf.nm` (`psp-nm -S -n`),
`size.txt` / `size-A.txt` (`psp-size`, `psp-size -A -x`), `sidecar.txt` / `sidecar/` (verdicts).

## psp-size

| Flavour | text | data | bss | dec |
|---|---|---|---|---|
| release | 3,522,232 | 184,884 | 10,304,660 | 14,011,776 |
| lab | 3,690,692 | 188,468 | 10,722,452 | 14,601,612 |
| harness | 3,650,912 | 187,268 | 10,665,108 | 14,503,288 |

Selected sections (`psp-size -A -x`): release `.text` 0x2fcb6c @0x08804040, `.data.pin` 0x1c @0x08b5ffe4,
`.data` @0x08b60000, `.bss` 0x9cb98c @0x08b96000; lab `.data.pin` 0xe1c, `.bss` @0x08bc0000;
harness `.data.pin` 0x96c, `.bss` @0x08bb6000.

## Determinism: run1 == run2

`det_check.py --runs baseline/run1 baseline/run2` (output in `determinism.txt`), exit 0:

- **All three flavours IDENTICAL**: every SHF_ALLOC section by name, address, size, type, flags and bytes (NOBITS by
  name/address/size), program headers and `e_entry`. The whole ELF is byte-identical (same sha256), and so are
  `EBOOT.PBP`, `gbadhoc_me.prx`, `gbadhoc_home.prx`, `gbadhoc_online.prx` and every `.su` file (87 / 90 / 86).
- `gpsp_adhoc.map` differs in 24 lines per flavour, and **only** in the LTO temporary names
  (`/tmp/ccBLBIJF.ltrans0.ltrans.o` vs `/tmp/ccLahKFO...`). After normalising `/tmp/cc??????.` the maps are identical.
  det_check reports this as "same modulo /tmp/cc*.ltrans names". Map consumers must not key on those names.
- Sources of non-determinism found and handled:
  - `__DATE__`/`__TIME__` in the harness/lab build stamp (`fe_evt("build stamp=%s_%s", ...)`, main_psp.c:13926).
    flavours.sh pins `SOURCE_DATE_EPOCH` to the commit time with a `docker` shim and then **checks** that the
    stamp `Nov  5 2026 19:52:08` (UTC) is in the lab and harness ELFs (`flavour.json` `buildStamp: pinned`).
    The release flavour compiles fe_evt out and contains no stamp. A build without the pin (`--no-sde`) was not
    re-measured here; it is expected to differ only in that stamp's bytes.
  - No build ids, no other timestamps were found: the ELF bytes are equal, so nothing else varies.

## SHF_ALLOC count: 24, not 25

`scratch/crit_secs.py` lists **24** SHF_ALLOC sections in all three clean ELFs and also in the old
`builds/beta4-out/pin144-rel/gpsp_adhoc.elf`: `.MIPS.abiflags .init .text .fini .sceStub.text .lib.ent.top .lib.ent
.lib.ent.btm .lib.stub.top .lib.stub .lib.stub.btm .rodata.sceModuleInfo .rodata.sceResident .rodata.sceNid .rodata
.eh_frame .ctors .dtors .data.pin .data .data.gbahot .sdata .sbss .bss` (22 PROGBITS + 2 NOBITS). PLAN §5/T2 and
PLAN-critique.md:45 say 25. That figure does not reproduce on either ELF; T2's acceptance should read 24.

## -fstack-usage and the map, without changing the ELF

Neither is produced by the normal build. `flavours_sidecar.sh` (runs in the same pspdev container after build.sh):

- replays every compile line make logged, with `-fstack-usage` and `-o` redirected to a side dir, and compares each
  object with the real one. Result per flavour (release / lab / harness): byte-identical 40 / 38 / 38, identical after
  `--strip-debug` 51 / 55 / 51 (the flag is recorded in `DW_AT_producer`, debug info only), **DIFF 0**, 3 PRX
  `exports.c` objects skipped (build.mak deletes their generated source; they are not in gpsp_adhoc.elf).
  So `-fstack-usage` changes no loadable byte of any object.
- relinks gpsp_adhoc.elf with `-Wl,-Map=` and runs `psp-fixup-imports`: the relinked ELF is **byte-identical** to the
  build's ELF in every flavour (`relink=SAME`), so the map describes the kept ELF.

The kept ELF is always the normal build's, never a sidecar product.

## Comparison with the old dirty `pin144-rel`

`pin144-rel` (`workingTreeClean: false`, `LAYOUT_PAD_BYTES=144`) is not this baseline. det_check against the clean
release: 21 of 24 sections differ; `.text` is 160 B longer in the clean build, `.data.pin` 156 B vs 28 B, and `.data`
has 4,255 differing bytes. The pad accounts for the `.data.pin` change; the rest comes from the dirty tree and/or the
pad (not separated here). Every x4 figure measured on pin144-rel must be re-measured on these ELFs; the 304/144 pair
of T1's acceptance needs one extra build, e.g. `FLAV_RELEASE_ENV="LAYOUT_PAD_BYTES=144" flavours.sh --commit 01067e74
--flavours release ...`.

## Problems hit (fixed in flavours.sh)

1. **The OneDrive /mnt/c path breaks under WSL.** Two attempts writing the export and the outputs straight into
   `builds/mainpsp-tools-out/baseline/` failed: `tar: Cannot mkdir: No such file or directory`, `wslpath` not seeing
   an existing dir, `mkdir: Already exists` for a dir neither side could list. flavours.sh now exports once into a
   native work dir and caches the Windows path for `git.exe`. These runs used a native `--out` and were copied over
   from `\\wsl$\Ubuntu\home\rayquaza\mainpsp-tools-out\baseline` (that copy is still there).
2. **Root-owned outputs.** The sidecar container wrote `su/` as root, so the next run could not clear the dir and would
   have mixed stale files into a new run. The sidecar now chowns `/out` back to the caller, and flavours.sh refuses
   (exit 1) a flavour dir it cannot empty.
3. An earlier run1 was discarded because flavours.sh was edited while it ran (bash reads scripts as it goes). Both
   runs here were built start to finish with the final script.

## Self-tests

- `flavours.sh --selftest` (= `tests/test_flavours.sh`, no PSP build): PASS. The controls and mutants cover a clean
  baseline, a dirty tree refused with `--baseline` (exit 3, before any docker run), the `could not fixup imports`
  warning with build.sh exiting 0 (exit 4), a docker that drops `SOURCE_DATE_EPOCH` (caught by the stamp check),
  `--commit` building only the committed tree, a failing build.sh and an unknown flavour.
- `det_check.py --selftest <elf>` on all three run2 ELFs: PASS. The mutants are one byte flipped in `.sceStub.text`,
  `.rodata.sceModuleInfo`, `.text`, `.lib.stub` and the last PROGBITS byte, plus a NOBITS size or address change.
  All are caught, and a `.comment` flip is correctly ignored.
