/*--------------------------------------------------
   TGB Dual - Gameboy Emulator -
   Copyright (C) 2001  Hii

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

//-----------------------------------------
// Œ^’è‹`•”

#ifndef _GB_TYPES
#define _GB_TYPES

#include <stdint.h>

typedef unsigned char byte;
typedef unsigned short word;
/* GBAdhoc: was `unsigned long`, which is 64-bit on LP64 hosts; the core
 * relies on 32-bit wrap (sound phase accumulators, tile row words). */
typedef uint32_t dword;


#define false 0
#define true 1

#endif
