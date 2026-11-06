/* online_room.h -- which room code the Online screen asks the server for.
 *
 * A joiner always sends the code on screen.  A host sends it only once the
 * player has typed one (Room code, X ... X): the server then creates exactly
 * that room (relay_core.c on_create, "code in use" if it is taken).  A host
 * who never touched the code sends none and gets a random one, so two
 * strangers on the "AAAA" default do not collide.  beta.4 sent "" for every
 * host, so a typed ZAZA came back as a random HOVV.
 *
 * Header-only so tools/test_online_room.c tests exactly this. */
#ifndef ONLINE_ROOM_H
#define ONLINE_ROOM_H

#include <stdio.h>

static inline void online_room_request(int is_host, int typed, const char *shown,
                                       char out[16])
{
   /* a host asks for a room code (four letters); a joiner sends what is on
    * screen: a room code, or an 8/11-character invite code that the online
    * module decodes (claude/online-desktophost) */
   if (is_host)
      snprintf(out, 5, "%s", typed ? shown : "");
   else
      snprintf(out, 16, "%s", shown);
}

#endif
