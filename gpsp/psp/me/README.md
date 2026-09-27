# psp/me — the Media Engine module

`gbadhoc_me.prx` is a kernel module that boots the PSP's second core and runs
the GBA scanline renderer (`video.cc`) on it (ADR-0080). It is built as a
sub-make from `psp/Makefile`, ships beside `EBOOT.PBP`, and is loaded at runtime
with `kuKernelLoadModule` — nothing here touches the EBOOT link.

## The two tracked binaries are generated, and they stay tracked

`gbadhoc_me.prx` and `gbadhoc_me.elf` are build outputs, and every
`tools/build.sh` run regenerates them. They are committed anyway, deliberately:

* the release package needs the `.prx`, and it cannot be produced without the
  pspdev toolchain, so a clone that only wants to *package* a release must find
  it present;
* it is reproducible — the tracked copy and a fresh build agree byte for byte
  (sha256 `7612a2e1425150ad…`, the same value the 7283f13 manifest recorded).

If you change anything under `psp/me/` or one of the shared renderer inputs,
**commit the rebuilt `.prx` with it.** `tools/build.sh` rebuilds the module and
records its sha256 in `psp/build-manifest.json`; `tools/make_release.sh` refuses
to package a PRX older than the module sources or its explicit shared
dependencies. Keep those dependency checks current when adding a new input.

The object files (`*.o`) are gitignored, as they should be.

## Pixel format

The renderer's channel order must match the core's and the frontend's.
`PSP_PIXFMT` now reaches this sub-make from `psp/Makefile`; it used to be
hardcoded here, which made it a third independent copy of the same decision.
A core/frontend mismatch is a link error; a PRX mismatch cannot be, because the
PRX is loaded at runtime — so it is inherited instead. See
`docs/BUILD-PROFILES.md`.
