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

#ifndef __DRAWLIST_H__
#define __DRAWLIST_H__

#include <vector>

#include "bitmap.h"
#include "typedef.h"

/* A command buffer between the map render and the double buffer.
   docs/port-log.md §10.1; PORTING.md's seam 2.

   Capture is opened around the area draw and closed by Close(), which replays
   the list by calling the same bitmap method that was intercepted. So while
   nothing else changes, the list is a pure interposition: the goldens are the
   proof that it carries everything the renderer needed.

   Every intercepted method calls Interpose as its first statement and returns
   if it returns true. Because it returns *before* delegating, the delegation
   -- LuminanceBlit to NormalBlit, AlphaPriorityBlit to either of two others --
   is never recorded on top of the outer call, and replaying the outer call
   re-delegates naturally. That is what makes the capture complete by
   construction rather than by enumeration, the argument harness.cpp uses for
   its two text funnels. Its cost is that the delegation is chosen at replay
   time from live state (Source->AlphaMap, Target->PriorityMap): nothing may
   add or remove either between the record and the Flush.

   Only writes whose destination is the capture target are recorded. Writes
   into the scratch bitmaps the map render composites through -- igraph's
   TileBuffer and FlagBuffer, lsquare::Memorized, lsquare::StaticContentCache
   -- run immediately, and the composite then arrives as one opaque command.

   Two barriers keep that safe, and both are counted because Phase C exists to
   drive the first of them to zero:

     read    humanoid::DrawBodyParts blits the destination region *into*
             TileBuffer, composites the body parts over it and copies the
             result back (human.cpp:2856), so the map render reads the double
             buffer. A blit whose source is the target flushes first.
     alias   a write into a scratch bitmap that a pending command still names
             as its source would replay the wrong pixels. Such a write flushes
             first. This is the general form of the read barrier and catches
             igraph::FlagBuffer, which no read of the target protects.

   Deferring is only sound because the list replays into one destination in the
   recorded order with nothing else writing in between: every Alpha* blit and
   AlphaPutPixel reads the destination pixel it blends onto. Reordering the
   list, batching it by source or dropping overpainted commands is therefore
   not free, whatever a profile suggests. */

namespace drawlist
{
  enum op
  {
    /* One per intercepted bitmap method. The order is the switch order in
       Replay() and nothing else depends on it. */

    OP_NORMAL_BLIT, OP_LUMINANCE_BLIT, OP_NORMAL_MASKED_BLIT,
    OP_LUMINANCE_MASKED_BLIT, OP_ALPHA_MASKED_BLIT, OP_ALPHA_LUMINANCE_BLIT,
    OP_MASKED_PRIORITY_BLIT, OP_ALPHA_PRIORITY_BLIT, OP_FAST_BLIT,
    OP_FAST_BLIT_POS, OP_FILL, OP_RECTANGLE, OP_CLEAR_TO_COLOR,
    OP_ALPHA_PUT_PIXEL, OP_COUNT
  };

  struct command
  {
    /* Source is the bitmap the method was called on, null for the four
       destination-only ops. Data.Bitmap is the destination and is always the
       capture target -- it is kept rather than implied so that Replay() needs
       no other state.

       The four ops that take no blitdata ride in its fields rather than in a
       union nothing else would use, and each field is the type the argument
       already is:

         OP_FILL             Dest = top left, Border = size, MaskColor = colour
         OP_RECTANGLE        Dest = top left, Border = bottom right,
                             MaskColor = colour, CustomData = Wide
         OP_CLEAR_TO_COLOR   MaskColor = colour
         OP_ALPHA_PUT_PIXEL  Dest = the pixel, MaskColor = the colour,
                             Luminance = the luminance, CustomData = alpha

       Never memcmp or hash a command: blitdata has tail padding this does not
       write (§6.6). */

    const bitmap* Source;
    blitdata Data;
    uchar Op;
  };

  /* drawlist::Target is declared in bitmap.h, where the two inline FastBlits
     need it; the rest of the interface is here. */

  extern std::vector<command> Commands;
  extern ulong Barriers;        /* flushes forced by a read of the target */
  extern ulong Aliases;         /* flushes forced by a write to a pending source */
  extern ulong Recorded;        /* commands over the whole run */
  extern ulong Renders;         /* capture windows closed */
  extern ulong Peak;            /* longest list any one window held */

  inline truth Active() { return Target != 0; }

  void Open(bitmap*);
  void Close();
  void Flush();

  truth Intercept(op, const bitmap*, cblitdata&);
  truth InterceptFill(bitmap*, int, int, int, int, col16);
  truth InterceptRectangle(bitmap*, int, int, int, int, col16, truth);
  truth InterceptClearToColor(bitmap*, col16);
  truth InterceptAlphaPutPixel(bitmap*, int, int, col16, col24, alpha);
  void Barrier(const bitmap*);

  /* The call each intercepted method makes as its first statement. Inactive
     costs one load of a static pointer and a branch, which is the whole reason
     the work is behind an out-of-line Intercept: this ships in the game. */

  inline truth Interpose(op Op, const bitmap* Source, cblitdata& Data)
  { return Active() && Intercept(Op, Source, Data); }

  inline truth InterposeFill(bitmap* Dest, int X, int Y, int W, int H, col16 Color)
  { return Active() && InterceptFill(Dest, X, Y, W, H, Color); }

  inline truth InterposeRectangle(bitmap* Dest, int L, int T, int R, int B,
                                  col16 Color, truth Wide)
  { return Active() && InterceptRectangle(Dest, L, T, R, B, Color, Wide); }

  inline truth InterposeClearToColor(bitmap* Dest, col16 Color)
  { return Active() && InterceptClearToColor(Dest, Color); }

  inline truth InterposeAlphaPutPixel(bitmap* Dest, int X, int Y, col16 Color,
                                      col24 Luminance, alpha Alpha)
  { return Active() && InterceptAlphaPutPixel(Dest, X, Y, Color, Luminance, Alpha); }

  /* For a write the command struct cannot express. It is not recorded: the
     list is flushed so that the write lands in order, and the write then runs
     as it always did. BlitAndCopyAlpha and FastBlitAndCopyAlpha write two
     planes, and neither can reach the double buffer -- it has no alpha map, so
     both ABORT -- but both write igraph::FlagBuffer, which is where the alias
     barrier earns its keep. */

  inline void InterposeBarrier(const bitmap* Dest)
  { if(Active()) Barrier(Dest); }

  /* The capture window. RAII because IVAN throws as ordinary control flow: a
     list left unflushed would outlive the bitmaps its commands point at. */

  class capture
  {
   public:
    capture(bitmap* Bitmap) { Open(Bitmap); }
    ~capture() { Close(); }
  };
}

#endif
