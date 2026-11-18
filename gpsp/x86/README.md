# x86/ -- inherited dynarec backend

Inherited from gpSP. Not built or maintained by GBAdhoc; GBAdhoc's dynarec features exist only in
the MIPS backend (`mips/`), so this may not compile against the current core. Kept as reference for
ports.

Checked 2026-11-17: it does not compile. `make platform=unix` on x86-64 selects this backend
(`HAVE_DYNAREC=1`, `CPU_ARCH=x86_32`) and stops in `jit_code.h` ("JIT_CODE_DISCIPLINE is implemented
for the MIPS dynarec only") and in `cpu_threaded.c`'s MIPS-only emitter calls.

The PC test builds:
- The SDL desktop twin (`sdl/Makefile`) builds the core with `HAVE_DYNAREC=0` (the interpreter) and
  does not use this folder.
- `tools/linkbench` (its default arm) and `tools/gbamirror` build `make platform=unix` with this
  backend, so they fail the same way; `tools/linkbench`'s `INTERP=1` arm builds the interpreter.
- The dynarec twin (`tools/drprof`) runs the MIPS backend under qemu (`platform=drprof-mipsel`).

A few GBAdhoc changes that touch every backend's stub file at once (the `reg[32..63]` registry,
`reg_slots.h`) reached `x86_stub.S`; nothing builds or tests them.
