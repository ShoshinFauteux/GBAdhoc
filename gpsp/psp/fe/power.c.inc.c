/* THE IN-GAME MENU OPENS OVER THE GAME (docs/UI-OVERLAY.md).  The same
 * snapshot the wake overlay sits on is taken here -- the core is paused from
 * this frame on, so it is the picture the player was looking at -- and the
 * menu draws it, scrimmed, behind every screen until it closes.  No new
 * memory: g_wake_frame is the netdrv arena's tail.  If it does not exist (the
 * arena reservation failed) the menu falls back to a flat page. */
static void menu_open(void)
{
   wake_snapshot();
   ui_set_backdrop(g_wake_have ? g_wake_frame : NULL,
                   (int)g_wake_w, (int)g_wake_h);
   ui_open();
}

