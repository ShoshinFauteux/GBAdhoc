/* The room code the Online screen asks the server for (psp/online_room.h).
 * beta.4 regression: a host who typed ZAZA got a random room, because the
 * frontend sent "" for every host. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../psp/online_room.h"

int main(void)
{
   char out[16];

   /* the owner's case: typed ZAZA, Create room -> ask for ZAZA */
   online_room_request(1, 1, "ZAZA", out);
   assert(!strcmp(out, "ZAZA"));
   /* a host who never touched the code: the server picks one */
   online_room_request(1, 0, "AAAA", out);
   assert(!strcmp(out, ""));
   /* a joiner always sends the code on screen, typed or not */
   online_room_request(0, 0, "AAAA", out);
   assert(!strcmp(out, "AAAA"));
   online_room_request(0, 1, "HOVV", out);
   assert(!strcmp(out, "HOVV"));
   /* never more than four letters, always terminated */
   memset(out, 'x', sizeof(out));
   online_room_request(1, 1, "ZAZAQ", out);
   assert(!strcmp(out, "ZAZA"));
   /* a joiner may send an invite code: 8 or 11 characters, kept whole */
   online_room_request(0, 0, "AB3D5F7H9JK", out);
   assert(!strcmp(out, "AB3D5F7H9JK"));
   online_room_request(0, 1, "AB3D5F7H", out);
   assert(!strcmp(out, "AB3D5F7H"));
   /* the box: an untouched code reads "random" (it IS random for a host);
    * a typed one -- AAAA included -- reads as typed */
   assert(!strcmp(online_code_shown(0, "AAAA", "random"), "random"));
   assert(!strcmp(online_code_shown(1, "AAAA", "random"), "AAAA"));
   assert(!strcmp(online_code_shown(1, "ZAZA", "random"), "ZAZA"));
   online_room_request(1, 1, "AAAA", out);       /* typed AAAA: room AAAA */
   assert(!strcmp(out, "AAAA"));
   puts("PASS: online room code (host typed / host default / joiner / box)");
   return 0;
}
