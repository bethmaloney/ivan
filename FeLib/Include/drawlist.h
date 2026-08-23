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

   Capture is opened around the area draw and closed by Flush(), which replays
   the list by calling the same bitmap method that was intercepted. So while
   nothing else changes, the list is a pure interposition: the goldens are the
   proof that it carries everything the renderer needed.

   Only writes whose destination is the capture target are recorded. Blits into
   the scratch bitmaps the map render composites through -- igraph::TileBuffer,
   lsquare::Memorized, lsquare::StaticContentCache -- run immediately, and the
   composite then arrives as one opaque command. §10.1 has the measurements. */

namespace drawlist
{
  enum op
  {
    /* One per intercepted bitmap method. The order is the switch order in
       Replay() and nothing else depends on it. */

    OP_NORMAL_BLIT, OP_LUMINANCE_BLIT, OP_NORMAL_MASKED_BLIT,
    OP_LUMINANCE_MASKED_BLIT, OP_ALPHA_MASKED_BLIT, OP_ALPHA_LUMINANCE_BLIT,
    OP_MASKED_PRIORITY_BLIT, OP_ALPHA_PRIORITY_BLIT, OP_FAST_BLIT,
    OP_FAST_BLIT_POS, OP_FILL, OP_RECTANGLE, OP_CLEAR_TO_COLOR, OP_COUNT
  };

  struct command
  {
    /* Source is the bitmap the method was called on, null for the primitives.
       Data.Bitmap is the destination, and is always the capture target -- it is
       kept rather than implied so that Replay() needs no other state. */

    uchar Op;
    const bitmap* Source;
    blitdata Data;

    /* Fill and DrawRectangle take neither a source nor a blitdata; their
       rectangle rides in Data.Dest/Data.Border and their colour in MaskColor,
       with Wide in CustomData. Reusing the fields keeps the command POD-sized
       rather than adding a union nothing else would use. */
  };

  void Open(bitmap* Target);
  void Flush();
  truth Replaying();

  extern bitmap* Target;
  extern std::vector<command> Commands;
  extern ulong Barriers;   /* flushes forced by a read of the target, §10.1 */

  inline truth Active() { return Target != 0; }
  inline truth IsTarget(const bitmap* What) { return What == Target && Target; }

  void Record(op, const bitmap*, cblitdata&);

  /* The one call each intercepted method makes, as its first statement.
     Returns true when the write was recorded and the caller must return.

     It also carries the read barrier. humanoid::DrawBodyParts blits the
     destination region *into* igraph::TileBuffer, composites the body parts
     over it with a per-pixel priority test and copies the result back
     (human.cpp:2856) -- so the map render reads the double buffer, and a
     pending command that has not been replayed yet would not be in what it
     reads. A read of the capture target flushes first. §10.1 counts them. */

  inline truth Interpose(op Op, const bitmap* Source, cblitdata& Data)
  {
    if(!Active())
      return false;

    if(IsTarget(Source))
      Flush();

    if(!IsTarget(Data.Bitmap))
      return false;

    Record(Op, Source, Data);
    return true;
  }
}

#endif
