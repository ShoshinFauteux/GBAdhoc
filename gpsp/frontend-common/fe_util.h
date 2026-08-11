/* fe_util.h — small shared helpers: crc32, sha1, BMP dump, ini reader. */
#ifndef FE_UTIL_H
#define FE_UTIL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CRC-32 (IEEE, zlib-compatible). Used for SRAM dirty detection + EVT lines. */
uint32_t fe_crc32(uint32_t crc, const void *data, size_t len);

/* SHA-1: out_hex must hold 41 bytes (40 hex chars + NUL). */
void fe_sha1_hex(const void *data, size_t len, char *out_hex);

/* Write a 240x160-ish RGB565 frame as a bottom-up 24bpp BMP.
 * pitch is in BYTES. Returns 0 on success. */
int fe_bmp_write_rgb565(const char *path, const uint16_t *pix,
                        unsigned w, unsigned h, size_t pitch);

/* Same, but the source is PSP-native GE 5650 (R in bits 0-4, G 5-10,
 * B 11-15 — the reverse of libretro RGB565). Used by the PSP frontend's
 * GE drawbuffer readback (GU color-order regression check). */
int fe_bmp_write_psp565(const char *path, const uint16_t *pix,
                        unsigned w, unsigned h, size_t pitch);

/* Minimal ini/key=value reader ('#' and ';' comments, sections ignored).
 * Copies the value for `key` into out (trimmed). Returns 1 if found. */
int fe_ini_get(const char *path, const char *key, char *out, size_t out_sz);
/* Integer convenience: returns def if missing/unparsable. */
long fe_ini_get_int(const char *path, const char *key, long def);

/* Replace (or append) `key = value` in a small ini file, preserving all
 * other lines. Creates the file if missing. Returns 0 on success. */
int fe_ini_set(const char *path, const char *key, const char *value);
int fe_ini_set_int(const char *path, const char *key, long value);

#ifdef __cplusplus
}
#endif

#endif /* FE_UTIL_H */
