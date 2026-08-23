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

   Writes whose destination is the capture target are recorded, and so -- for
   the length of one composite -- are writes into the scratch tile that
   composite builds. humanoid::DrawBodyParts blits the destination region
   *into* igraph::TileBuffer, composites the body parts over it and copies the
   result back (human.cpp:2856), so the map render reads the double buffer;
   recording that group rather than running it is what puts the read back in
   replay order instead of forcing the list out ahead of it (§10.4).

   The group needs no nesting. Every command already carries its own
   destination, so the group's commands sit in the one list in the order they
   were made, and the replay rebuilds the scratch tile and consumes it at
   exactly the point it always was consumed.

   Two barriers keep the rest safe:

     read    a blit whose source is the target, and whose destination the list
             does not carry, has to see pixels the list still owes, so it
             flushes first. The map render no longer contains one: 3,943 on
             autoplay-2000 before the composite window and 0 after. The
             counter stays because that zero is the claim.
     alias   a write into a scratch bitmap that a pending command still names
             as its source would replay the wrong pixels. Such a write flushes
             first. This catches igraph::FlagBuffer, which nothing else does.

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
       Replay(), and it is also the save format: memorized::Save writes the op
       as a byte (§10.3). Append, never renumber. */

    OP_NORMAL_BLIT, OP_LUMINANCE_BLIT, OP_NORMAL_MASKED_BLIT,
    OP_LUMINANCE_MASKED_BLIT, OP_ALPHA_MASKED_BLIT, OP_ALPHA_LUMINANCE_BLIT,
    OP_MASKED_PRIORITY_BLIT, OP_ALPHA_PRIORITY_BLIT, OP_FAST_BLIT,
    OP_FAST_BLIT_POS, OP_FILL, OP_RECTANGLE, OP_CLEAR_TO_COLOR,
    OP_ALPHA_PUT_PIXEL, OP_FILL_PRIORITY, OP_COUNT
  };

  struct command
  {
    /* Source is the bitmap the method was called on, null for the five
       destination-only ops. Data.Bitmap is the destination -- the capture
       target, or an open composite's tile -- and is kept rather than implied,
       which is what lets a composite's commands sit in the same flat list as
       everything else and still replay to the right place.

       The five ops that take no blitdata ride in its fields rather than in a
       union nothing else would use, and each field is the type the argument
       already is:

         OP_FILL             Dest = top left, Border = size, MaskColor = colour
         OP_RECTANGLE        Dest = top left, Border = bottom right,
                             MaskColor = colour, CustomData = Wide
         OP_CLEAR_TO_COLOR   MaskColor = colour
         OP_ALPHA_PUT_PIXEL  Dest = the pixel, MaskColor = the colour,
                             Luminance = the luminance, CustomData = alpha
         OP_FILL_PRIORITY    CustomData = the priority

       Never memcmp or hash a command: blitdata has tail padding this does not
       write (§6.6). */

    const bitmap* Source;
    blitdata Data;
    uchar Op;
  };

  /* drawlist::Target is declared in bitmap.h, where the two inline FastBlits
     need it; the rest of the interface is here. */

  extern std::vector<command> Commands;
  extern bitmap* Scratch;       /* the open composite's tile, or null */
  extern truth Taking;          /* the list is being taken, not replayed */
  extern ulong Barriers;        /* flushes forced by a read of the target */
  extern ulong Aliases;         /* flushes forced by a write to a pending source */
  extern ulong Recorded;        /* commands over the whole run */
  extern ulong Renders;         /* capture windows closed */
  extern ulong Peak;            /* longest list any one window held */
  extern ulong Composites;      /* composite windows opened */
  extern ulong Taken;           /* commands kept rather than replayed */
  extern ulong Sublists;        /* windows they were taken through */

  /* Set for the duration of a taken list. Every source passes through it as it
     is recorded, and the command keeps what it returns. A source the taker
     cannot name by an identity that outlives the frame has to be copied here
     and not at Take(): igraph::FlagBuffer is rewritten one line before the blit
     that reads it, so a copy taken any later is the next command's pixels. */

  extern const bitmap* (*Stabilise)(const bitmap*);

  inline truth Active() { return Target != 0; }

  void Open(bitmap*);
  void Close();
  void Flush();
  void Replay(const command&);

  truth Intercept(op, const bitmap*, cblitdata&);
  truth InterceptFill(bitmap*, int, int, int, int, col16);
  truth InterceptRectangle(bitmap*, int, int, int, int, col16, truth);
  truth InterceptClearToColor(bitmap*, col16);
  truth InterceptAlphaPutPixel(bitmap*, int, int, col16, col24, alpha);
  truth InterceptFillPriority(bitmap*, priority);
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

  inline truth InterposeFillPriority(bitmap* Dest, priority Priority)
  { return Active() && InterceptFillPriority(Dest, Priority); }

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

  /* A composite: for as long as one is open, writes into Buffer are recorded
     alongside the writes into the target instead of running now. The stamp
     that ends the composite is an ordinary command whose source is Buffer, so
     the whole group is deferred together and the shared tile is written and
     read entirely inside the replay. */

  class composite
  {
   public:
    composite(bitmap*, const bitmap*);
    ~composite() { Scratch = Outer; }
   private:
    bitmap* Outer;
  };

  /* A window whose list is taken rather than replayed. lsquare::UpdateMemorized
     records what DrawStaticContents draws and keeps the commands as the
     square's map memory (§10.3) instead of the pixels they would have made.

     Both barriers are off inside one, and that is not a shortcut: nothing will
     be replayed into the target, so there is no order left to keep, and a
     source that is scratch is fatal here rather than merely early, because the
     replay is a turn or a reload away. Take() is what has to notice, by
     resolving every source to an identity that outlives the frame. */

  class sublist
  {
   public:
    sublist(bitmap*, const bitmap* (*)(const bitmap*));
    ~sublist();
    void Take(std::vector<command>&);
   private:
    std::vector<command> Held;
    bitmap* Outer;
  };
}

#endif
