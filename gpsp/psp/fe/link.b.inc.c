/* ---- ADR-0069: THE SCREENSHOT CHORD --------------------------------------
 * L+R+SELECT asks the loop for a screenshot (it replaced the old marker-file
 * poll, whose sceIoGetstat blocked for 12.4 ms).  An "As displayed"
 * screenshot that waits for a clean picture asks the loop to serve the
 * chord again next time (player_screenshot). */
static int      g_dump_chord_pending;

static void shot_ask_again(void)
{
   g_dump_chord_pending = 1;
}

/* The emulation thread's UID, taken at boot (emu_prio) on that thread; the
 * stall watchdog reads its run status. */
static SceUID   g_emu_thid = -1;

