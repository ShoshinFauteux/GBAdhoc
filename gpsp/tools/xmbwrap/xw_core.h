/* xw_core.h -- make a game's own XMB entry for GBAdhoc (issue #29,
 * docs/XMB-LAUNCH.md).  Platform-free C: the Windows maker (xw_win.c), the
 * command line (xw_cli.c) and, later, the PSP's own maker share it, so every
 * maker writes the same entry.
 *
 * An entry is a folder PSP/GAME/<folder>/ holding
 *   EBOOT.PBP   the launcher's code (DATA.PSP, from the launcher template),
 *               a PARAM.SFO with the entry's title, ICON0 (144x80) and PIC1
 *               (480x272) made from the chosen art;
 *   launch.ini  which ROM (relative to GBAdhoc's roms/) and where GBAdhoc is.
 * No ROM goes into an entry. */
#ifndef XW_CORE_H
#define XW_CORE_H

#include <stddef.h>

typedef struct
{
   unsigned char *data;
   size_t len;
} xw_buf;

void xw_buf_free(xw_buf *b);

/* A file into memory / memory into a file (paths in UTF-8). 0 = ok. */
int xw_read_file(const char *path, xw_buf *out);
int xw_write_file(const char *path, const void *data, size_t len);

/* PBP: the eight sections (PARAM.SFO, ICON0.PNG, ICON1.PMF, PIC0.PNG,
 * PIC1.PNG, SND0.AT3, DATA.PSP, DATA.PSAR) as views into `pbp`. */
enum { XW_SFO, XW_ICON0, XW_ICON1, XW_PIC0, XW_PIC1, XW_SND0, XW_PSP, XW_PSAR,
       XW_NSECT };
int xw_pbp_split(const xw_buf *pbp, const unsigned char *sect[XW_NSECT],
                 size_t len[XW_NSECT]);
int xw_pbp_join(const unsigned char *const sect[XW_NSECT],
                const size_t len[XW_NSECT], xw_buf *out);

/* PARAM.SFO with one string key replaced (TITLE). 0 = ok. */
int xw_sfo_set_string(const unsigned char *sfo, size_t len, const char *key,
                      const char *value, unsigned max_len, xw_buf *out);
/* Read a string key (for tests and for showing an entry's title). */
int xw_sfo_get_string(const unsigned char *sfo, size_t len, const char *key,
                      char *out, size_t cap);

/* Art: any image stb_image reads (PNG, JPEG, BMP, GIF, TGA) into a PNG of
 * exactly w x h.  `cover` 1 fills the frame (crops), 0 fits inside it with
 * transparent margins.  0 = ok. */
int xw_image_png(const char *path, int w, int h, int cover, xw_buf *png);

/* A folder name from a title: letters, digits, space, - and _, at most 32. */
void xw_folder_from_title(const char *title, char *out, size_t cap);

typedef struct
{
   const unsigned char *template_pbp;  /* the launcher's EBOOT.PBP */
   size_t template_len;
   const char *title;        /* XMB title, UTF-8 */
   const char *rom;          /* relative to GBAdhoc's roms/ (or dev:/...) */
   const char *gbadhoc;      /* GBAdhoc's folder, e.g. "PSP/GAME/GBAdhoc" */
   const char *icon_image;   /* NULL = no ICON0 (the XMB's default) */
   const char *bg_image;     /* NULL = no PIC1 */
   const char *root;         /* the card's root ("G:/", "/mnt/psp") */
   const char *folder;       /* NULL = from the title */
} xw_entry;

/* A ROM picked on the PSP's card (".../PSP/GAME/<gbadhoc>/roms/<rel>"):
 * the card's root, GBAdhoc's folder ("PSP/GAME/<gbadhoc>") and the ROM
 * relative to roms/.  0 = it is inside a GBAdhoc install; -1 = it is not. */
int xw_infer(const char *rom_path, char *root, size_t root_cap, char *gbadhoc,
             size_t gb_cap, char *rel, size_t rel_cap);

/* The ROM as launch.ini should name it.  A non-ASCII name ("Pokémon") is
 * written in its FAT 8.3 form, which the PSP firmware always opens (Windows
 * reads it off the card); elsewhere it stays long and 1 is returned. */
int xw_rom_for_psp(const char *rom_path, const char *rel, char *out, size_t cap);

/* GBAdhoc's own art for a ROM, if the install has it: hero/<stem>.png (a
 * 480x272 background) and boxart/<stem>.{png,bmp,jpg} (the icon).  Either
 * comes back "" when there is none.  `install` = root + gbadhoc folder. */
void xw_default_art(const char *install, const char *rom_rel, char *hero,
                    size_t hero_cap, char *box, size_t box_cap);

/* A title from a ROM file name: the stem, "(USA)"-style tags kept. */
void xw_title_from_rom(const char *rom_rel, char *out, size_t cap);

/* Write PSP/GAME/<folder>/{EBOOT.PBP,launch.ini} under entry->root.  On
 * success `made` holds the folder written.  On failure `err` says why. */
int xw_make_entry(const xw_entry *e, char *made, size_t made_cap, char *err,
                  size_t err_cap);

#endif
