#!/usr/bin/env python3
"""mkscript.py ROLE SCENARIO -- the `ui_browser_script` each console of
tools/share/run_share_ppsspp.sh runs (docs/SAVE-SHARE.md).

A script line is `<frame> press <BUTTON>`, `<frame> dump <name>` or
`<frame> exit`; frames count the browser's own loop iterations, which stop
while something blocks (the 10 s room scan, the radio coming up), so the
gaps below only have to cover what happens INSIDE a screen: a transfer, a
thumbnail arriving, the console flare.  They are generous on purpose.

R on the game list opens its Wireless screen.  Rows from the cursor's
start: Room, Share game data, Join: scan for rooms, Ad hoc channel, Fast
link (docs/design/UI-OVERHAUL.md).  Builds before the 5.1 menu overhaul
opened these rows from START's Settings (Room, Link mode, Session overlay,
Share game data, Join: scan for rooms) and need this file from before it."""
import sys


class S:
    def __init__(self):
        self.t = 0
        self.out = []

    def wait(self, n):
        self.t += n
        return self

    def press(self, b, gap=12):
        self.t += gap
        self.out.append('%d press %s' % (self.t, b))
        return self

    def dump(self, name, gap=6):
        self.t += gap
        self.out.append('%d dump %s' % (self.t, name))
        return self

    def exit(self, gap=30):
        self.t += gap
        self.out.append('%d exit' % self.t)
        return self


def sharer(scen):
    s = S()
    s.wait(60).press('R').wait(20)   # the game list's Wireless screen
    s.press('DOWN')                  # Room -> Share game data
    s.dump('a1_settings', 20)
    s.press('CROSS')                 # scan the stick, start the room
    s.dump('a2_waiting', 90)
    # while the other console works: a picture every 5 s
    for k in range(8 if scen in ('old', 'ingame') else 16):
        s.dump('a3_live_%02d' % k, 300)
    s.press('CIRCLE').dump('a4_settings_again', 60)
    s.press('CIRCLE').dump('a5_browser', 60)
    s.exit()
    return s.out


def join(s):
    """From the browser to the shared-data list."""
    s.wait(60).press('R').wait(20)   # the game list's Wireless screen
    for _ in range(2):               # Room -> Share game data -> Join: scan
        s.press('DOWN')
    s.dump('b01_settings', 20)
    s.press('CROSS')                 # the room list (blocks 10 s scanning)
    s.dump('b02_rooms', 60)
    s.press('CROSS')                 # join Room 7: the menu opens by itself
    s.dump('b03_list', 300)


def guest(scen):
    s = S()
    join(s)
    if scen == 'full':
        for _ in range(3):
            s.press('DOWN')          # AW2, Golden Sun, Mario Kart, Emerald
        s.dump('b04_list_emerald', 60)
        s.press('CROSS')             # straight to Their states
        s.dump('b05_their_slot1', 200)
        s.press('DOWN').dump('b06_their_slot2', 150)
        s.press('CROSS')             # Your slots
        s.press('DOWN').dump('b07_your_slots', 60)
        s.press('CROSS')             # their Slot 2 -> our Slot 3
        s.dump('b08_state_transfer', 30)
        s.dump('b09_state_got', 600)
        s.press('UP').press('UP')    # the Save file row
        s.dump('b10_save_row', 20)
        s.dump('b10b_save_row_icon', 40)
        s.press('CROSS').dump('b11_save_question', 20)
        s.press('CROSS').dump('b12_save_transfer', 14)
        s.dump('b13_save_replaced', 300)
        s.press('CIRCLE').dump('b14_list_marks', 60)
        s.press('TRIANGLE').dump('b15_flare', 11).dump('b16_flare_caption', 11)
        s.dump('b17_gb_list', 150)
        s.press('CROSS').dump('b18_gb_their', 200)
        s.press('CIRCLE').wait(30)
        s.press('TRIANGLE').dump('b19_gbc_empty', 200)
        s.press('TRIANGLE').dump('b20_gba_again', 200)
        s.press('CIRCLE')            # leave the room
        s.dump('b21_browser', 120)
    elif scen in ('old', 'ingame'):
        return ['1 dump unused']
    elif scen == 'drop':
        for _ in range(3):
            s.press('DOWN')
        s.press('CROSS').dump('b05_their', 400)
        s.press('DOWN').press('CROSS')       # Slot 2 -> Your slots
        s.press('CROSS')                     # ... into our Slot 2
        s.dump('b08_transfer', 60)
        # the sharer is killed here; LOST_AFTER is 6 s
        s.dump('b09_lost', 700)
        s.press('CROSS')                     # try again: still nobody
        s.dump('b10_still_lost', 300)
        s.wait(900)
        s.press('CIRCLE').dump('b11_list', 60)
        s.press('CIRCLE')
        s.dump('b12_browser', 200)
    elif scen == 'liar':
        for _ in range(3):
            s.press('DOWN')
        s.press('CROSS').dump('b05_their', 400)
        s.press('DOWN').press('DOWN')        # Slot 3: not a state
        s.press('CROSS')                     # Your slots
        s.press('CROSS')                     # ... into our Slot 3 (taken)
        s.dump('b08_transfer', 30)
        s.dump('b09_refused', 900)
        s.press('CIRCLE').dump('b10_back', 30)
        s.press('CIRCLE').press('CIRCLE')
        s.dump('b11_browser', 200)
    elif scen == 'mismatch':
        for _ in range(3):
            s.press('DOWN')
        s.dump('b04_list_mismatch', 60)
        s.press('CROSS').dump('b05_their_greyed', 120)
        s.press('CROSS').wait(30)            # the save question (has a save)
        s.press('CIRCLE').dump('b06_kept_mine', 30)
        s.press('CIRCLE')                    # back to the list
        s.press('UP').press('UP')            # Golden Sun: no save here
        s.press('CROSS').dump('b07_goldensun', 120)
        s.press('CROSS').dump('b08_save_no_question', 20)
        s.dump('b09_saved', 300)
        s.press('CIRCLE').press('CIRCLE')
        s.dump('b10_browser', 200)
    elif scen == 'full_stick':
        for _ in range(3):
            s.press('DOWN')
        s.press('CROSS').wait(200)
        s.press('UP')                        # Slot 1 -> the Save file row
        s.press('CROSS').wait(20).press('CROSS')
        s.dump('b08_write_failed', 300)
        s.press('CIRCLE').dump('b09_back', 30)
        s.press('CIRCLE').press('CIRCLE')
        s.dump('b10_browser', 200)
    s.exit()
    return s.out


if __name__ == '__main__':
    role, scen = sys.argv[1], sys.argv[2]
    print('\n'.join(sharer(scen) if role == 'sharer' else guest(scen)))
