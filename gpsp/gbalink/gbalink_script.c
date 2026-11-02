/* gbalink_script.c -- see gbalink_script.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gbalink_script.h"

static uint16_t parse_btns(const char *s)
{
   static const char *nm[10] = { "A", "B", "SELECT", "START", "RIGHT", "LEFT",
                                 "UP", "DOWN", "R", "L" };
   uint16_t k = 0;
   char buf[128];
   const char *p = s;
   while (*p)
   {
      size_t n = strcspn(p, "+");
      unsigned i;
      if (n >= sizeof(buf))
         n = sizeof(buf) - 1;
      memcpy(buf, p, n);
      buf[n] = 0;
      for (i = 0; i < 10; i++)
         if (!strcmp(buf, nm[i]))
            k |= (uint16_t)(1u << i);
      p += n;
      if (*p == '+')
         p++;
   }
   return k;
}

static void push(gbalink_script *sc, uint16_t k, unsigned n)
{
   while (n-- && sc->n < sc->cap)
      sc->keys[sc->n++] = k;
}

static void step(gbalink_script *sc, const char *line)
{
   char cmd[32], a1[128];
   int n2 = 0;
   a1[0] = 0;
   if (sscanf(line, "%31s %127s %d", cmd, a1, &n2) < 1)
      return;
   if (!strcmp(cmd, "wait"))
      push(sc, 0, (unsigned)atoi(a1));
   else if (!strcmp(cmd, "press"))
   {
      push(sc, parse_btns(a1), n2 > 0 ? (unsigned)n2 : 2);
      push(sc, 0, 2);
   }
   else if (!strcmp(cmd, "hold"))
      push(sc, parse_btns(a1), (unsigned)(n2 > 0 ? n2 : 0));
   else if (!strcmp(cmd, "dump"))
   {
      if (sc->n < sc->cap)
         sc->dump[sc->n] = 1;
   }
   else if (!strcmp(cmd, "evt"))
   {
      if (sc->n < sc->cap && !sc->mark[sc->n])
      {
         size_t l = strlen(a1) + 1;
         sc->mark[sc->n] = (char *)malloc(l);
         if (sc->mark[sc->n])
            memcpy(sc->mark[sc->n], a1, l);
      }
   }
}

int gbalink_script_load(gbalink_script *sc, const char *path, unsigned cap)
{
   FILE *f;
   char line[256];
   /* The heap: a script is a harness run, and a static 16 KiB was in every
    * image. */
   char (*rep)[256];
   int in_rep = 0, nrep = 0, reps = 0;

   memset(sc, 0, sizeof(*sc));
   sc->cap = cap;
   sc->keys = (uint16_t *)calloc(cap ? cap : 1, sizeof(uint16_t));
   sc->dump = (uint8_t *)calloc(cap ? cap : 1, 1);
   sc->mark = (char **)calloc(cap ? cap : 1, sizeof(char *));
   if (!sc->keys || !sc->dump || !sc->mark)
      return -1;
   if (!path || !path[0])
      return 0;
   if (!(f = fopen(path, "r")))
      return -1;
   if (!(rep = (char (*)[256])malloc(64 * 256)))
   {
      fclose(f);
      return -1;
   }
   while (fgets(line, sizeof(line), f))
   {
      char *p = strpbrk(line, "#;\r\n");
      if (p)
         *p = 0;
      p = line;
      while (*p == ' ' || *p == '\t')
         p++;
      if (!*p)
         continue;
      if (!strncmp(p, "repeat", 6))
      {
         in_rep = 1; nrep = 0; reps = atoi(p + 6);
         continue;
      }
      if (in_rep)
      {
         if (strncmp(p, "endrepeat", 9))
         {
            if (nrep < 64)
               snprintf(rep[nrep++], sizeof(rep[0]), "%s", p);
            continue;
         }
         in_rep = 0;
         while (reps-- > 0)
         {
            int i;
            for (i = 0; i < nrep; i++)
               step(sc, rep[i]);
         }
         continue;
      }
      step(sc, p);
   }
   fclose(f);
   free(rep);
   return 0;
}

void gbalink_script_free(gbalink_script *sc)
{
   unsigned i;
   if (sc->mark)
      for (i = 0; i < sc->cap; i++)
         free(sc->mark[i]);
   free(sc->mark);
   free(sc->keys);
   free(sc->dump);
   memset(sc, 0, sizeof(*sc));
}
