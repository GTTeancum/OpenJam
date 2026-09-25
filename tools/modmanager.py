#!/usr/bin/env python3
"""NBA JAM Mod Manager - the window a player uses to install mods.

It ships as one executable that sits in the game folder and works on what is
around it: the game beside it, the mods under it, the saves next to those.
Installing a mod is picking the .pkg it was downloaded as. Everything after
that - opening the package, converting its artwork into the format this build
reads, building it a game folder of its own, giving it its own saves, and
putting it in the list the game shows on its main menu - happens on its own.

It is made to look like the game rather than like a tool: the same panel, the
same orange for whatever is loaded, and the game's own typefaces, which are
sitting in the `ui` folder beside it because the port needs them too.

The work runs on a thread of its own so the window keeps drawing, and it
reports back through a queue the window drains on a timer. Nothing outside
the main thread touches Tk.

Built with tools/build_manager.py, which wraps PyInstaller; `dlc.py stage`
copies the result into the game folder.
"""

import ctypes
import os
import queue
import sys
import threading
import traceback
from pathlib import Path

import tkinter as tk
from tkinter import filedialog, messagebox, simpledialog

if getattr(sys, "frozen", False):
    sys.path.insert(0, str(Path(sys._MEIPASS) / "tools"))
else:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
import modkit


class Relay:
    """Somewhere for print() to go in a window with no console behind it.

    A windowed build has no standard output at all - it is None - and the
    unpacker talks as it works, so without this the first thing it says takes
    the whole install down. What it says goes to the status line instead,
    which is where someone watching would want it anyway.
    """

    def __init__(self, sink):
        self.sink = sink
        self.pending = ""

    def write(self, text):
        self.pending += text
        while "\n" in self.pending:
            line, self.pending = self.pending.split("\n", 1)
            line = line.strip()
            if line:
                self.sink(line)
        return len(text)

    def flush(self):
        pass

TITLE = "NBA JAM: On Fire Edition - Mod Manager"

# The panel's colours, the same ones the game draws its mod list in.
INK = "#070c15"
PLATE = "#0b1220"
ROW = "#101c30"
ROW_ON = "#3a2812"
LINE = "#26364f"
TEXT = "#f2f5f8"
QUIET = "#93a5bd"
ORANGE = "#ffa23c"
BLUE = "#4a86e8"


def load_game_fonts(game):
    """Make the game's own typefaces available to this window.

    They are unpacked into `ui` beside the executable for the port's own use.
    Loading them privately means the window is set in the same faces as the
    menu it stands in for, and nothing is installed on the machine.
    """
    families = {}
    ui = Path(game) / "ui"
    for file, family in (("display.ttf", "EADsplyN"),
                         ("text.ttf", "Eurostile LT Std Demi")):
        path = ui / file
        if not path.is_file():
            continue
        try:
            added = ctypes.windll.gdi32.AddFontResourceExW(
                ctypes.c_wchar_p(str(path)), 0x10, 0)   # FR_PRIVATE
            if added:
                families[file] = family
        except Exception:
            pass
    return families


def game_folder_near(start):
    """The game folder, looking where the manager is most likely to sit."""
    here = Path(start).resolve()
    for candidate in (here, here.parent, here.parent.parent):
        if modkit.is_game_folder(candidate):
            return candidate
    return None


class Card(tk.Frame):
    """One installed game, drawn the way the in-game list draws it."""

    def __init__(self, parent, mod, fonts, on_pick, **kw):
        super().__init__(parent, bg=ROW, highlightthickness=2,
                         highlightbackground=BLUE, cursor="hand2", **kw)
        self.mod = mod
        self.on_pick = on_pick
        self.columnconfigure(0, weight=1)

        self.name = tk.Label(self, text=mod.get("name", mod["id"]).upper(),
                             bg=ROW, fg=TEXT, anchor="w", font=fonts["row"])
        self.name.grid(row=0, column=0, sticky="w", padx=14, pady=(9, 0))
        self.mark = tk.Label(self, text="", bg=ROW, fg=ORANGE, anchor="e",
                             font=fonts["tag"])
        self.mark.grid(row=0, column=1, sticky="e", padx=14, pady=(9, 0))

        bits = []
        if mod.get("version"):
            bits.append("Version " + mod["version"])
        if mod.get("author"):
            bits.append("by " + mod["author"])
        if mod.get("files"):
            bits.append("%s files changed" % mod["files"])
        if not bits:
            bits.append("The game as EA shipped it")
        self.detail = tk.Label(self, text="   ".join(bits), bg=ROW, fg=QUIET,
                               anchor="w", font=fonts["small"])
        self.detail.grid(row=1, column=0, columnspan=2, sticky="w", padx=14,
                         pady=(0, 9))

        for w in (self, self.name, self.detail, self.mark):
            w.bind("<Button-1>", self._clicked)
            w.bind("<Double-Button-1>", self._double)

    def _clicked(self, _event):
        self.on_pick(self.mod["id"], False)

    def _double(self, _event):
        self.on_pick(self.mod["id"], True)

    def show(self, selected, active):
        fill = ROW_ON if active else ROW
        edge = ORANGE if active else (TEXT if selected else BLUE)
        self.configure(bg=fill, highlightbackground=edge,
                       highlightthickness=3 if selected else 2)
        for w in (self.name, self.detail, self.mark):
            w.configure(bg=fill)
        self.mark.configure(text="LOADED" if active else "")


class Manager(tk.Tk):
    def __init__(self, game):
        super().__init__()
        self.game = Path(game)
        self.jobs = queue.Queue()
        self.working = False
        self.selected = None
        self.cards = {}

        self.title(TITLE)
        self.configure(bg=INK)
        self.geometry("760x560")
        self.minsize(680, 480)
        have = load_game_fonts(self.game)
        display = have.get("display.ttf", "Segoe UI Semibold")
        text = have.get("text.ttf", "Segoe UI")
        self.fonts = {
            "head": (display, 22),
            "sub": (text, 10),
            "row": (display, 15),
            "tag": (text, 9),
            "small": (text, 9),
            "button": (display, 12),
            "status": (text, 9),
        }
        self._build()
        self.refresh()
        self.after(80, self._drain)
        # Anything the working parts print belongs on the status line; in a
        # windowed build there is nowhere else for it to go.
        relay = Relay(lambda line: self.jobs.put(("say", line)))
        sys.stdout = relay
        sys.stderr = relay

    # ---------------------------------------------------------------- layout
    def _build(self):
        head = tk.Frame(self, bg=INK)
        head.pack(fill="x", padx=22, pady=(18, 0))
        tk.Label(head, text="MODS", bg=INK, fg=ORANGE,
                 font=self.fonts["head"]).pack(side="left")
        tk.Label(head, text=str(self.game), bg=INK, fg=QUIET,
                 font=self.fonts["sub"]).pack(side="right", pady=(10, 0))
        tk.Frame(self, bg=LINE, height=2).pack(fill="x", padx=22, pady=(6, 12))

        body = tk.Frame(self, bg=PLATE)
        body.pack(fill="both", expand=True, padx=22)
        self.canvas = tk.Canvas(body, bg=PLATE, highlightthickness=0, bd=0)
        self.canvas.pack(side="left", fill="both", expand=True)
        # A slim bar, and only when the list is longer than the window.
        self.scroll = tk.Scrollbar(body, orient="vertical", width=10,
                                   command=self.canvas.yview, bd=0,
                                   relief="flat", troughcolor=PLATE,
                                   bg=LINE, activebackground=ORANGE,
                                   highlightthickness=0)
        self.canvas.configure(yscrollcommand=self._scrolled)
        self.list = tk.Frame(self.canvas, bg=PLATE)
        self.window = self.canvas.create_window((0, 0), window=self.list,
                                                anchor="nw")
        self.list.bind("<Configure>", lambda _e: self.canvas.configure(
            scrollregion=self.canvas.bbox("all")))
        self.canvas.bind("<Configure>", lambda e: self.canvas.itemconfigure(
            self.window, width=e.width))
        self.canvas.bind_all("<MouseWheel>", self._wheel)

        tk.Frame(self, bg=LINE, height=2).pack(fill="x", padx=22, pady=(12, 0))
        feet = tk.Frame(self, bg=INK)
        feet.pack(fill="x", padx=22, pady=14)
        self.install = self._button(feet, "Install a mod", self.on_add,
                                    accent=True)
        self.remove = self._button(feet, "Remove", self.on_remove)
        self.default = self._button(feet, "Load this one", self.on_active)
        self.play = self._button(feet, "Play", self.on_play, side="right")

        self.status = tk.Label(self, text="", bg=INK, fg=QUIET, anchor="w",
                               font=self.fonts["status"])
        self.status.pack(fill="x", padx=24)
        self.bar = tk.Frame(self, bg=INK, height=4)
        self.bar.pack(fill="x", padx=22, pady=(4, 14))
        self.bar_fill = tk.Frame(self.bar, bg=ORANGE, height=4, width=0)
        self.bar_fill.place(x=0, y=0)
        self.say("Pick a mod you have downloaded and it will be installed for "
                 "you.")

    def _button(self, parent, text, command, accent=False, side="left"):
        b = tk.Button(parent, text=text, command=command, relief="flat",
                      bg=ORANGE if accent else ROW,
                      fg=INK if accent else TEXT,
                      activebackground="#ffb864" if accent else LINE,
                      activeforeground=INK if accent else TEXT,
                      font=self.fonts["button"], padx=18, pady=8, bd=0,
                      cursor="hand2", disabledforeground=QUIET)
        b.pack(side=side, padx=(0, 10) if side == "left" else (10, 0))
        return b

    def _wheel(self, event):
        if self.canvas.yview() != (0.0, 1.0):
            self.canvas.yview_scroll(-1 * (event.delta // 120), "units")

    def _scrolled(self, first, last):
        """Show the bar only when there is somewhere to scroll to."""
        if float(first) <= 0.0 and float(last) >= 1.0:
            self.scroll.pack_forget()
        else:
            self.scroll.pack(side="right", fill="y", padx=(6, 0))
        self.scroll.set(first, last)

    # ----------------------------------------------------------------- state
    def refresh(self):
        for card in self.cards.values():
            card.destroy()
        self.cards = {}
        active = modkit.active_id(self.game)
        mods = modkit.installed(self.game)
        for mod in mods:
            card = Card(self.list, mod, self.fonts, self.on_pick)
            card.pack(fill="x", pady=(0, 8))
            self.cards[mod["id"]] = card
        if self.selected not in self.cards:
            self.selected = active if active in self.cards else mods[0]["id"]
        self._paint(active)

    def _paint(self, active=None):
        active = active or modkit.active_id(self.game)
        for mod_id, card in self.cards.items():
            card.show(mod_id == self.selected, mod_id == active)

    def on_pick(self, mod_id, double):
        self.selected = mod_id
        self._paint()
        if double:
            self.on_active()

    def say(self, line):
        self.status.configure(text=line)

    def progress(self, done, total):
        width = self.bar.winfo_width() or 1
        self.bar_fill.configure(width=int(width * done / max(1, total)))

    def _drain(self):
        try:
            while True:
                kind, payload = self.jobs.get_nowait()
                if kind == "say":
                    self.say(payload)
                elif kind == "progress":
                    self.progress(*payload)
                elif kind == "done":
                    self.working = False
                    self.bar_fill.configure(width=0)
                    self.refresh()
                    for b in (self.install, self.remove, self.default,
                              self.play):
                        b.configure(state="normal")
                    if payload:
                        messagebox.showerror(TITLE, payload)
                    else:
                        self.say("Done.")
        except queue.Empty:
            pass
        self.after(80, self._drain)

    def _run(self, work):
        if self.working:
            return
        self.working = True
        for b in (self.install, self.remove, self.default, self.play):
            b.configure(state="disabled")

        def go():
            problem = ""
            try:
                work()
            except Exception as exc:
                problem = str(exc) or exc.__class__.__name__
                traceback.print_exc()
            self.jobs.put(("done", problem))

        threading.Thread(target=go, daemon=True).start()

    # --------------------------------------------------------------- actions
    def begin_install(self, package, name):
        """Install without asking anything. See main()."""
        def work():
            modkit.install(
                self.game, package, name=name,
                say=lambda s: self.jobs.put(("say", s)),
                progress=lambda d, t: self.jobs.put(("progress", (d, t))))
        self._run(work)

    def on_add(self):
        picked = filedialog.askopenfilename(
            parent=self, title="Pick the mod you downloaded",
            filetypes=[("PS3 mod package", "*.pkg"), ("All files", "*.*")])
        if not picked:
            return
        suggested = Path(picked).stem.replace("_", " ").replace("-", " ")
        name = simpledialog.askstring(
            TITLE, "What should this mod be called in the game's list?",
            initialvalue=suggested, parent=self)
        if not name:
            return

        def work():
            modkit.install(
                self.game, picked, name=name,
                say=lambda s: self.jobs.put(("say", s)),
                progress=lambda d, t: self.jobs.put(("progress", (d, t))))
        self._run(work)

    def on_remove(self):
        if not self.selected or self.selected == modkit.BASE:
            messagebox.showinfo(TITLE, "The base game cannot be removed.")
            return
        name = self.cards[self.selected].mod.get("name", self.selected)
        keep = messagebox.askyesnocancel(
            TITLE,
            "Remove %s?\n\nYes  -  remove it, keep its saves\n"
            "No   -  remove it and its saves too" % name, parent=self)
        if keep is None:
            return
        chosen = self.selected

        def work():
            modkit.remove(self.game, chosen, keep_saves=bool(keep),
                          say=lambda s: self.jobs.put(("say", s)))
        self._run(work)

    def on_active(self):
        if not self.selected:
            return
        modkit.set_active(self.game, self.selected)
        name = self.cards[self.selected].mod.get("name", self.selected)
        self.say("The game will load %s." % name)
        self._paint()

    def on_play(self):
        try:
            modkit.play(self.game)
            self.say("Starting the game.")
        except Exception as exc:
            messagebox.showerror(TITLE, str(exc), parent=self)


def main():
    # Packaged, it sits in the game folder. Run from the source tree it has to
    # be told, or it looks for the staged game beside the port.
    #
    # `--install <package> [--name <name>]` starts an install the moment the
    # window opens, with no dialogs in the way. That is not for players: it is
    # how the built executable gets tested doing the real thing, rather than
    # the same code being tested through an interpreter that has a console.
    argv = [a for a in sys.argv[1:]]
    package = name = None
    if "--install" in argv:
        at = argv.index("--install")
        package = argv[at + 1] if len(argv) > at + 1 else None
        del argv[at:at + 2]
    if "--name" in argv:
        at = argv.index("--name")
        name = argv[at + 1] if len(argv) > at + 1 else None
        del argv[at:at + 2]

    root = None
    if argv and modkit.is_game_folder(argv[0]):
        root = Path(argv[0])
    if root is None:
        start = (Path(sys.executable).parent if getattr(sys, "frozen", False)
                 else Path(__file__).resolve().parents[2])
        root = game_folder_near(start)
    if root is None and not getattr(sys, "frozen", False):
        for sibling in Path(__file__).resolve().parents[2].iterdir():
            if sibling.is_dir() and modkit.is_game_folder(sibling):
                root = sibling
                break
    if root is None:
        picker = tk.Tk()
        picker.withdraw()
        messagebox.showinfo(
            TITLE, "Point me at the game folder - the one with "
                   "nbajam_ofe.exe in it.")
        picked = filedialog.askdirectory(title="Where is the game?")
        picker.destroy()
        if not picked or not modkit.is_game_folder(picked):
            return 1
        root = Path(picked)
    window = Manager(root)
    if package:
        window.after(300, lambda: window.begin_install(package, name or "Mod"))
    window.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
