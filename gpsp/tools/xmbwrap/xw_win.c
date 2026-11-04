/* xw_win.c -- GBAdhoc XMB Entry Maker, the Windows app (issue #29,
 * docs/XMB-LAUNCH.md).  One window over xw_core:
 *
 *   1. Game: pick the ROM in your PSP's GBAdhoc roms folder (PSP in USB
 *      mode).  The card, GBAdhoc's folder and the ROM's place are read from
 *      that path; the title and GBAdhoc's own art for it are filled in.
 *   2. Title, icon and background can be changed (any PNG/JPEG/BMP; the
 *      icon is fitted into 144x80, the background fills 480x272).
 *   3. Create: writes PSP/GAME/<title>/ on the PSP.  Nothing is copied but
 *      the entry itself: the game stays in GBAdhoc's roms folder. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <string.h>
#include "xw_core.h"
#include "launcher_pbp.h"

enum { ID_ROM = 100, ID_ROM_BR, ID_TITLE, ID_ICON, ID_ICON_BR, ID_BG, ID_BG_BR,
       ID_MAKE, ID_STATUS, ID_WHERE };

static HWND g_wnd, g_rom, g_title, g_icon, g_bg, g_status, g_where;
static HFONT g_font;

static void to_utf8(const wchar_t *w, char *out, int cap)
{
   if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL))
      out[0] = '\0';
}
static void from_utf8(const char *s, wchar_t *out, int cap)
{
   if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap))
      out[0] = 0;
}
static void get_utf8(HWND h, char *out, int cap)
{
   wchar_t w[1024];
   GetWindowTextW(h, w, 1024);
   to_utf8(w, out, cap);
}
static void set_utf8(HWND h, const char *s)
{
   wchar_t w[1024];
   from_utf8(s, w, 1024);
   SetWindowTextW(h, w);
}
static void status(const char *s)
{
   set_utf8(g_status, s);
}

static int pick_file(HWND owner, const wchar_t *filter, wchar_t *out, int cap)
{
   OPENFILENAMEW of;
   memset(&of, 0, sizeof(of));
   of.lStructSize = sizeof(of);
   of.hwndOwner = owner;
   of.lpstrFilter = filter;
   of.lpstrFile = out;
   of.nMaxFile = (DWORD)cap;
   of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
   return GetOpenFileNameW(&of);
}

/* The ROM changed: infer where things are, fill the title and the art. */
static void rom_changed(void)
{
   char rom[1024], root[1024], gb[512], rel[1024], t[256], hero[1200], box[1200];
   char install[1600], where[2400];
   get_utf8(g_rom, rom, sizeof(rom));
   if (!rom[0])
      return;
   if (xw_infer(rom, root, sizeof(root), gb, sizeof(gb), rel, sizeof(rel)) != 0)
   {
      set_utf8(g_where, "");
      status("Pick the game from the roms folder of GBAdhoc on your PSP "
             "(connect it in USB mode first).");
      return;
   }
   xw_title_from_rom(rel, t, sizeof(t));
   set_utf8(g_title, t);
   snprintf(install, sizeof(install), "%s%s", root, gb);
   xw_default_art(install, rel, hero, sizeof(hero), box, sizeof(box));
   set_utf8(g_icon, box);
   set_utf8(g_bg, hero);
   snprintf(where, sizeof(where), "GBAdhoc: %s%s     game: roms/%s", root, gb, rel);
   set_utf8(g_where, where);
   status(box[0] || hero[0] ? "GBAdhoc's own art for this game is filled in. "
                              "Change anything, then Create."
                            : "No GBAdhoc art for this game: pick an icon and a "
                              "background, or leave them empty.");
}

static void make(void)
{
   char rom[1024], root[1024], gb[512], rel[1024], title[512], icon[1024], bg[1024];
   char made[1100], err[1400], msg[1600];
   xw_entry e;
   get_utf8(g_rom, rom, sizeof(rom));
   get_utf8(g_title, title, sizeof(title));
   get_utf8(g_icon, icon, sizeof(icon));
   get_utf8(g_bg, bg, sizeof(bg));
   if (!rom[0] || xw_infer(rom, root, sizeof(root), gb, sizeof(gb), rel, sizeof(rel)) != 0)
   {
      status("Pick the game from the roms folder of GBAdhoc on your PSP first.");
      return;
   }
   if (!title[0])
   {
      status("Give the entry a title.");
      return;
   }
   memset(&e, 0, sizeof(e));
   e.template_pbp = xw_launcher_pbp;
   e.template_len = xw_launcher_pbp_len;
   e.title = title;
   {
      static char psp_rel[1024];
      xw_rom_for_psp(rom, rel, psp_rel, sizeof(psp_rel));   /* 8.3 if non-ASCII */
      e.rom = psp_rel;
   }
   e.gbadhoc = gb;
   e.icon_image = icon[0] ? icon : NULL;
   e.bg_image = bg[0] ? bg : NULL;
   e.root = root;
   SetCursor(LoadCursor(NULL, IDC_WAIT));
   if (xw_make_entry(&e, made, sizeof(made), err, sizeof(err)) != 0)
   {
      snprintf(msg, sizeof(msg), "Not made: %s", err);
      status(msg);
      return;
   }
   snprintf(msg, sizeof(msg), "Made %s. Eject the PSP and look for \"%s\" in Game.",
            made, title);
   status(msg);
}

static HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y,
                int w, int h, int id)
{
   HWND c = CreateWindowExW(!wcscmp(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text,
                            WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd,
                            (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
   SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
   return c;
}

static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
   static const wchar_t *rom_f = L"Game Boy games (*.gba;*.gb;*.gbc)\0*.gba;*.gb;*.gbc\0All files\0*.*\0";
   static const wchar_t *img_f = L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.gif)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0All files\0*.*\0";
   wchar_t f[1024];
   switch (m)
   {
   case WM_CREATE:
   {
      int y = 16;
      g_wnd = w;
      add(L"STATIC", L"1. Game (in GBAdhoc's roms folder on your PSP):", 0, 16, y, 560, 20, 0);
      y += 22;
      g_rom = add(L"EDIT", L"", ES_AUTOHSCROLL, 16, y, 470, 24, ID_ROM);
      add(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 494, y, 90, 24, ID_ROM_BR);
      y += 28;
      g_where = add(L"STATIC", L"", 0, 16, y, 568, 20, ID_WHERE);
      y += 30;
      add(L"STATIC", L"2. Title in the XMB:", 0, 16, y, 560, 20, 0);
      y += 22;
      g_title = add(L"EDIT", L"", ES_AUTOHSCROLL, 16, y, 568, 24, ID_TITLE);
      y += 34;
      add(L"STATIC", L"Icon (fitted into 144 x 80; empty = the XMB's default):", 0, 16, y, 560, 20, 0);
      y += 22;
      g_icon = add(L"EDIT", L"", ES_AUTOHSCROLL, 16, y, 470, 24, ID_ICON);
      add(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 494, y, 90, 24, ID_ICON_BR);
      y += 34;
      add(L"STATIC", L"Background (fills 480 x 272; empty = none):", 0, 16, y, 560, 20, 0);
      y += 22;
      g_bg = add(L"EDIT", L"", ES_AUTOHSCROLL, 16, y, 470, 24, ID_BG);
      add(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 494, y, 90, 24, ID_BG_BR);
      y += 40;
      add(L"BUTTON", L"3. Create XMB entry", BS_DEFPUSHBUTTON, 16, y, 200, 30, ID_MAKE);
      y += 42;
      g_status = add(L"STATIC", L"Connect your PSP in USB mode, then pick a game.", 0,
                     16, y, 568, 40, ID_STATUS);
      return 0;
   }
   case WM_COMMAND:
      switch (LOWORD(wp))
      {
      case ID_ROM_BR:
         f[0] = 0;
         if (pick_file(w, rom_f, f, 1024))
         {
            SetWindowTextW(g_rom, f);
            rom_changed();
         }
         return 0;
      case ID_ROM:
         if (HIWORD(wp) == EN_KILLFOCUS)
            rom_changed();
         return 0;
      case ID_ICON_BR:
         f[0] = 0;
         if (pick_file(w, img_f, f, 1024))
            SetWindowTextW(g_icon, f);
         return 0;
      case ID_BG_BR:
         f[0] = 0;
         if (pick_file(w, img_f, f, 1024))
            SetWindowTextW(g_bg, f);
         return 0;
      case ID_MAKE:
         make();
         return 0;
      }
      break;
   case WM_CTLCOLORSTATIC:
      SetBkMode((HDC)wp, TRANSPARENT);
      return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
   case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
   }
   return DefWindowProcW(w, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
   WNDCLASSW wc;
   MSG msg;
   RECT r = { 0, 0, 600, 400 };
   (void)prev; (void)cmd;
   g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                        CLEARTYPE_QUALITY, 0, L"Segoe UI");
   memset(&wc, 0, sizeof(wc));
   wc.lpfnWndProc = proc;
   wc.hInstance = inst;
   wc.hCursor = LoadCursor(NULL, IDC_ARROW);
   wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
   wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1));
   wc.lpszClassName = L"GBAdhocXmbMaker";
   RegisterClassW(&wc);
   AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
   CreateWindowW(L"GBAdhocXmbMaker", L"GBAdhoc XMB Entry Maker",
                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
                 CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                 NULL, NULL, inst, NULL);
   while (GetMessageW(&msg, NULL, 0, 0) > 0)
   {
      if (!IsDialogMessageW(g_wnd, &msg))
      {
         TranslateMessage(&msg);
         DispatchMessageW(&msg);
      }
   }
   return 0;
}
