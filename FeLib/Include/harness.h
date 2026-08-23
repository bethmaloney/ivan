/*
 *
 *  Iter Vehemens ad Necem (IVAN)
 *  Copyright (C) Timo Kiviluoto
 *  Released under the GNU General
 *  Public License
 *
 *  See LICENSING which should be included
 *  along with this file for more details
 *
 */

#ifndef __HARNESS_H__
#define __HARNESS_H__

#include "typedef.h"

/*
 * Differential test harness: deterministic input record/replay, per frame
 * hashing of the double buffer, RNG call counting and screen capture.
 *
 * Everything here is inert until harness::ParseArgs() sees one of --record,
 * --replay, --trace, --seed, --shot, --shot-dir, --text or --headless. This
 * header is included from femath.cpp, whandler.cpp, graphics.cpp, sfx.cpp and
 * rawbit.cpp, all of which are compiled in every #ifdef branch, so it must not
 * pull in SDL, graphics.h or anything from Main.
 */

namespace harness
{
  /* Implementation detail; use the queries below. These are extern so that
     the queries can be inline reads and Rand() pays nothing when disabled. */

  extern truth Recording;
  extern truth Replaying;
  extern truth Tracing;
  extern truth CapturingText;
  extern truth Headless;
  extern ulong RandCount;

  /* Diagnostic attribution of RNG draws. RandCount counts every MT draw;
     GameRandCount counts only those made outside a femath::SaveSeed/LoadSeed
     bracket, which are the ones that actually advance the game stream. Draws
     inside a bracket are discarded by LoadSeed and cannot affect game state -
     unless the brackets nest, which the single mtb backup slot cannot express.
     NestedBrackets counts every SaveSeed entered at depth > 0. */

  extern ulong GameRandCount;
  extern int SeedDepth;
  extern ulong NestedBrackets;

  /* The presentation generator's draws (visualrand, femath.h). Counted apart
     because they are not evidence of anything the game did: two runs that
     differ here and agree on GameRandCount are two runs of the same game. */

  extern ulong VisualRandCount;

  void ParseArgs(int, char**);
  void Shutdown();

  inline truth IsRecording() { return Recording; }
  inline truth IsReplaying() { return Replaying; }
  inline truth IsTracing() { return Tracing; }
  inline truth IsCapturingText() { return CapturingText; }

  /*
   * --headless: run with no display and no audio device. Every SDL call that
   * needs one is skipped - the window, the renderer, the streaming texture,
   * the final blit and Mix_OpenAudio - and nothing else is. The game still
   * renders in full, because rendering is software into the bitmap double
   * buffer and only the last blit ever reaches the GPU, so TraceFrame() still
   * hashes exactly what it hashes with a window open.
   *
   * This is what lets the WASM build run under bare node: Emscripten's SDL2
   * port binds to the DOM at video init (emscripten_get_screen_size wants
   * `screen`) and its audio backend wants an AudioContext, neither of which
   * exists outside a browser. Native and WASM then run the same path, which
   * is the point - a frame comparison across two platforms is only evidence
   * if both took the same route to the frame.
   */

  inline truth IsHeadless() { return Headless; }

  void RecordKey(int);
  truth NextReplayKey(int&);

  void TraceFrame();

  /*
   * Screen capture. The point of these is that a headless replay is otherwise
   * unreviewable: a frame hash proves two runs agree but says nothing about
   * what is on the screen, so there is no way to decide what to press next.
   *
   * RecordText is the text layer. Every glyph IVAN draws goes through
   * rawbitmap::Printf/PrintfUnshaded, so logging the strings there recovers
   * the message log, the side panel, menus and prompts as real text rather
   * than as 8x8 pixel blocks in a screenshot. Target is the bitmap* being
   * drawn into, kept opaque here so this header stays free of bitmap.h; the
   * implementation compares it against DOUBLE_BUFFER to tell text drawn
   * straight to the screen from text drawn into an offscreen buffer.
   */

  void RecordText(const void* Target, int X, int Y, int Color, cchar* Text);

  /* Writes the double buffer as a PNG, plus a sidecar .txt holding the text
     layer of the same frame. Reason is stamped into the sidecar. */

  void WriteShot(cchar* FileName, cchar* Reason);

  /* Rand() is called millions of times per level generation, so this counts
     unconditionally rather than branching. It counts every MT draw, including
     the ones made inside femath::SaveSeed/LoadSeed brackets that are later
     discarded - see the comment in harness.cpp. It does not see the
     presentation generator at all; CountVisualRand does. */

  inline void CountRand() { ++RandCount; if(!SeedDepth) ++GameRandCount; }
  inline void CountVisualRand() { ++VisualRandCount; }
  inline ulong GetRandCount() { return RandCount; }
  inline ulong GetGameRandCount() { return GameRandCount; }
  inline ulong GetVisualRandCount() { return VisualRandCount; }

  /* Called from femath::SaveSeed/LoadSeed only. */

  inline void EnterSeedBracket() { if(SeedDepth++) ++NestedBrackets; }
  inline void LeaveSeedBracket() { if(SeedDepth > 0) --SeedDepth; }

  truth HasSeedOverride();
  ulong GetSeedOverride();

  /* --visual-seed. Unlike the game seed this always has a value, because a run
     that pins nothing else still has to reproduce its own frames. */

  ulong GetVisualSeed();
}

#endif
