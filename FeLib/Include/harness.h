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
 * --replay, --trace, --seed, --shot, --shot-dir or --text. This header is
 * included from femath.cpp, whandler.cpp, graphics.cpp and rawbit.cpp, all of
 * which are compiled in every #ifdef branch, so it must not pull in SDL,
 * graphics.h or anything from Main.
 */

namespace harness
{
  /* Implementation detail; use the queries below. These are extern so that
     the queries can be inline reads and Rand() pays nothing when disabled. */

  extern truth Recording;
  extern truth Replaying;
  extern truth Tracing;
  extern truth CapturingText;
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

  void ParseArgs(int, char**);
  void Shutdown();

  inline truth IsRecording() { return Recording; }
  inline truth IsReplaying() { return Replaying; }
  inline truth IsTracing() { return Tracing; }
  inline truth IsCapturingText() { return CapturingText; }

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
     discarded - see the comment in harness.cpp. */

  inline void CountRand() { ++RandCount; if(!SeedDepth) ++GameRandCount; }
  inline ulong GetRandCount() { return RandCount; }
  inline ulong GetGameRandCount() { return GameRandCount; }

  /* Called from femath::SaveSeed/LoadSeed only. */

  inline void EnterSeedBracket() { if(SeedDepth++) ++NestedBrackets; }
  inline void LeaveSeedBracket() { if(SeedDepth > 0) --SeedDepth; }

  truth HasSeedOverride();
  ulong GetSeedOverride();
}

#endif
