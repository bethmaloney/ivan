/*
 * Tests for tools/web/music.js. Run with: node tools/web/music.test.js
 *
 * The module's job is a contract with audio.cpp and a piece of arithmetic, and
 * both are checkable without a browser. What is stubbed here is the ambient
 * API -- AudioContext, media elements, fetch -- and what is exercised is
 * everything music.js actually decides:
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

'use strict';

var Failures = 0;
var Checks = 0;

function Check(What, Got, Want) {
  Checks++;

  var Same = JSON.stringify(Got) === JSON.stringify(Want);

  if(!Same) {
    Failures++;
    console.log('FAIL ' + What + '\n  got  ' + JSON.stringify(Got) +
                '\n  want ' + JSON.stringify(Want));
  }
}

/* ---- stubs ------------------------------------------------------------- */

var Now = 0;
var Fetched = [];
var AllRamps = [];

function Param() {
  var Self = { value: 0 };

  Self.setValueAtTime = function (Value) { Self.value = Value; };
  Self.cancelScheduledValues = function () {};

  /* The ramp target is applied at once and its duration recorded, so a test
     can assert both the value the mix lands on and the rate it gets there at
     -- the rate is audio.cpp's US_PER_VOLUME_CHANGE and is part of the
     behaviour, not a smoothing detail. */

  Self.linearRampToValueAtTime = function (Value, When) {
    AllRamps.push(+(When - Now).toFixed(4));
    Self.value = Value;
  };

  return Self;
}

/* Every stem ramps together and over the same span, so the last one recorded
   is the span of the change as a whole. */
function LastRamp() {
  return AllRamps[AllRamps.length - 1];
}

var Elements = [];

global.Audio = function () {
  var Self = {
    src: '', preload: '', loop: false, paused: true,
    currentTime: 0, duration: 100, playbackRate: 1,
    Listeners: {}
  };

  Self.addEventListener = function (Name, Fn) {
    (Self.Listeners[Name] = Self.Listeners[Name] || []).push(Fn);
  };

  Self.removeAttribute = function () {};
  Self.load = function () {};
  Self.pause = function () { Self.paused = true; };
  Self.play = function () { Self.paused = false; return Promise.resolve(); };
  Self.Fire = function (Name) {
    (Self.Listeners[Name] || []).forEach(function (Fn) { Fn(); });
  };

  Elements.push(Self);
  return Self;
};

var CreatedGains = [];

var Ctx = {
  state: 'running',
  destination: { name: 'destination' },
  createGain: function () {
    var Node = { gain: Param(), connectedTo: null, disconnect: function () {} };

    Node.connect = function (Target) { Node.connectedTo = Target; };
    CreatedGains.push(Node);

    return Node;
  },
  createMediaElementSource: function (Media) {
    return { media: Media, connect: function () {}, disconnect: function () {} };
  }
};

Object.defineProperty(Ctx, 'currentTime', { get: function () { return Now; } });

/* The gain sfx.js owns and effects also pass through. Starts at unity, as a
   real one does, so that anything music.js wrongly writes here shows up. */
var MasterGain = { gain: Param(), connect: function () {}, disconnect: function () {} };

MasterGain.gain.value = 1;

/* music.js's own node is the one it hangs off the shared master. */
function MusicNode() {
  return CreatedGains.filter(function (G) { return G.connectedTo === MasterGain; })[0];
}

global.ivanSfx = {
  context: function () { return Ctx; },
  master: function () { return MasterGain; }
};

global.document = { addEventListener: function () {} };
global.location = { search: '' };
global.window = {};

var Manifest = {
  'Cathedral.mid': ['const', 'fadein'],
  'Dungeon.mid': ['const', 'fadeout', 'fadein'],
  'Dungeon2.mid': ['const', 'fadeout', 'fadein'],
  'Empty.mid': [],
  'mainmenu.mid': []
};

global.fetch = function (Url) {
  Fetched.push(Url);

  return Promise.resolve({
    ok: true,
    json: function () { return Promise.resolve(Manifest); }
  });
};

require('./music.js');

var Music = global.ivanMusic;

/* Lets the manifest promise and the chain hanging off it settle. */
function Settle() {
  return new Promise(function (Resolve) { setTimeout(Resolve, 0); });
}

function Gains() {
  return Music.stats().gains;
}

/* ---- the tests --------------------------------------------------------- */

(async function () {
  /* Nothing playing yet: the index dungeon::PrepareMusic reads must be -1, not
     an accidental 0, or every level change would think it shared a track. */
  Check('index before anything', Music.currentIndex(), -1);
  Check('playlist starts empty', Music.playlist(), []);

  /* ivanconfig::Initialize sets the volume long before a gesture can have let
     an AudioContext exist (iconf.cpp:1343), so this is the ordinary case, not
     an edge one. The value has to survive until there is somewhere to put it.  */
  Check('no gain node before a context', MusicNode(), undefined);
  Music.setVolume(64);

  /* main.cpp:185-188 -- stop, clear, load, play. */
  Music.setPlaylist('mainmenu.mid');
  Music.setPlaying(true);
  await Settle();

  Check('manifest fetched once', Fetched.length, 1);
  Check('a track with no stems plays nothing', Music.stats().track, null);
  Check('and reports no index', Music.currentIndex(), -1);
  Check('and is not an error', Music.stats().failed, 0);

  /* A real track, as dungeon::PrepareMusic loads for Attnam. */
  Music.setPlaylist('Cathedral.mid');
  await Settle();

  Check('cathedral is playing', Music.stats().track, 'Cathedral.mid');
  Check('with only the stems it has', Music.stats().stems, ['const', 'fadein']);
  Check('index resolves for PrepareMusic', Music.currentIndex(), 0);
  Check('one element per stem', Elements.length, 2);
  Check('urls are the rendered stems',
        Elements.map(function (E) { return E.src; }),
        ['./Music/Cathedral.const.ogg', './Music/Cathedral.fadein.ogg']);
  Check('and both are playing', Elements.every(function (E) { return !E.paused; }), true);

  /* Intensity 0 is full health: const at full, fadein silent. This is the
     case that carries 62% of the notes, and getting it wrong is inaudible
     rather than obviously broken. */
  Music.setIntensity(0);
  Check('at full health, fadein is silent', Gains(), [1, 0]);

  Music.setIntensity(127);
  Check('at death\'s door, fadein is full', Gains(), [1, 1]);

  /* Halfway, through the GM square law: (64/127)^2 = 0.2539. const never
     moves, which is the property the whole three-way split exists to keep. */
  Music.setIntensity(64);
  Check('halfway is the square law', Gains(), [1, 0.254]);

  /* The mix does not jump to a new intensity, it slews -- audio.cpp:78 moves
     CurrentIntensity one step per 15ms toward the target, so the swell as a
     fight turns bad takes real time. 64 steps down from 64 is 0.96s, and a
     full 127-step sweep is 1.905s. Asserted because a ramp that is merely
     "smoothed" by some arbitrary constant would sound wrong in a way nobody
     would think to look for. */

  Music.setIntensity(0);
  Check('64 steps ramps over 64 * 15ms', LastRamp(), 0.96);

  Music.setIntensity(127);
  Check('a full sweep takes the full 1.905s', LastRamp(), 1.905);

  Music.setIntensity(126);
  Check('a one-step change still ramps', LastRamp(), 0.015);

  Music.setIntensity(0);

  /* A level change that keeps the current track must not restart it:
     dungeon.cpp:174 relies on exactly this. */
  var Before = Music.stats().started;

  Music.setPlaylist('Cathedral.mid,Dungeon.mid');
  await Settle();

  Check('sharing the track does not restart it', Music.stats().started, Before);
  Check('and it is still the same one', Music.stats().track, 'Cathedral.mid');
  Check('index follows the reorder', Music.currentIndex(), 0);

  /* Reordered so Cathedral is no longer first -- the index must follow the
     name, not a position remembered from before. */
  Music.setPlaylist('Dungeon.mid,Cathedral.mid');
  await Settle();

  Check('index is resolved by name', Music.currentIndex(), 1);
  Check('still not restarted', Music.stats().started, Before);

  /* A level change that drops it must stop it and start something else. */
  Music.setPlaylist('Dungeon.mid');
  await Settle();

  Check('dropping the track changes the music', Music.stats().track, 'Dungeon.mid');
  Check('which is a new start', Music.stats().started, Before + 1);
  Check('three stems this time', Music.stats().stems, ['const', 'fadeout', 'fadein']);
  Check('index is right again', Music.currentIndex(), 0);

  /* Now all three curves are present, so the fadeout arm can be checked. */
  Music.setIntensity(0);
  Check('full health: fadeout up, fadein down', Gains(), [1, 1, 0]);

  Music.setIntensity(127);
  Check('near death: fadeout down, fadein up', Gains(), [1, 0, 1]);

  /* The volume set before any context existed had to land on the node when it
     was finally made, or the game would start at full volume whatever the
     player configured. */
  Check('the early volume was applied', +MusicNode().gain.value.toFixed(3), 0.254);

  /* And it must land on music.js's own node, never on the shared one --
     sfx.js's effects pass through that, and they have their own setting. */
  Check('the shared master is untouched', MasterGain.gain.value, 1);

  Music.setVolume(127);
  Check('full volume', MusicNode().gain.value, 1);

  Music.setVolume(0);
  Check('silence', MusicNode().gain.value, 0);

  Music.setVolume(64);
  Check('half volume is the same curve', +MusicNode().gain.value.toFixed(3), 0.254);
  Check('and the master is still untouched', MasterGain.gain.value, 1);

  /* End of track: audio::Loop picked a new one at random from the playlist,
     and so does this. With one entry it is the same track again. */
  var Started = Music.stats().started;
  var Leader = Elements[Elements.length - 3];

  Leader.Fire('ended');
  await Settle();

  Check('a finished track is replaced', Music.stats().started, Started + 1);
  Check('and counted', Music.stats().finished, 1);
  Check('with the only track available', Music.stats().track, 'Dungeon.mid');

  /* SetPlaybackStatus(0) from ChangeMIDIOutputDevice(0) -- the player turning
     the soundtrack off in the options menu. */
  Music.setPlaying(false);
  Check('stopping clears the track', Music.stats().track, null);
  Check('and the index goes back to -1', Music.currentIndex(), -1);

  /* An empty playlist is what DeInit pushes on the way out. */
  Music.setPlaylist('');
  Music.setPlaying(true);
  await Settle();

  Check('an empty playlist plays nothing', Music.stats().track, null);
  Check('and does not fault', Music.stats().failed, 0);

  console.log((Failures ? 'FAILED ' : 'ok ') + (Checks - Failures) + '/' + Checks +
              ' checks');
  process.exit(Failures ? 1 : 0);
})();
