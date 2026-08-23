/*
 * Tests for src/audio/sfx.ts. Run with: npm test
 *
 * sfx.js crossed without ever having had one -- music.js and saves.js were the
 * two node suites, and the effects were covered only by ear. What is pinned here
 * is the behaviour that is deliberate rather than incidental, and that a reader
 * would otherwise be free to "fix": the voice cap drops rather than mixes, a
 * failed fetch is cached as a failure, a sound that arrives late is thrown away,
 * and a suspended context drops instead of queueing.
 */

import { test } from 'node:test';
import { strictEqual, deepStrictEqual, ok } from 'node:assert';
import * as Sfx from './sfx.ts';

interface FakeGain {
  gain: { value: number };
  connect(Target: unknown): void;
  disconnect(): void;

  /* What this node was connected to. The trim lives on a node between the
     voices and the master, so where a voice is wired is now part of the
     behaviour and not an implementation detail. */

  To: unknown[];
}

interface FakeSource {
  buffer: unknown;
  onended: (() => void) | null;
  connect(): void;
  start(): void;
}

interface World {
  Gains: FakeGain[];
  Fetched: string[];
  Playing: { gain: number; source: FakeSource }[];
  Ctx: { state: string };
  Now: (Ms: number) => void;
  Fail: () => void;
  Silence: () => string[];
}

/* One page, built fresh per test. The module keeps its state at module scope and
   node caches the module, so Reset() is what stops one test's voices and counts
   leaking into the next. */

function Visit(Search: string): World {
  Sfx.Reset();

  const Fetched: string[] = [];
  const Playing: { gain: number; source: FakeSource }[] = [];
  const Gains: FakeGain[] = [];

  let Failing = false;
  let Clock = 0;

  const Ctx = {
    state: 'running',
    destination: { name: 'destination' },

    createGain(): FakeGain {
      const Node: FakeGain = {
        gain: { value: 1 },
        connect(Target: unknown): void { Node.To.push(Target); },
        disconnect: () => {},
        To: []
      };

      Gains.push(Node);
      return Node;
    },

    createBufferSource(): FakeSource {
      const Node: FakeSource = {
        buffer: null,
        onended: null,
        connect: () => {},
        start(): void {
          /* Start() creates its gain immediately before its source, so the last
             one made is this source's. */
          Playing.push({ gain: Gains[Gains.length - 1]?.gain.value ?? -1, source: Node });
        }
      };

      return Node;
    },

    decodeAudioData: (Bytes: ArrayBuffer) => Promise.resolve({ bytes: Bytes } as unknown as AudioBuffer),
    resume: () => Promise.resolve()
  };

  Object.defineProperty(globalThis, 'location', { configurable: true, value: { search: Search } });
  Object.defineProperty(globalThis, 'document', {
    configurable: true,
    value: { addEventListener: () => {} }
  });
  Object.defineProperty(globalThis, 'AudioContext', {
    configurable: true,
    value: function () { return Ctx; }
  });
  Object.defineProperty(globalThis, 'performance', {
    configurable: true,
    value: { now: () => Clock }
  });

  globalThis.fetch = ((Url: string) => {
    Fetched.push(Url);

    if(Failing)
      return Promise.resolve({ ok: false, status: 404 } as Response);

    return Promise.resolve({
      ok: true,
      arrayBuffer: () => Promise.resolve(new ArrayBuffer(8))
    } as Response);
  }) as typeof fetch;

  return {
    Gains,
    Fetched,
    Playing,
    Ctx,
    Now: (Ms) => { Clock = Ms; },
    Fail: () => { Failing = true; },

    /* The failure path warns on purpose; a test that exercises it should assert
       on the line rather than print it. */
    Silence(): string[] {
      const Seen: string[] = [];
      console.warn = (...Args: unknown[]) => { Seen.push(Args.join(' ')); };
      return Seen;
    }
  };
}

/* Lets the fetch/decode chain hanging off Play() settle. */

function Settle(): Promise<void> {
  return new Promise((Resolve) => { setTimeout(Resolve, 0); });
}

test('a path is recorded and fetched once, then played from cache', async () => {
  const Page = Visit('');

  Sfx.Play('./Sound/hit.wav', 127);
  await Settle();

  deepStrictEqual(Page.Fetched, ['./Sound/hit.wav']);
  strictEqual(Page.Playing.length, 1);
  strictEqual(Sfx.Api.stats().played, 1);

  /* The second call is the common case and takes the synchronous path: no
     second fetch, and playing before Settle() rather than after it. */

  Sfx.Play('./Sound/hit.wav', 127);

  strictEqual(Page.Fetched.length, 1);
  strictEqual(Page.Playing.length, 2);
});

test('volume is ivanconfig 0..127 scaled to unity', async () => {
  const Page = Visit('');

  Sfx.Play('./Sound/hit.wav', 127);
  await Settle();
  strictEqual(Page.Playing[0]?.gain, 1);

  Sfx.Play('./Sound/hit.wav', 0);
  strictEqual(Page.Playing[1]?.gain, 0);

  /* Out of range is clamped rather than trusted: a gain above 1 is distortion
     across the whole master, not a louder effect. */

  Sfx.Play('./Sound/hit.wav', 254);
  strictEqual(Page.Playing[2]?.gain, 1);
});

test('the bus is at unity and every voice routes through it', async () => {
  const Page = Visit('');

  Sfx.Play('./Sound/hit.wav', 127);
  await Settle();

  const Master = Page.Gains[0];
  const Bus = Page.Gains[1];
  const Voice = Page.Gains[2];

  /* The voice spans SDL_mixer's linear 0..127 -> 0..1 and nothing else scales
     it, which is what makes the page and native agree to 0.07dB. §9.7b put the
     12dB this used to take into the files; a number back here would take it
     twice. */

  strictEqual(Voice?.gain.value, 1);
  strictEqual(Bus?.gain.value, 1);

  /* voice -> bus -> master -> destination. Still routed through the bus even at
     unity, because that is what ?sfxgain and bus() move -- a voice reconnected
     straight to the master would ignore both and nothing else would notice. */

  deepStrictEqual(Voice?.To, [Bus]);
  deepStrictEqual(Bus?.To, [Master]);
  strictEqual(Master?.To.length, 1);
});

test('?sfxgain= moves the bus; nonsense and a bare = leave it alone', () => {
  const Level = (Search: string): number | undefined => {
    const Page = Visit(Search);

    Sfx.Api.context();

    return Page.Gains[1]?.gain.value;
  };

  strictEqual(Level(''), 1);                    /* what native sounds like */
  strictEqual(Level('?sfxgain=1'), 1);
  strictEqual(Level('?sfxgain=0.5'), 0.5);
  strictEqual(Level('?sfxgain=0'), 0);          /* silence, if that is the ask */
  strictEqual(Level('?sfxgain=4'), 1);          /* out of range */
  strictEqual(Level('?sfxgain=loud'), 1);
  strictEqual(Level('?sfxgain='), 1);           /* a typo, not a request for silence */
});

test('?sfx=off still records what it would have played', async () => {
  const Page = Visit('?sfx=off');

  Sfx.Play('./Sound/hit.wav', 127);
  await Settle();

  deepStrictEqual(Sfx.Api.played(), ['./Sound/hit.wav']);
  deepStrictEqual(Page.Fetched, []);
  strictEqual(Page.Playing.length, 0);
  strictEqual(Sfx.Api.state(), 'none');
});

test('?sfxbase redirects the fetch and keeps only the filename', async () => {
  const Page = Visit('?sfxbase=https%3A%2F%2Fr2.example%2Fwav%2F');

  Sfx.Play('./Sound/hit.wav', 127);
  await Settle();

  deepStrictEqual(Page.Fetched, ['https://r2.example/wav/hit.wav']);
});

/* 16 voices, and the seventeenth is dropped rather than mixed -- a hundred
   simultaneous copies of hit.wav is a bug that presents as a loud noise. */

test('the voice cap drops rather than mixing', async () => {
  const Page = Visit('');

  Sfx.Play('./Sound/hit.wav', 127);
  await Settle();

  for(let N = 0; N < 20; N++)
    Sfx.Play('./Sound/hit.wav', 127);

  strictEqual(Page.Playing.length, 16);
  strictEqual(Sfx.Api.stats().voices, 16);
  ok(Sfx.Api.stats().dropped >= 5);

  /* A finished voice frees its slot. */

  Page.Playing[0]?.source.onended?.();
  strictEqual(Sfx.Api.stats().voices, 15);

  Sfx.Play('./Sound/hit.wav', 127);
  strictEqual(Page.Playing.length, 17);
});

test('a missing file is one warning and one fetch for the session', async () => {
  const Page = Visit('');
  const Warned = Page.Silence();

  Page.Fail();
  Sfx.Play('./Sound/gone.wav', 127);
  await Settle();

  strictEqual(Sfx.Api.stats().failed, 1);
  strictEqual(Warned.length, 1);
  ok(Warned[0]?.includes('gone.wav'), Warned[0]);

  /* Cached as a failure: asking again is neither a second fetch nor a second
     line, because the game asks for the same effect hundreds of times. */

  Sfx.Play('./Sound/gone.wav', 127);
  await Settle();

  strictEqual(Page.Fetched.length, 1);
  strictEqual(Sfx.Api.stats().failed, 1);
  strictEqual(Warned.length, 1);
  strictEqual(Page.Playing.length, 0);
});

/* currentTime does not advance while a context is suspended, so a sound
   scheduled against one waits and then fires together with every other queued
   sound the moment the autoplay policy releases it. */

test('a suspended context drops instead of queueing', async () => {
  const Page = Visit('');

  Page.Ctx.state = 'suspended';
  Sfx.Play('./Sound/hit.wav', 127);
  await Settle();

  strictEqual(Page.Playing.length, 0);
  strictEqual(Sfx.Api.stats().played, 0);
  ok(Sfx.Api.stats().dropped >= 1);
  strictEqual(Sfx.Api.state(), 'suspended');

  /* The buffer still decoded, so it is ready the moment the gesture lands. */

  Page.Ctx.state = 'running';
  Sfx.Play('./Sound/hit.wav', 127);

  strictEqual(Page.Playing.length, 1);
  strictEqual(Page.Fetched.length, 1);
});

test('a sound that lands too late belongs to no turn and is dropped', async () => {
  const Page = Visit('');

  Sfx.Play('./Sound/hit.wav', 127);
  Page.Now(251);
  await Settle();

  strictEqual(Page.Playing.length, 0);
  ok(Sfx.Api.stats().dropped >= 1);

  /* 250ms is the bound itself, and on it the sound still plays -- the first
     door in a session is worth hearing late. */

  const Fresh = Visit('');

  Sfx.Play('./Sound/hit.wav', 127);
  Fresh.Now(250);
  await Settle();

  strictEqual(Fresh.Playing.length, 1);
});

test('the console API and the contract music depends on', () => {
  Visit('');

  strictEqual(typeof Sfx.Api.play, 'function');
  strictEqual(typeof Sfx.Api.stats, 'function');
  strictEqual(typeof Sfx.Api.played, 'function');
  strictEqual(typeof Sfx.Api.state, 'function');

  /* music borrows both rather than opening a second context. */

  strictEqual(typeof Sfx.Api.context, 'function');
  strictEqual(typeof Sfx.Api.master, 'function');
  strictEqual(typeof Sfx.Api.bus, 'function');

  strictEqual(Sfx.Api.master(), null);
  strictEqual(Sfx.Api.bus(), null);
  ok(Sfx.Api.context());
  ok(Sfx.Api.master());
  ok(Sfx.Api.bus());

  /* stats() reports the node rather than the constant, so a level moved through
     bus() reads back out. */

  strictEqual(Sfx.Api.stats().trim, 1);

  const Bus = Sfx.Api.bus();

  if(Bus)
    Bus.gain.value = 0.4;

  strictEqual(Sfx.Api.stats().trim, 0.4);
});

test('the log keeps the last 256 paths, newest last', () => {
  Visit('?sfx=off');

  for(let N = 0; N < 300; N++)
    Sfx.Play('./Sound/' + N + '.wav', 127);

  const Log = Sfx.Api.played();

  strictEqual(Log.length, 256);
  strictEqual(Log[Log.length - 1], './Sound/299.wav');
  strictEqual(Log[0], './Sound/44.wav');
});
