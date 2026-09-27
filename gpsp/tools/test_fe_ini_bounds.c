#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../frontend-common/fe_util.c"

int main(void)
{
  char path[] = "/tmp/gbadhoc-ini-XXXXXX";
  char value[4401];
  char actual[64] = {0};
  int fd = mkstemp(path);
  FILE *file;
  assert(fd >= 0);
  file = fdopen(fd, "w");
  assert(file);
  assert(fputs("theme = original\nother = keep\n", file) >= 0);
  assert(fclose(file) == 0);

  memset(value, 'x', sizeof(value) - 1);
  value[sizeof(value) - 1] = '\0';
  assert(fe_ini_set(path, "theme", value) == -1);

  file = fopen(path, "r");
  assert(file);
  assert(fgets(actual, sizeof(actual), file));
  assert(strcmp(actual, "theme = original\n") == 0);
  assert(fgets(actual, sizeof(actual), file));
  assert(fclose(file) == 0);
  assert(strcmp(actual, "other = keep\n") == 0);
  assert(unlink(path) == 0);
  puts("PASS oversized INI update is refused without truncating the existing file");
  return 0;
}
