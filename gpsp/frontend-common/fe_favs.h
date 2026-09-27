/* fe_favs.h — the favourites list: a set of ROM paths relative to roms/.
 *
 * One GLOBAL list for every console.  The browser only ever shows the ROMs
 * that match the active console's extension, so filtering the favourites
 * view by console is free: a .gbc entry is simply never in a GB scan.
 *
 * STATIC, NOT HEAP.  gpSP's init_gamepak_buffer() mallocs 1 MiB blocks until
 * malloc fails, so once a game has been loaded there is no heap for the
 * browser to allocate from when the player backs out to the list.  The whole
 * structure is a fixed ~14 KB the caller places in BSS.
 *
 * Pure C, no platform calls: the PSP side reads the file into `pool` and
 * calls fe_favs_parse_pool(); the host test exercises the same code. */
#ifndef FE_FAVS_H
#define FE_FAVS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FE_FAVS_MAX   256                  /* entries                        */
#define FE_FAVS_POOL  (12 * 1024)          /* bytes of NUL-separated names  */

typedef struct {
   unsigned       n;                       /* live entries                   */
   unsigned       used;                    /* bytes of pool in use           */
   unsigned       hash[FE_FAVS_MAX];       /* FNV-1a of the name, for lookup */
   unsigned short off[FE_FAVS_MAX];        /* name = pool + off[i]           */
   char           pool[FE_FAVS_POOL];
} fe_favs;

/* FNV-1a over the bytes of `s`.  Exposed so the browser can pre-hash a scan
 * entry once and test membership without a strcmp per row per frame. */
unsigned fe_favs_hash(const char *s);

void fe_favs_clear(fe_favs *f);

/* The file's text has been read into f->pool[0..len).  Splits it into lines
 * IN PLACE: blank lines, '#' comments, CR and surrounding whitespace are
 * dropped, duplicates keep their first position, and entries past
 * FE_FAVS_MAX are ignored.  Returns the number of entries kept. */
unsigned fe_favs_parse_pool(fe_favs *f, size_t len);

/* Index of `rel` in the list, or -1. */
int fe_favs_find(const fe_favs *f, const char *rel);
/* As fe_favs_find, with the hash already computed. */
int fe_favs_find_h(const fe_favs *f, const char *rel, unsigned h);

/* 0 on success.  -1 when the list or the pool is full, or the name is
 * empty; adding a present name is a success and changes nothing. */
int fe_favs_add(fe_favs *f, const char *rel);
/* 0 on success, -1 when absent.  The pool is compacted in place. */
int fe_favs_remove(fe_favs *f, const char *rel);

/* Writes one name per line, LF-terminated, into out.  Returns the number of
 * bytes the full text needs; if that exceeds cap nothing past cap-1 is
 * written and the result is still NUL-terminated (when cap > 0). */
size_t fe_favs_serialise(const fe_favs *f, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* FE_FAVS_H */
