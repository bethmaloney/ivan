/*
 * What the wasm side calls on the page, declared once.
 *
 * These are not types the compiler can check across the boundary: an EM_JS body
 * is a string of JavaScript pasted into ivan.js, so `Music.setVolume(Level)`
 * resolving to nothing is a silent no-op at runtime rather than any kind of
 * error -- and the corpora cannot see it, because a headless replay makes no
 * sound. contract.test.ts parses the EM_JS blocks out of the C++ and checks them
 * against this in both directions, so a call that is not declared fails and a
 * declaration nothing calls any more fails too.
 *
 * The C++ side is the authority on the names. When it changes, this changes.
 */

export interface Bridge {
  /* Where the EM_JS block lives, so a failure names the file to open. */
  source: string;
  methods: readonly string[];
}

export const Bridges: Record<string, Bridge> = {
  ivanSfx: {
    source: 'FeLib/Source/sfx.cpp',
    methods: ['play']
  },

  ivanMusic: {
    source: 'audio/audio.cpp',
    methods: ['setPlaylist', 'setPlaying', 'setVolume', 'setIntensity', 'currentIndex']
  }
};

/* The directories an EM_JS block can appear in. Main/Source has none today and
   is scanned anyway: a bridge added there and nowhere else is exactly the case
   this test exists to catch. */

export const SourceDirs: readonly string[] = ['FeLib/Source', 'audio', 'Main/Source'];
