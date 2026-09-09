"""HeroForge -- box art and marquee wallpapers for GBAdhoc.

Point it at your ROM folder.  It works out which games you have, pulls the
matching cover from the public libretro-thumbnails archive, and produces two
things per game:

    boxart/<rom name>.png   the cover, for the Shelf shell
    hero/<rom name>.png     a 480x272 wallpaper composed for the Marquee shell

Then copy both folders into PSP/GAME/GBAdhoc/ on your memory stick.

It only fetches art for games already in your folder.  It does not find,
link to or download games.

Requires Python 3.8+ and Pillow:   pip install pillow
Run:                               python heroforge.py
Headless:                          python heroforge.py --roms <dir> --out <dir>
"""
import os
import sys
import threading
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import library                                   # noqa: E402


def build(rom_dir, out_dir, log, want_hero=True, want_box=True, stop=None):
    """Do the work.  `log(str)` receives progress; `stop()` may cancel."""
    try:
        import compose
    except ImportError:
        log("Pillow is required:  pip install pillow")
        return 0, 0

    box_dir = os.path.join(out_dir, "boxart")
    hero_dir = os.path.join(out_dir, "hero")
    os.makedirs(box_dir, exist_ok=True)
    os.makedirs(hero_dir, exist_ok=True)

    log("Reading the thumbnail index ...")
    names = library.thumbnail_index()
    log("  %d covers available" % len(names))

    log("Matching your ROM folder ...")
    rows = library.match_roms(rom_dir, names)
    have = [r for r in rows if r[1]]
    miss = [r for r in rows if not r[1]]
    log("  %d of %d games matched" % (len(have), len(rows)))
    for fn, _, sc in miss:
        log("  no cover found: %s" % fn)

    done = fail = 0
    for i, (fn, thumb, score) in enumerate(have, 1):
        if stop and stop():
            log("Cancelled.")
            break
        stem = os.path.splitext(fn)[0]
        box = os.path.join(box_dir, stem + ".png")
        try:
            if not os.path.exists(box):
                library.fetch_cover(thumb, box)
            if want_hero:
                compose.compose(box, os.path.join(hero_dir, stem + ".png"))
            if not want_box:
                os.remove(box)
            done += 1
            log("  [%d/%d] %s" % (i, len(have), stem))
        except Exception as e:                    # keep going: one bad cover
            fail += 1                             # must not end the batch
            log("  [%d/%d] FAILED %s (%s)" % (i, len(have), stem, e))
    log("")
    log("Done: %d built, %d failed, %d unmatched." % (done, fail, len(miss)))
    log("Copy 'boxart' and 'hero' into PSP/GAME/GBAdhoc/ on your memory stick.")
    return done, fail


# ---------------------------------------------------------------- UI --------
def gui():
    import tkinter as tk
    from tkinter import filedialog, ttk

    root = tk.Tk()
    root.title("HeroForge -- GBAdhoc art builder")
    root.geometry("720x520")
    root.configure(bg="#111114")

    fg, bg, mid = "#e8e8e8", "#111114", "#1d1d22"
    st = ttk.Style()
    st.theme_use("clam")
    st.configure("TButton", background=mid, foreground=fg, borderwidth=0,
                 focuscolor=mid, padding=8)
    st.map("TButton", background=[("active", "#2a2a31")])
    st.configure("TCheckbutton", background=bg, foreground=fg)

    rom_v = tk.StringVar()
    out_v = tk.StringVar(value=os.path.join(os.path.expanduser("~"),
                                            "Desktop", "GBAdhoc-art"))
    hero_v = tk.BooleanVar(value=True)
    box_v = tk.BooleanVar(value=True)

    def row(label, var, browse_title):
        f = tk.Frame(root, bg=bg)
        f.pack(fill="x", padx=18, pady=(14, 0))
        tk.Label(f, text=label, bg=bg, fg="#8c8c8c",
                 font=("Segoe UI", 9)).pack(anchor="w")
        g = tk.Frame(f, bg=bg)
        g.pack(fill="x", pady=(4, 0))
        e = tk.Entry(g, textvariable=var, bg=mid, fg=fg, relief="flat",
                     insertbackground=fg, font=("Segoe UI", 10))
        e.pack(side="left", fill="x", expand=True, ipady=6)
        ttk.Button(g, text="Browse",
                   command=lambda: var.set(
                       filedialog.askdirectory(title=browse_title) or var.get())
                   ).pack(side="left", padx=(8, 0))

    tk.Label(root, text="HeroForge", bg=bg, fg=fg,
             font=("Segoe UI Semibold", 20)).pack(anchor="w", padx=18, pady=(18, 0))
    tk.Label(root, text="Cover art and Marquee wallpapers for the games you own.",
             bg=bg, fg="#8c8c8c", font=("Segoe UI", 9)).pack(anchor="w", padx=18)

    row("ROM folder", rom_v, "Select your ROM folder")
    row("Output folder", out_v, "Where to write boxart/ and hero/")

    opts = tk.Frame(root, bg=bg)
    opts.pack(fill="x", padx=18, pady=(14, 0))
    ttk.Checkbutton(opts, text="Box art (Shelf shell)", variable=box_v).pack(side="left")
    ttk.Checkbutton(opts, text="Wallpapers (Marquee shell)", variable=hero_v).pack(side="left", padx=(18, 0))

    out = tk.Text(root, bg="#0b0b0e", fg="#c9c9c9", relief="flat",
                  font=("Consolas", 9), height=14)
    out.pack(fill="both", expand=True, padx=18, pady=14)

    cancel = {"stop": False}

    def log(msg):
        out.insert("end", msg + "\n")
        out.see("end")
        root.update_idletasks()

    def run():
        rd = rom_v.get().strip()
        if not os.path.isdir(rd):
            log("Pick a ROM folder first.")
            return
        cancel["stop"] = False
        go.state(["disabled"])
        out.delete("1.0", "end")

        def worker():
            try:
                build(rd, out_v.get().strip(), log, hero_v.get(), box_v.get(),
                      lambda: cancel["stop"])
            except Exception:
                log(traceback.format_exc())
            finally:
                go.state(["!disabled"])
        threading.Thread(target=worker, daemon=True).start()

    bar = tk.Frame(root, bg=bg)
    bar.pack(fill="x", padx=18, pady=(0, 16))
    go = ttk.Button(bar, text="Build art", command=run)
    go.pack(side="left")
    ttk.Button(bar, text="Stop",
               command=lambda: cancel.__setitem__("stop", True)).pack(side="left", padx=8)

    root.mainloop()


def main():
    args = sys.argv[1:]
    if "--roms" in args:
        rd = args[args.index("--roms") + 1]
        od = args[args.index("--out") + 1] if "--out" in args else "GBAdhoc-art"
        build(rd, od, print)
    else:
        gui()


if __name__ == "__main__":
    main()
