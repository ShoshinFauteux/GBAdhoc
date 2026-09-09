/* Single translation unit for stb_image, restricted to what the box-art
 * loader needs.
 *
 * PNG and JPEG only: those are what box-art scans actually come as, and every
 * other decoder stb ships (PSD, GIF, HDR, PIC, PNM, TGA) is dead weight in an
 * EBOOT.  No stdio -- the PSP reads through sceIo, so the loader hands stb a
 * memory buffer it has already read.  No SIMD: Allegrex has none of the ones
 * stb probes for, and the probes themselves do not compile for MIPS.
 */
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_PSD
#define STBI_NO_GIF
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_TGA
#define STBI_NO_BMP          /* we keep the streaming BMP reader in ui_psp.c */
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
