/*
 * Tests for src/audio/music.ts. Run with: npm test
 *
 * A port of tools/web/music.test.js, which was one sequential narrative sharing
 * a single world; these are the same checks as independent cases, each building
 * its own page. The module's job is a contract with audio.cpp and a piece of
 * arithmetic, and both are checkable without a browser. What is stubbed is the
 * ambient API -- AudioContext, media elements, fetch -- and what is exercised is
 * everything the module actually decides:
 *
 *   - the playlist crossing the boundary, and the index that crosses back,
 *     which is what dungeon::PrepareMusic branches on
 *   - the rule that a level change sharing the current track must not restart
 *     it, and that one dropping it must
 *   - intensity to gains, against the two tables in audio.cpp
 *   - tracks with no stems, which is six of the eleven and must be silence
 *     rather than an error
 *
 * What it cannot check is that a browser makes a sound. That needs the render
 * and a machine with an audio device; see HARNESS.md §9.8.
 */

import { test, after } from 'node:test';
import { strictEqual, deepStrictEqual, ok } from 'node:assert';
import * as Music from './music.ts';

interface FakeParam {
  value: number;
  setValueAtTime(Value: number): void;
  cancelScheduledValues(): void;
  linearRampToValueAtTime(Value: number, When: number): void;
}

interface FakeGain {
  gain: FakeParam;
  ConnectedTo: unknown;
  connect(Target: unknown): void;
  disconnect(): void;
}

interface FakeMedia {
  src: string;
  preload: string;
  loop: boolean;
  paused: boolean;
  currentTime: number;
  duration: number;
  playbackRate: number;
  readyState: number;
  addEventListener(Name: string, Fn: () => void, Options?: { once?: boolean }): void;
  removeAttribute(): void;
  load(): void;
  pause(): void;
  play(): Promise<void>;

  /* The stub's own half: fire an event the module subscribed to. */
  Fire(Name: string): void;
}

interface World {
  Elements: FakeMedia[];
  Fetched: string[];
  Ramps: number[];
  Master: FakeGain;

  /* The node the module hangs off the shared master, which is where the music
     volume has to live -- never on the master itself. */
  MusicNode(): FakeGain | undefined;

  /* readyState for elements built after this call. 4 is HAVE_ENOUGH_DATA, 0 is
     HAVE_NOTHING, and 0 is what exercises the readiness barrier. */
  Buffered(State: number): void;

  Silence(): string[];
}

/* Five of the eleven tracks, including two of the six that have no notes at all
   -- Music/stems.json records those as empty rather than omitting them. */

const Manifest: Record<string, string[]> = {
  'Cathedral.mid': ['const', 'fadein'],
  'Dungeon.mid': ['const', 'fadeout', 'fadein'],
  'Dungeon2.mid': ['const', 'fadeout', 'fadein'],
  'Empty.mid': [],
  'mainmenu.mid': []
};

/* One page, built fresh per test. The module keeps its state at module scope and
   node caches the module, so Reset() is what stops one test's track, counters
   and drift timer leaking into the next. */

function Visit(Search: string): World {
  Music.Reset();

  const Elements: FakeMedia[] = [];
  const Fetched: string[] = [];
  const Ramps: number[] = [];
  const Created: FakeGain[] = [];

  let NextReadyState = 4;

  function Param(): FakeParam {
    const Self: FakeParam = {
      value: 0,
      setValueAtTime(Value) { Self.value = Value; },
      cancelScheduledValues() {},

      /* The ramp target is applied at once and its duration recorded, so a test
         can assert both the value the mix lands on and the rate it gets there
         at -- the rate is audio.cpp's US_PER_VOLUME_CHANGE and is part of the
         behaviour, not a smoothing detail. currentTime never advances, so
         `When` is the duration. */

      linearRampToValueAtTime(Value, When) {
        Ramps.push(+When.toFixed(4));
        Self.value = Value;
      }
    };

    return Self;
  }

  function Gain(): FakeGain {
    const Node: FakeGain = {
      gain: Param(),
      ConnectedTo: null,
      connect(Target) { Node.ConnectedTo = Target; },
      disconnect() {}
    };

    Created.push(Node);
    return Node;
  }

  /* Starts at unity, as a real one does, so anything the module wrongly writes
     here shows up. Effects pass through it with their own SfxVolume. */

  const Master = Gain();
  Master.gain.value = 1;

  const Ctx = {
    state: 'running',
    currentTime: 0,
    destination: { name: 'destination' },
    createGain: Gain,
    createMediaElementSource: (Media: FakeMedia) => ({
      media: Media,
      connect: () => {},
      disconnect: () => {}
    })
  };

  Object.defineProperty(globalThis, 'location', { configurable: true, value: { search: Search } });
  Object.defineProperty(globalThis, 'document', {
    configurable: true,
    value: { addEventListener: () => {} }
  });
  Object.defineProperty(globalThis, 'Audio', {
    configurable: true,
    value: function (): FakeMedia {
      const Listeners = new Map<string, (() => void)[]>();

      const Self: FakeMedia = {
        src: '', preload: '', loop: false, paused: true,
        currentTime: 0, duration: 100, playbackRate: 1,
        readyState: NextReadyState,

        addEventListener(Name, Fn, Options) {
          const Wrapped = Options?.once
            ? () => {
              const Kept = (Listeners.get(Name) ?? []).filter((Other) => Other !== Wrapped);
              Listeners.set(Name, Kept);
              Fn();
            }
            : Fn;

          Listeners.set(Name, [...(Listeners.get(Name) ?? []), Wrapped]);
        },

        removeAttribute() {},
        load() {},
        pause() { Self.paused = true; },
        play() { Self.paused = false; return Promise.resolve(); },
        Fire(Name) { for(const Fn of [...(Listeners.get(Name) ?? [])]) Fn(); }
      };

      Elements.push(Self);
      return Self;
    }
  });

  Object.defineProperty(globalThis, 'ivanSfx', {
    configurable: true,
    value: { context: () => Ctx, master: () => Master }
  });

  globalThis.fetch = ((Url: string) => {
    Fetched.push(Url);

    return Promise.resolve({
      ok: true,
      json: () => Promise.resolve(Manifest)
    } as Response);
  }) as typeof fetch;

  return {
    Elements,
    Fetched,
    Ramps,
    Master,
    MusicNode: () => Created.find((Node) => Node.ConnectedTo === Master),
    Buffered: (State) => { NextReadyState = State; },

    Silence(): string[] {
      const Seen: string[] = [];
      console.warn = (...Args: unknown[]) => { Seen.push(Args.join(' ')); };
      return Seen;
    }
  };
}

/* Lets the manifest promise and the chain hanging off it settle. */

function Settle(): Promise<void> {
  return new Promise((Resolve) => { setTimeout(Resolve, 0); });
}

function Wait(Ms: number): Promise<void> {
  return new Promise((Resolve) => { setTimeout(Resolve, Ms); });
}

/* The last test leaves a live Sync interval, and nothing after it calls Visit(). */

after(() => { Music.Reset(); });

/* Drives the module to a playing track the way the game does: main.cpp:185-188
   is stop, clear, load, play. */

async function Playing(Names: string): Promise<void> {
  Music.Api.setPlaylist(Names);
  Music.Api.setPlaying(true);
  await Settle();
}

function Gains(): number[] {
  return Music.Api.stats().gains;
}

test('nothing is playing before the game says anything', () => {
  Visit('');

  /* -1, not an accidental 0, or every level change would think it shared a
     track with the one before it. */

  strictEqual(Music.Api.currentIndex(), -1);
  deepStrictEqual(Music.Api.playlist(), []);
  strictEqual(Music.Api.stats().track, null);
  strictEqual(Music.Api.stats().state, 'none');
});

test('a track with no stems is silence, not an error', async () => {
  const Page = Visit('');

  await Playing('mainmenu.mid');

  strictEqual(Page.Fetched.length, 1);
  strictEqual(Music.Api.stats().track, null);
  strictEqual(Music.Api.currentIndex(), -1);
  strictEqual(Music.Api.stats().failed, 0);
  strictEqual(Page.Elements.length, 0);
});

test('a real track builds one element per rendered stem', async () => {
  const Page = Visit('');

  await Playing('Cathedral.mid');

  strictEqual(Music.Api.stats().track, 'Cathedral.mid');
  deepStrictEqual(Music.Api.stats().stems, ['const', 'fadein']);
  strictEqual(Music.Api.currentIndex(), 0);
  strictEqual(Page.Elements.length, 2);
  deepStrictEqual(Page.Elements.map((E) => E.src),
                  ['./Music/Cathedral.const.ogg', './Music/Cathedral.fadein.ogg']);
  ok(Page.Elements.every((E) => !E.paused));
});

/* audio.cpp:84-89. Intensity 0 is full health, and it is the case that carries
   62% of the notes -- getting it wrong is inaudible rather than obviously
   broken. `const` never moves, which is the property the three-way split exists
   to keep. */

test('intensity maps to the two volume tables', async () => {
  Visit('');

  await Playing('Cathedral.mid');

  Music.Api.setIntensity(0);
  deepStrictEqual(Gains(), [1, 0], 'at full health, fadein is silent');

  Music.Api.setIntensity(127);
  deepStrictEqual(Gains(), [1, 1], "at death's door, fadein is full");

  /* Halfway, through the GM square law: (64/127)^2 = 0.2539. */

  Music.Api.setIntensity(64);
  deepStrictEqual(Gains(), [1, 0.254]);

  /* A track with all three curves, so the fadeout arm can be checked too. */

  Visit('');

  await Playing('Dungeon.mid');

  Music.Api.setIntensity(0);
  deepStrictEqual(Gains(), [1, 1, 0], 'full health: fadeout up, fadein down');

  Music.Api.setIntensity(127);
  deepStrictEqual(Gains(), [1, 0, 1], 'near death: fadeout down, fadein up');
});

/* The mix does not jump to a new intensity, it slews -- audio.cpp:78 moves
   CurrentIntensity one step per 15ms toward the target, so the swell as a fight
   turns bad takes real time. Asserted because a ramp merely "smoothed" by some
   arbitrary constant would sound wrong in a way nobody would think to look
   for. */

test('the mix slews at 15ms per step rather than jumping', async () => {
  const Page = Visit('');

  await Playing('Dungeon.mid');

  Music.Api.setIntensity(64);
  Music.Api.setIntensity(0);
  strictEqual(Page.Ramps[Page.Ramps.length - 1], 0.96, '64 steps is 64 * 15ms');

  Music.Api.setIntensity(127);
  strictEqual(Page.Ramps[Page.Ramps.length - 1], 1.905, 'a full sweep is 1.905s');

  Music.Api.setIntensity(126);
  strictEqual(Page.Ramps[Page.Ramps.length - 1], 0.015, 'a one-step change still ramps');
});

/* dungeon::PrepareMusic changes the playlist on every level change and
   dungeon.cpp:174 relies on the music not restarting when the new area shares
   the track already playing. */

test('a level change that keeps the track does not restart it', async () => {
  Visit('');

  await Playing('Cathedral.mid');

  const Before = Music.Api.stats().started;

  Music.Api.setPlaylist('Cathedral.mid,Dungeon.mid');
  await Settle();

  strictEqual(Music.Api.stats().started, Before);
  strictEqual(Music.Api.stats().track, 'Cathedral.mid');
  strictEqual(Music.Api.currentIndex(), 0);

  /* Reordered so Cathedral is no longer first: the index must follow the name,
     not a position remembered from before. */

  Music.Api.setPlaylist('Dungeon.mid,Cathedral.mid');
  await Settle();

  strictEqual(Music.Api.currentIndex(), 1);
  strictEqual(Music.Api.stats().started, Before);
});

test('a level change that drops the track cuts it short', async () => {
  Visit('');

  await Playing('Cathedral.mid');

  const Before = Music.Api.stats().started;

  Music.Api.setPlaylist('Dungeon.mid');
  await Settle();

  strictEqual(Music.Api.stats().track, 'Dungeon.mid');
  strictEqual(Music.Api.stats().started, Before + 1);
  deepStrictEqual(Music.Api.stats().stems, ['const', 'fadeout', 'fadein']);
  strictEqual(Music.Api.currentIndex(), 0);
});

/* ivanconfig::Initialize sets the volume long before a gesture can have let an
   AudioContext exist (iconf.cpp:1343), so this is the ordinary case, not an edge
   one: the value has to survive until there is somewhere to put it. */

test('a volume set before any context existed lands on the node', async () => {
  const Page = Visit('');

  strictEqual(Page.MusicNode(), undefined, 'no gain node before a context');
  Music.Api.setVolume(64);

  await Playing('Dungeon.mid');

  strictEqual(+(Page.MusicNode()?.gain.value ?? -1).toFixed(3), 0.254);

  /* And on the module's own node, never on the shared one -- effects pass
     through that and have their own setting. */

  strictEqual(Page.Master.gain.value, 1);

  Music.Api.setVolume(127);
  strictEqual(Page.MusicNode()?.gain.value, 1);

  Music.Api.setVolume(0);
  strictEqual(Page.MusicNode()?.gain.value, 0);

  Music.Api.setVolume(64);
  strictEqual(+(Page.MusicNode()?.gain.value ?? -1).toFixed(3), 0.254);
  strictEqual(Page.Master.gain.value, 1);
});

/* audio::Loop picked a new track at random from the playlist when one ended,
   and so does this. With one entry it is the same track again. */

test('a finished track is replaced from the playlist', async () => {
  const Page = Visit('');

  await Playing('Dungeon.mid');

  const Started = Music.Api.stats().started;
  const Leader = Page.Elements[0];

  Leader?.Fire('ended');
  await Settle();

  strictEqual(Music.Api.stats().started, Started + 1);
  strictEqual(Music.Api.stats().finished, 1);
  strictEqual(Music.Api.stats().track, 'Dungeon.mid');
});

/* The readiness barrier, and the reason it exists. Measured in a real session
   before it did: the fade-out stem, the largest of the three, began 166ms behind
   the other two, because play() on three elements starts whichever has data
   first. Two of the three stayed locked to 0.1ms, so the clocks were never the
   problem -- the start was. */

test('nothing plays until every stem can', async () => {
  const Page = Visit('');

  Page.Buffered(0);
  await Playing('Dungeon2.mid');

  const Started = (): number => Page.Elements.filter((E) => !E.paused).length;

  strictEqual(Page.Elements.length, 3);
  strictEqual(Started(), 0, 'none start while one is unbuffered');

  /* Two of the three arrive. Still nothing, because a partial start is exactly
     the 166ms bug. */

  for(const At of [0, 1]) {
    const Stem = Page.Elements[At];

    if(Stem) {
      Stem.readyState = 4;
      Stem.Fire('canplay');
    }
  }

  await Settle();
  strictEqual(Started(), 0, 'two ready is still not enough');

  const Last = Page.Elements[2];

  if(Last) {
    Last.readyState = 4;
    Last.Fire('canplay');
  }

  await Settle();
  strictEqual(Started(), 3, 'all three go once the last is ready');
});

/* A stem that errors must not hold the other two silent forever -- a thinner mix
   beats none, so the barrier releases on failure as well as on success. */

test('a stem that will not load is a fault, and releases the barrier', async () => {
  const Page = Visit('');

  Page.Buffered(0);
  await Playing('Dungeon.mid');

  const Warned = Page.Silence();

  for(const At of [0, 1]) {
    const Stem = Page.Elements[At];

    if(Stem) {
      Stem.readyState = 4;
      Stem.Fire('canplay');
    }
  }

  await Settle();
  strictEqual(Page.Elements.filter((E) => !E.paused).length, 0,
              'a stem still loading holds the start');

  Page.Elements[2]?.Fire('error');
  await Settle();

  /* The module calls play() on the broken one too, which a real browser rejects
     and Launch swallows; the stub cannot model that, so the check is on the
     stems that were supposed to work. */

  deepStrictEqual([Page.Elements[0]?.paused, Page.Elements[1]?.paused], [false, false]);
  strictEqual(Music.Api.stats().failed, 1);
  strictEqual(Warned.length, 1);
  ok(/Dungeon\.mid\.fadein/.test(Warned[0] ?? ''), Warned[0]);
});

/* The barrier gets them started together but play() is still not synchronous
   across three elements, so a small skew survives it. That is what the one Align
   pass is for, and 30ms is the shape of it: too small for Sync to seek (it
   nudges below 50ms) and far too big to leave in place, since a nudge would take
   six seconds to walk it back. Only Align fixes this one. */

test('a small start skew is aligned out', async () => {
  const Page = Visit('');

  await Playing('Dungeon.mid');

  const Skewed = Page.Elements[1];
  const Before = Music.Api.stats().seeks;

  ok(Skewed);
  Skewed.currentTime = 0.03;
  await Wait(600);

  strictEqual(Music.Api.stats().seeks, Before + 1);
  strictEqual(Skewed.currentTime, Page.Elements[0]?.currentTime);
});

/* Steady-state drift correction. The thresholds are the ones the 166ms
   measurement moved, so the cases are written with that number in them: a gap
   that large has to be seeked, because closing it with the old 0.2% nudge would
   have taken 83 seconds. */

test('a gap the nudge cannot close is seeked, a small one is nudged', async () => {
  const Page = Visit('');

  await Playing('Dungeon.mid');

  /* Let the one-time Align pass fire and be done first. Without this wait the
     checks below catch Align rather than Sync -- it seeks on a 10ms tolerance,
     so it swallows any gap injected before it runs, and the steady-state
     thresholds these cases exist for are never reached at all. */

  await Wait(600);

  const Leader = Page.Elements[0];
  const Far = Page.Elements[1];
  const Near = Page.Elements[2];
  const Seeks = Music.Api.stats().seeks;

  ok(Leader && Far && Near);

  Far.currentTime = 0.166;                 /* what the fade-out stem actually did */
  await Wait(700);

  strictEqual(Music.Api.stats().seeks, Seeks + 1);
  strictEqual(Far.currentTime, Leader.currentTime);
  strictEqual(Far.playbackRate, 1, 'and back at normal rate');

  const Corrections = Music.Api.stats().corrections;

  Near.currentTime = 0.02;
  await Wait(700);

  strictEqual(Music.Api.stats().seeks, Seeks + 1, 'a small gap is not seeked');
  ok(Music.Api.stats().corrections > Corrections);

  /* Ahead of the leader, so it slows down and lets the leader come to it. */

  ok(Near.playbackRate < 1, 'by easing off the rate');
  ok(Near.playbackRate >= 0.995, 'but only slightly');
});

/* SetPlaybackStatus(0) from ChangeMIDIOutputDevice(0) -- the player turning the
   soundtrack off in the options menu. */

test('stopping clears the track and the index', async () => {
  Visit('');

  await Playing('Dungeon.mid');
  Music.Api.setPlaying(false);

  strictEqual(Music.Api.stats().track, null);
  strictEqual(Music.Api.currentIndex(), -1);
});

/* What DeInit pushes on the way out. */

test('an empty playlist plays nothing and is not a fault', async () => {
  const Page = Visit('');

  await Playing('');

  strictEqual(Music.Api.stats().track, null);
  strictEqual(Music.Api.stats().failed, 0);
  strictEqual(Page.Elements.length, 0);
});

/* The three query options, which the node suite never covered. They are read at
   call time rather than at load now that this is a module rather than a --pre-js,
   which is what lets one process hold more than one page. */

test('?music=off records the playlist and opens no context', async () => {
  const Page = Visit('?music=off');

  await Playing('Dungeon.mid');

  deepStrictEqual(Music.Api.playlist(), ['Dungeon.mid']);
  strictEqual(Music.Api.stats().playing, true);
  strictEqual(Music.Api.stats().track, null);
  strictEqual(Music.Api.stats().state, 'none', 'no AudioContext was borrowed');
  deepStrictEqual(Page.Fetched, []);
  strictEqual(Page.Elements.length, 0);
});

test('?musicbase redirects the manifest and the stems', async () => {
  const Page = Visit('?musicbase=https%3A%2F%2Fr2.example%2Fogg%2F');

  await Playing('Cathedral.mid');

  deepStrictEqual(Page.Fetched, ['https://r2.example/ogg/stems.json']);
  deepStrictEqual(Page.Elements.map((E) => E.src), [
    'https://r2.example/ogg/Cathedral.const.ogg',
    'https://r2.example/ogg/Cathedral.fadein.ogg'
  ]);
});

test('?musiccurve=linear drops the GM square law', async () => {
  const Page = Visit('?musiccurve=linear');

  Music.Api.setVolume(64);
  await Playing('Cathedral.mid');

  /* 64/127 rather than (64/127)^2. */

  strictEqual(+(Page.MusicNode()?.gain.value ?? -1).toFixed(3), 0.504);

  Music.Api.setIntensity(64);
  deepStrictEqual(Gains(), [1, 0.504]);
});

test('the console API and the five names audio.cpp calls', () => {
  Visit('');

  for(const Name of ['setPlaylist', 'setPlaying', 'setVolume', 'setIntensity',
                     'currentIndex', 'resume', 'playlist', 'stats'])
    strictEqual(typeof (Music.Api as unknown as Record<string, unknown>)[Name], 'function', Name);
});
