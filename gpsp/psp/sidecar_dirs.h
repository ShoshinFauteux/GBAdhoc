/* sidecar_dirs.h -- where a ROM's battery save and save states live
 * (4.6.0-beta.4, GitHub issue #31; docs in README.md "Folders").
 *
 *   <app>/roms/            ROMs, any layout; roms/GBA, roms/GB, roms/GBC are
 *                          created empty on first run.  ROMs are NEVER moved.
 *   <app>/saves/<rel>      battery saves:  Game.sav, Game.gb.sav, Game.gbc.sav
 *   <app>/states/<rel>     save states and their pictures: Game.st0..st4,
 *                          Game.gb.st0.., + .thumb / .shot
 *
 * <rel> is the ROM's path under roms/ ("GBA/hacks/Game.gba"), so saves and
 * states mirror the ROM's folders and two ROMs can never share a file.  The
 * per-console suffixes are exactly the beta.3 ones: the callers apply them to
 * the MIRRORED ROM path (sc_mirror) the way they applied them to the ROM path.
 *
 * A ROM that is not under roms/ (an XMB entry naming an absolute path) keeps
 * its files beside it, as every build before beta.4 did.
 *
 * MIGRATION.  The files beside a ROM (the "legacy" layout, every build up to
 * 4.6.0-beta.3) are moved the first time that game is opened, one group at a
 * time (the save and its .bak/.old; every state slot with its pictures and
 * .old): each file is copied to a temp name, the copy is read back and
 * compared (size and CRC32) with the original, renamed into place and read
 * back again, and only when EVERY file of the group has arrived are the
 * originals removed.  Any failure removes the copies made by this pass and
 * the game keeps using the legacy files.  A file already present in the new
 * place is never overwritten: an identical one counts as moved (the legacy
 * copy is removed: an interrupted earlier pass), a different one is a
 * conflict -- the new file is used and the legacy one is left where it is.
 *
 * Pure C over an injectable file-system table, so tools/tests/
 * test_sidecar_dirs.c runs all of it on the host with failure injection. */
#ifndef PSP_SIDECAR_DIRS_H
#define PSP_SIDECAR_DIRS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SC_SAVES_DIR  "saves"
#define SC_STATES_DIR "states"
#define SC_MIG_SUFFIX ".mig"           /* the copy's name before it is final */

typedef struct sc_fs
{
   void *user;
   void *(*open)(void *user, const char *path, int write); /* NULL = fail */
   int   (*read)(void *user, void *h, void *buf, unsigned n);  /* -1 = fail */
   int   (*write)(void *user, void *h, const void *buf, unsigned n);
   int   (*close)(void *user, void *h);                 /* 0 = ok */
   int   (*exists)(void *user, const char *path);       /* 1 / 0 / -1 */
   int   (*rename)(void *user, const char *from, const char *to);
   int   (*remove)(void *user, const char *path);
   int   (*mkdir)(void *user, const char *path);        /* result ignored */
   void  (*log)(void *user, const char *line);          /* may be NULL */
} sc_fs;

/* The PSP's (sceIo), built unless SC_HOST_TEST. */
extern const sc_fs sc_fs_psp;

/* rom_path's part under rom_dir ("GBA/x.gba"), compared case-insensitively
 * as FAT does; NULL when the ROM is not inside rom_dir. */
const char *sc_rel_of(const char *rom_dir, const char *rom_path);

/* out = base/dir/rel (dir = SC_SAVES_DIR or SC_STATES_DIR).  0 = fits. */
int sc_mirror(char *out, size_t cap, const char *base, const char *dir,
              const char *rel);

/* Create every folder of `file_path` below `base` (base itself included).
 * 0 when the file's folder exists afterwards. */
int sc_mkdirs_for(const sc_fs *fs, const char *base, const char *file_path);

/* First run: roms/GBA, roms/GB, roms/GBC (each with an empty .keep),
 * saves/ and states/.  Only what is missing is made. */
void sc_first_run(const sc_fs *fs, const char *base, const char *rom_dir);

/* The two groups.  `path` is the save path, or the slot-1 state path. */
enum { SC_GROUP_SAVE = 0, SC_GROUP_STATES = 1 };

/* Outcome of sc_migrate. */
enum
{
   SC_MIG_NONE = 0,     /* no legacy file: nothing to do                 */
   SC_MIG_MOVED = 1,    /* every legacy file is in the new place          */
   SC_MIG_FAILED = -1   /* nothing changed: keep using the legacy files   */
};

typedef struct
{
   unsigned moved, dup, conflict, failed, left;   /* files */
} sc_report;

/* Move group `group` from legacy_path's family to new_path's.  new_path's
 * folders must exist (sc_mkdirs_for).  `rep` may be NULL. */
int sc_migrate(const sc_fs *fs, int group, const char *legacy_path,
               const char *new_path, sc_report *rep);

/* Does any file of the group exist at `path` (its .old/.bak included)? */
int sc_group_exists(const sc_fs *fs, int group, const char *path);

/* The path to use, read-only callers (the browser, a sharer): the new one
 * if anything of the group is there, else the legacy one if anything is
 * there, else the new one.  Returns 1 when it picked the legacy path. */
int sc_pick(const sc_fs *fs, int group, const char *legacy_path,
            const char *new_path, const char **out);

/* The whole open-a-game step for one group: make the new folders, migrate,
 * decide.  *use is new_path, or legacy_path when the folders cannot be made
 * or the migration failed.  Returns sc_migrate's value (SC_MIG_FAILED also
 * for a folder failure). */
int sc_settle(const sc_fs *fs, const char *base, int group,
              const char *legacy_path, const char *new_path,
              const char **use, sc_report *rep);

/* Group member i (0..): the suffix appended to the group's path, or NULL
 * past the end.  For states, *slot (1..5) says which slot path it is on. */
const char *sc_member(int group, unsigned i, unsigned *slot);

#ifdef __cplusplus
}
#endif

#endif
