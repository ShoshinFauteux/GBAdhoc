/* Copyright (c) the gpsp contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef FE_CONSOLE_H
#define FE_CONSOLE_H

/* Selected hardware family for ROM browsing and core dispatch. Keep the GBA
 * value zero so zero-initialized frontend configs preserve current behavior. */
typedef enum fe_console
{
  FE_CONSOLE_GBA = 0,
  FE_CONSOLE_GB,
  FE_CONSOLE_GBC,
  FE_CONSOLE_COUNT
} fe_console_t;

#endif
