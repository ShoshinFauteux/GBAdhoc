/* online_link.h -- the ABI between the EBOOT and gbadhoc_online.prx, the
 * on-demand online-play module (docs/ONLINE-TUNNEL.md section 8).
 *
 * WHY A MODULE.  Online play is experimental and most players never turn it
 * on, but in the EBOOT its ~14 KB of code cost 16 KiB of boot heap on every
 * console: the pinned layout moves in 8 KiB steps.  As a PRX it is loaded
 * only when a session goes online, into the 1 MiB of user partition that
 * PSP_HEAP_SIZE_KB(-1024) leaves outside newlib's heap (where the net
 * modules, the ME and HOME modules already live), and unloaded when the
 * session ends.  Boot heap equals an ONLINE=0 build's.
 *
 * THE HANDSHAKE.  The EBOOT starts the module with one argument, a pointer
 * to an online_link.  The module checks the host table's magic, version and
 * size, fills the api table (its own magic, version and size), and returns
 * 0; a mismatch returns OL_ERR_ABI and the EBOOT unloads it.  No exports
 * beyond syslib: everything goes through these two tables (the ADR-0080
 * pattern of gbadhoc_home.prx).
 *
 * The module has no libc of its own: memory and formatting come from the
 * host table, so it never takes a heap of its own from the partition.
 */
#ifndef ONLINE_LINK_H
#define ONLINE_LINK_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "../../netdrv/netdrv.h"

#define OL_MAGIC_HOST 0x474F4C48u   /* 'GOLH' */
#define OL_MAGIC_API  0x474F4C41u   /* 'GOLA' */
/* Bump on ANY change to either table or to the semantics of a call. */
#define OL_VERSION    4u   /* 3: room[16] (invite codes), api.slot; the host chip/toast. 4: both (beta.4 fix 1) */
#define OL_ERR_ABI    (-0x4F4C)

typedef struct online_host
{
   uint32_t magic, version, size;
   void *(*calloc_)(size_t n, size_t sz);
   void  (*free_)(void *p);
   int   (*vsnprintf_)(char *out, size_t cap, const char *fmt, va_list ap);
   void  (*evt)(const char *line);              /* one EVT line, no newline */
   /* infrastructure Wi-Fi (psp/mgift_net.c): the EBOOT brings it up BEFORE
    * loading the module -- the module imports sceNetInet / sceNetResolver,
    * whose libraries exist only once the net modules are loaded -- and takes
    * it down after unloading it.  The module only asks. */
   int   (*wifi_is_up)(void);
   const char *(*local_ip)(void);
   /* a bounded wait is progress for the harness stall exit; NULL in a release */
   void  (*beat)(void);
   /* the session chip and a toast (osd_psp.c): the module shows the path
    * to the other PSP and its ping, "Online ABCD  Direct 12 ms", and a
    * change of path.  In the host TABLE, not EBOOT code: the EBOOT has no
    * text to spare (docs/LAYOUT-PINNING.md) */
   void  (*chip)(const char *text);
   void  (*toast)(const char *fmt, ...);
} online_host;

/* What the session needs from the EBOOT's configuration (online_psp.c). */
typedef struct online_cfg
{
   char relay[64];              /* host name or a.b.c.d[:port] */
   char room[16];               /* as typed: 4 letters (a room code, looked up),
                                   8 or 11 symbols (an invite code), "" = host:
                                   the server picks */
   int  direct, wifi, mode;
   unsigned drop_direct_ms;
} online_cfg;

typedef struct online_api
{
   uint32_t magic, version, size;
   /* DNS, socket, room (Wi-Fi is already up); blocks up to timeout_ms.
    * 0 = in the room; otherwise why[] says why, and nothing of the module's
    * is left up (Wi-Fi is the EBOOT's to take down). */
   int  (*up)(const online_cfg *cfg, int is_host, const char *build,
              unsigned timeout_ms, char *why, unsigned why_cap);
   void (*down)(void);
   int  (*is_up)(void);
   const char *(*room)(void);
   int  (*room_mode)(void);
   void (*iface)(nd_transport *out);
   int  (*connected)(void);
   void (*stats_evt)(const char *at);
   void (*status)(char *out, unsigned cap);
   int  (*slot)(void);          /* 0 = this console hosts the room (v3) */
} online_api;

typedef struct online_link
{
   const online_host *host;
   online_api *api;             /* filled by module_start */
} online_link;

#endif
