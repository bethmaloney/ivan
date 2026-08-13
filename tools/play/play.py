#!/usr/bin/env python3
"""Drive IVAN headlessly, one keystroke at a time, and review the result.

A --replay run is deterministic and ends the moment the recording is
exhausted, so a session is not a live process: it is a list of keys. Every
command here rewrites that list, replays it from the start under a pinned
seed, and captures the screen the game came to rest on. Replaying from scratch
rather than keeping a process alive is what makes the session reproducible,
resumable after a crash and safe to rewind - `undo` is just a shorter list.

Cost of that choice: each command re-runs world generation, about a second and
a half. Worth it for a reviewable oracle.

  play.py new  [--seed N] [--start]   begin a session
  play.py send <key>...               append keys, replay, capture
  play.py auto [turns]                let the wizard auto-play AI take turns
  play.py undo [n]                    drop the last n keys, replay, capture
  play.py show                        replay the current list and capture
  play.py rec                         print the recording
  play.py keys                        list the key names send accepts

Every command that captures prints the text layer of the final screen and the
path of the PNG beside it. The PNG is the map; the text layer is the panel,
the message log, the menus and the prompts.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_SESSION = os.path.join(REPO, "build", "play-session")
DATA_DIRS = ("Graphics", "Script", "Music", "Sound")

# From FeLib/Include/felibdef.h. The eight directions are the numpad layout
# IVAN actually uses: the arrow and page keys double as the diagonals.
#
# Deliberately no one letter direction aliases. IVAN's own commands are single
# letters, and a bare single character has to mean that character so that `>`,
# `?` and `S` work as typed - which would make `e` the eat command rather than
# east. Naming them at all would be a trap, so the short names stop at two
# characters, where nothing collides.
KEYS = {
    "bs": 0x08, "backspace": 0x08,
    "enter": 0x0D, "return": 0x0D,
    "esc": 0x1B, "escape": 0x1B,
    "space": 0x20,
    "home": 0x147, "nw": 0x147, "northwest": 0x147,
    "up": 0x148, "north": 0x148,
    "pgup": 0x149, "ne": 0x149, "northeast": 0x149,
    "left": 0x14B, "west": 0x14B,
    "right": 0x14D, "east": 0x14D,
    "end": 0x14F, "sw": 0x14F, "southwest": 0x14F,
    "down": 0x150, "south": 0x150,
    "pgdn": 0x151, "se": 0x151, "southeast": 0x151,
    "del": 0x152, "delete": 0x152,
    "ins": 0x153, "insert": 0x153,
    "wait": 0x02, "numpad5": 0x02,
}

# Commands worth knowing about when driving from the outside, so that `keys`
# is a usable crib sheet rather than just a keycode table. From
# Main/Source/command.cpp.
COMMANDS = [
    ("?",  "list all commands"),
    (">",  "enter an area, or go down stairs"),
    ("<",  "go up stairs"),
    (".",  "rest / wait a turn"),
    ("i",  "inventory"),
    (",",  "pick up"),
    ("d",  "drop"),
    ("e",  "eat"),
    ("q",  "quaff"),
    ("r",  "read"),
    ("W",  "wear"),
    ("w",  "wield"),
    ("a",  "apply"),
    ("o",  "open"),
    ("c",  "close"),
    ("l",  "look"),
    ("S",  "save and quit"),
    ("Q",  "quit without saving"),
    ("~",  "wizard mode auto play (hold ESC to stop)"),
]


# Four ENTERs is exactly character creation: pick the default character, accept
# the name FantasyName() pre-filled, and dismiss the two intro screens. A fifth
# lands on the world map, where ENTER is not a command and the game answers
# "Unknown key" without spending a turn - harmless, but it makes the message log
# harder to read, so the prelude stops at four.
START_KEYS = ["enter*4"]

# Engaging the wizard auto-play AI, which is not just `~`. `~` is AutoPlay and
# is WizardModeFunction-gated, so wizard mode has to be *runtime* activated
# first — that is backtick (`command.cpp:141`), which asks "Do you want to
# cheat, cheater? [y/N]" and needs the y. Then `~` raises AutoPlayMode to 1.
#
# From there the AI acts once per `.`. It is not self-driving: AutoPlayModeApply
# computes a key timeout for modes 2-4 and then throws it away — `SetKeyTimeout`
# has no callers anywhere in the tree. Which is a gift for this harness, because
# it means auto-play advances on input rather than on the wall clock.
#
# Mode 1 specifically, and not more presses of `~`: at mode >= 2 any key that is
# not `.` or `~` disables auto-play again (`char.cpp:3819`).
WIZARD_KEYS = ["`", "y", "~"]


def die(message):
    print("play: " + message, file=sys.stderr)
    sys.exit(2)


class Session:
    """The key list plus the scratch directory the game is run inside.

    The game is a portable build, so it writes its config, saves and bone
    files into the directory it is launched from. Launching it inside the
    session directory - with the read only data directories symlinked in -
    keeps all of that out of the repository, and means deleting the session
    directory really does reset everything.
    """

    def __init__(self, path):
        self.path = os.path.abspath(path)
        self.state_file = os.path.join(self.path, "session.json")
        self.rec = os.path.join(self.path, "session.rec")
        self.png = os.path.join(self.path, "screen.png")
        self.txt = os.path.join(self.path, "screen.txt")
        self.text_log = os.path.join(self.path, "text.log")
        self.trace = os.path.join(self.path, "trace.jsonl")
        self.run_log = os.path.join(self.path, "run.log")
        self.frames = os.path.join(self.path, "frames")
        self.seed = 999
        self.keys = []
        self.autoplay = False

    def load(self):
        if not os.path.exists(self.state_file):
            die("no session at %s; run `play.py new` first" % self.path)

        with open(self.state_file) as f:
            state = json.load(f)

        self.seed = state["seed"]
        self.keys = state["keys"]
        self.autoplay = state.get("autoplay", False)
        return self

    def save(self):
        with open(self.state_file, "w") as f:
            json.dump({"seed": self.seed, "keys": self.keys,
                       "autoplay": self.autoplay}, f, indent=1)

    def create(self, seed):
        os.makedirs(self.path, exist_ok=True)
        self.seed = seed
        self.keys = []
        self.autoplay = False

        for name in DATA_DIRS:
            source = os.path.join(REPO, name)

            if not os.path.isdir(source):
                die("%s is not in %s; run this from a built checkout" %
                    (name, REPO))

            link = os.path.join(self.path, name)

            if not os.path.exists(link):
                os.symlink(source, link)

        self.save()

    def write_rec(self):
        """The seq column is an integrity check the harness enforces, and the
        trailer is what tells it the recording is complete rather than cut
        short, so both are regenerated from scratch every time."""

        with open(self.rec, "w") as f:
            f.write("# written by tools/play/play.py\n")
            f.write("ivan-record 1 seed=%d ivan=0.59\n" % self.seed)

            for index, key in enumerate(self.keys, 1):
                f.write("K %d 0 0 %d\t# %s\n" % (index, key, describe(key)))

            f.write("# end keys=%d\n" % len(self.keys))


def describe(key):
    for name, code in KEYS.items():
        if code == key and len(name) > 1:
            return name

    if 0x20 <= key < 0x7F:
        return "'%s'" % chr(key)

    return "0x%X" % key


def parse_keys(tokens):
    """Turns a command line into key codes.

    A bare single character is that character, so `> ? S` work as written. A
    longer token is a name, `text:foo` types foo a character at a time, and
    `0x150` or `336` is a raw code for anything without a name. `x*3` repeats.
    """

    out = []

    for token in tokens:
        repeat = 1

        if "*" in token and not token.startswith("text:"):
            token, _, count = token.rpartition("*")

            if not count.isdigit() or not token:
                die("cannot read the repeat count in %r" % (token + "*" + count))

            repeat = int(count)

        if token.startswith("text:"):
            codes = [ord(c) for c in token[len("text:"):]]

            if any(c > 0xFF for c in codes):
                die("text: takes plain ASCII only")
        elif len(token) == 1:
            codes = [ord(token)]
        elif token.lower() in KEYS:
            codes = [KEYS[token.lower()]]
        else:
            try:
                codes = [int(token, 0)]
            except ValueError:
                die("%r is not a key name, a single character or a number;"
                    " try `play.py keys`" % token)

        out.extend(codes * repeat)

    return out


def replay(session, want_frames, timeout):
    session.write_rec()

    for stale in (session.png, session.txt):
        if os.path.exists(stale):
            os.remove(stale)

    if want_frames:
        shutil.rmtree(session.frames, ignore_errors=True)
        os.makedirs(session.frames)

    command = [os.path.join(REPO, "build", "Main", "ivan"),
               "--replay", session.rec,
               "--shot", session.png,
               "--text", session.text_log,
               "--trace", session.trace]

    if want_frames:
        command += ["--shot-dir", session.frames]

    environment = dict(os.environ,
                       SDL_VIDEODRIVER="dummy",
                       SDL_AUDIODRIVER="dummy")

    if not os.path.exists(command[0]):
        die("%s is not built; cmake --build build" % command[0])

    with open(session.run_log, "w") as log:
        try:
            done = subprocess.run(command, cwd=session.path, env=environment,
                                  stdout=log, stderr=subprocess.STDOUT,
                                  timeout=timeout)
            status = done.returncode
        except subprocess.TimeoutExpired:
            status = None

    if status is None:
        print("play: the game did not finish within %ds. It is waiting for"
              " something that is not a keypress, or the last key opened a"
              " screen that draws forever. See %s"
              % (timeout, session.run_log))
    elif status != 0:
        print("play: the game exited %d. See %s" % (status, session.run_log))

    return status


def report(session, status):
    if os.path.exists(session.txt):
        with open(session.txt) as f:
            sys.stdout.write(f.read())
    else:
        print("play: no screen was captured.")

        if status == 0:
            print("play: the run ended before the first frame was drawn.")

    print()
    print("screen: %s" % session.png)
    print("keys:   %d  (%s)" % (len(session.keys),
                                " ".join(describe(k) for k in session.keys)
                                or "none"))

    if os.path.isdir(session.frames):
        shots = [f for f in os.listdir(session.frames) if f.endswith(".png")]
        size = sum(os.path.getsize(os.path.join(session.frames, f))
                   for f in os.listdir(session.frames))
        print("frames: %d in %s (%d MB - the PNGs are uncompressed)"
              % (len(shots), session.frames, size // (1 << 20)))


def main():
    parser = argparse.ArgumentParser(
        description="Drive IVAN headlessly and capture the screen.")
    parser.add_argument("--session", default=DEFAULT_SESSION,
                        help="session directory (default %(default)s)")
    parser.add_argument("--frames", action="store_true",
                        help="also capture every frame that changed, into"
                             " <session>/frames. Reaching the first dungeon"
                             " level is about 440 frames and 600MB, because"
                             " the PNGs are uncompressed - the directory is"
                             " emptied on each run, not appended to.")
    parser.add_argument("--timeout", type=int, default=180,
                        help="seconds to allow the replay (default"
                             " %(default)s)")
    sub = parser.add_subparsers(dest="command", required=True)

    new = sub.add_parser("new", help="begin a session")
    new.add_argument("--seed", type=int, default=999)
    new.add_argument("--start", action="store_true",
                     help="also press the 4 keys that accept every character"
                          " creation default and land on the world map")

    send = sub.add_parser("send", help="append keys, replay, capture")
    send.add_argument("keys", nargs="+")

    undo = sub.add_parser("undo", help="drop the last n keys, replay, capture")
    undo.add_argument("count", nargs="?", type=int, default=1)

    auto = sub.add_parser("auto",
                          help="let the wizard auto-play AI take n turns")
    auto.add_argument("turns", nargs="?", type=int, default=100)

    sub.add_parser("show", help="replay the current key list and capture")
    sub.add_parser("rec", help="print the recording")
    sub.add_parser("keys", help="list the key names send accepts")

    args = parser.parse_args()

    if args.command == "keys":
        print("Key names:")
        by_code = {}

        for name, code in sorted(KEYS.items()):
            by_code.setdefault(code, []).append(name)

        for code in sorted(by_code):
            print("  %-24s 0x%03X" % (" ".join(sorted(by_code[code])), code))

        print("\nAlso: any single character (> < ? , . S), text:word to type a"
              "\nword, 0x150 or 336 for a raw code, and name*3 to repeat.")
        print("\nIn game commands:")

        for key, what in COMMANDS:
            print("  %-4s %s" % (key, what))

        return

    session = Session(args.session)

    if args.command == "new":
        if os.path.exists(session.state_file):
            shutil.rmtree(session.path)

        session.create(args.seed)

        if args.start:
            session.keys = parse_keys(START_KEYS)
            session.save()
        else:
            print("Session created at %s, seed %d. No keys yet."
                  % (session.path, session.seed))
            print("`play.py send %s` accepts the character creation defaults"
                  " and lands on the world map." % " ".join(START_KEYS))
            return
    else:
        session.load()

    if args.command == "send":
        session.keys += parse_keys(args.keys)
        session.save()
    elif args.command == "auto":
        if args.turns < 1:
            die("auto needs a positive turn count")

        if not session.keys:
            die("get in-game first — the AI needs a character."
                " Try `play.py new --seed 999 --start`.")

        if not session.autoplay:
            session.keys += parse_keys(WIZARD_KEYS)
            session.autoplay = True
            print("play: activating wizard mode and auto-play"
                  " (%s)" % " ".join(WIZARD_KEYS))

        # One '.' is one AI action, so the turn count and the key count are the
        # same thing. Undoing back past the activation keys leaves `autoplay`
        # set, which would then skip re-activating — checked for on undo.
        session.keys += parse_keys(["."] * args.turns)
        session.save()
    elif args.command == "undo":
        if args.count < 1:
            die("undo needs a positive count")

        if args.count > len(session.keys):
            die("cannot undo %d keys, the session only has %d"
                % (args.count, len(session.keys)))

        del session.keys[len(session.keys) - args.count:]

        # Rewinding past the activation keys un-activates auto-play, otherwise
        # the next `auto` would skip re-activating and the dots would be plain
        # rests with no AI behind them.
        activation = parse_keys(WIZARD_KEYS)
        session.autoplay = any(
            session.keys[i:i + len(activation)] == activation
            for i in range(len(session.keys) - len(activation) + 1))
        session.save()
    elif args.command == "rec":
        session.write_rec()

        with open(session.rec) as f:
            sys.stdout.write(f.read())

        return

    report(session, replay(session, args.frames, args.timeout))


if __name__ == "__main__":
    main()
