# arm/ -- inherited dynarec backends (32-bit ARM and ARM64)

Inherited from gpSP. Not built or maintained by GBAdhoc; GBAdhoc's dynarec features exist only in
the MIPS backend (`mips/`), so this may not compile against the current core. Kept as reference for
ports.

`arm_*` is the 32-bit ARM backend (`CPU_ARCH=arm`), `arm64_*` the ARM64 one (`CPU_ARCH=arm64`).
No GBAdhoc build and no PC test build selects either: the PSP builds use `mips/`, the SDL desktop
twin (`sdl/Makefile`) builds the interpreter (`HAVE_DYNAREC=0`), and the dynarec twin
(`tools/drprof`) runs the MIPS backend under qemu.

A port to an ARM console (a Vita port builds on GBAdhoc) starts from here, against the MIPS
backend's current behaviour. Known gap: `arm64_emit.h`'s `arm_block_memory_final_store` still
stores R15 the old way (a KNOWN BUG note there; the MIPS fix, commit 6eb61d61, is the model).
