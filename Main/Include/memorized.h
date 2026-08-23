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

#ifndef __MEMORIZED_H__
#define __MEMORIZED_H__

#include <vector>

#include "drawlist.h"
#include "igraph.h"

class inputfile;
class outputfile;

/* A square's map memory, held as the draw commands lsquare::DrawStaticContents
   emitted rather than the 16x16 of pixels they made. docs/port-log.md §10.3.

   The composite is therefore replayed at the luminance the square has when it
   is drawn, where it used to be composited once at NORMAL_LUMINANCE and
   luminated whole on the way out. That is not pixel-neutral and was never
   going to be: the luminance macros are a clamped per-channel add, so clamping
   once at the end and clamping every operand are different sums.

   A saved command cannot hold a bitmap pointer, so every source is resolved to
   an identity that outlives both the frame and the process:

     SRC_TILE     a tile igraph::AddUser minted, named by its graphicid and
                  held by a user count of its own -- the memory of a wall has
                  to outlive the wall, which is the whole point of it.
     SRC_GRAPHIC  one of igraph's whole-sheet graphics, named by its index.
                  Reached by the altar symbol and by the stack's plus and
                  danger symbols, which blit out of Graphics/Symbol.png with
                  Src set.
     SRC_PIXELS   a bitmap graphicid cannot name, copied and owned. Two sources
                  reach it: fluid::imagedata::Picture and web::Picture, both
                  generated per entity and already serialised as raw pixels by
                  their owners. It also catches the shared scratches -- item's
                  rotation bitmap, igraph::FlagBuffer -- because the copy is
                  taken at record time, which is the only moment their pixels
                  are the ones the command meant.
     SRC_NONE     a command with no source at all, which today is only the fill
                  that stands for a square too dark to be felt.

   Measured over autoplay-2000: 72,930 commands in 15,317 composites, 4.76 per
   composite, of which 92.2% SRC_TILE, 0.5% SRC_GRAPHIC and 7.3% SRC_PIXELS.
   Four ops account for all of them -- LuminanceBlit 13,108, AlphaLuminanceBlit
   57,860, AlphaPriorityBlit 1,090, LuminanceMaskedBlit 872 -- and every one of
   the four takes a luminance, which is what makes replaying at the square's
   own luminance a rewrite of the composite rather than a loss. */

class memorized
{
 public:
  memorized() { }
  ~memorized() { Clear(); }
  void Darken();
  void Draw(blitdata&) const;
  void DrawFogged(blitdata&) const;
  void Save(outputfile&) const;
  void Load(inputfile&);
  static bitmap* Surface();

  /* The capture window. Commit() is separate from the destructor so that a
     DrawStaticContents leaving through an exception leaves the old memory
     standing rather than half of a new one. */

  class record
  {
   public:
    record();
    ~record();
    void Commit(memorized*);
   private:
    drawlist::sublist Sub;
  };

 private:
  enum { SRC_NONE, SRC_TILE, SRC_GRAPHIC, SRC_PIXELS };

  struct entry
  {
    const bitmap* Source;    /* resolved at record or load time, never saved */
    tilemap::iterator Tile;  /* SRC_TILE, and this entry holds one of its users */
    bitmap* Pixels;          /* SRC_PIXELS, owned */
    uchar Graphic;           /* SRC_GRAPHIC */
    uchar Kind;
    uchar Op;
    v2 Src, Dest, Border;
    col24 Luminance;
    col16 MaskColor;
    ulong CustomData;
  };

  void Clear();
  void Take(std::vector<drawlist::command>&);
  static const bitmap* Stabilise(const bitmap*);
  static col24 Combine(col24, col24);
  memorized(const memorized&);
  memorized& operator=(const memorized&);

  std::vector<entry> Entry;
};

outputfile& operator<<(outputfile&, const memorized*);
inputfile& operator>>(inputfile&, memorized*&);

#endif
