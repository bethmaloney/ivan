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

#include "memorized.h"

#include "bitmap.h"
#include "ivandef.h"
#include "save.h"

namespace
{
  /* What a source resolved to, one per recorded command and in the order the
     commands were recorded. Filled by Stabilise at record time because that is
     the only moment a scratch source holds the pixels the command meant, and
     drained by record::Commit. */

  struct resolved
  {
    uchar Kind;
    uchar Graphic;
    tilemap::iterator Tile;
    bitmap* Pixels;
  };

  std::vector<resolved> Resolution;
  bitmap* CaptureSurface = 0;
}

/* The destination DrawStaticContents is pointed at while its commands are being
   taken. Its pixels are never written: a taken list is not replayed into the
   target, and drawlist.h's fourteen recording points are every write method the
   map render can reach, so nothing composites into it behind the list's back.
   It exists only to be a pointer the intercept can compare against. */

bitmap* memorized::Surface()
{
  if(!CaptureSurface)
    CaptureSurface = new bitmap(TILE_V2);

  return CaptureSurface;
}

const bitmap* memorized::Stabilise(const bitmap* Source)
{
  resolved R;
  R.Graphic = 0;
  R.Pixels = 0;

  if(!Source)
    R.Kind = SRC_NONE;
  else if(igraph::IdentifyTile(Source, R.Tile))
    R.Kind = SRC_TILE;
  else
  {
    cint Graphic = igraph::IdentifyGraphic(Source);

    if(Graphic >= 0)
    {
      R.Kind = SRC_GRAPHIC;
      R.Graphic = Graphic;
    }
    else
    {
      /* Nothing names this one, so the memory has to own the pixels. The copy
         carries the alpha map -- an alpha blit needs it -- but not the
         priority map, and that is not an omission: a priority blit falls back
         the moment either side lacks one, and the destination is always the
         double buffer, which has none. FastFlag because every tile
         igraph::AddUser mints has it, and the memorized draw has never
         clipped. */

      R.Kind = SRC_PIXELS;
      R.Pixels = new bitmap(Source, 0, true);
      R.Pixels->ActivateFastFlag();
      Source = R.Pixels;
    }
  }

  Resolution.push_back(R);
  return Source;
}

memorized::record::record() : Sub(memorized::Surface(), memorized::Stabilise)
{
  Resolution.clear();
}

memorized::record::~record()
{
  /* Anything Commit did not take. A DrawStaticContents that left through an
     exception leaves the old memory standing and these copies unowned. */

  for(size_t c = 0; c < Resolution.size(); ++c)
    delete Resolution[c].Pixels;

  Resolution.clear();
}

void memorized::record::Commit(memorized* Into)
{
  std::vector<drawlist::command> Command;
  Sub.Take(Command);
  Into->Take(Command);
  Resolution.clear();
}

/* The new list takes its users before the old one gives its own up, because a
   tile the new composite draws is usually one the old memory also held:
   releasing first would delete it out from under an iterator Stabilise took
   while it was still alive. */

void memorized::Take(std::vector<drawlist::command>& Command)
{
  std::vector<entry> Fresh;
  Fresh.resize(Command.size());

  for(size_t c = 0; c < Command.size(); ++c)
  {
    const drawlist::command& C = Command[c];
    resolved& R = Resolution[c];
    entry& E = Fresh[c];
    E.Source = C.Source;
    E.Kind = R.Kind;
    E.Graphic = R.Graphic;
    E.Pixels = R.Pixels;
    E.Tile = R.Tile;
    E.Op = C.Op;
    E.Src = C.Data.Src;
    E.Dest = C.Data.Dest;
    E.Border = C.Data.Border;
    E.Luminance = C.Data.Luminance;
    E.MaskColor = C.Data.MaskColor;
    E.CustomData = C.Data.CustomData;
    R.Pixels = 0;

    if(E.Kind == SRC_TILE)
      igraph::AddUser(E.Tile);

    /* The two ops that mean "all of it" cannot be replayed at an offset. Both
       are exact rewrites while the capture surface is one tile: a whole-bitmap
       FastBlit of a 16x16 is the positioned one at the origin, and clearing a
       16x16 is filling it. Neither is reached by the corpora; they are here so
       that a source that starts using one is drawn rather than misplaced. */

    if(E.Op == drawlist::OP_FAST_BLIT)
      E.Op = drawlist::OP_FAST_BLIT_POS;
    else if(E.Op == drawlist::OP_CLEAR_TO_COLOR)
    {
      E.Op = drawlist::OP_FILL;
      E.Border = TILE_V2;
    }
  }

  Clear();
  Entry.swap(Fresh);
}

void memorized::Clear()
{
  for(size_t c = 0; c < Entry.size(); ++c)
  {
    if(Entry[c].Kind == SRC_TILE)
      igraph::RemoveUser(Entry[c].Tile);
    else if(Entry[c].Kind == SRC_PIXELS)
      delete Entry[c].Pixels;
  }

  Entry.clear();
}

/* The square that was dark and could not be felt. It used to be a
   ClearToColor(0) over the memorized bitmap; as a command it is the fill that
   was always implied. */

void memorized::Darken()
{
  Clear();
  Entry.resize(1);
  entry& E = Entry[0];
  E.Source = 0;
  E.Pixels = 0;
  E.Kind = SRC_NONE;
  E.Graphic = 0;
  E.Op = drawlist::OP_FILL;
  E.Src = ZERO_V2;
  E.Dest = ZERO_V2;
  E.Border = TILE_V2;
  E.Luminance = NORMAL_LUMINANCE;
  E.MaskColor = 0;
  E.CustomData = 0;
}

/* Two luminances are two per-channel offsets, and the blitter keeps them in
   channel steps: (R >> 2) - 32 for red and blue, (G >> 1) - 64 for green
   (bitmap.cpp:542-544). Composing them is adding the step counts and clamping
   once, and NORMAL_LUMINANCE is the identity of that -- which is why the early
   return is exact rather than a shortcut, and why it is the only branch the
   corpora take: every command a composite records is recorded at
   NORMAL_LUMINANCE, and the stack's symbols only leave it when Contrast does. */

col24 memorized::Combine(col24 Recorded, col24 Asked)
{
  if(Recorded == NORMAL_LUMINANCE)
    return Asked;

  int Red = (int(Recorded >> 16 & 0xFF) >> 2) + (int(Asked >> 16 & 0xFF) >> 2) - 32;
  int Green = (int(Recorded >> 8 & 0xFF) >> 1) + (int(Asked >> 8 & 0xFF) >> 1) - 64;
  int Blue = (int(Recorded & 0xFF) >> 2) + (int(Asked & 0xFF) >> 2) - 32;

  Red = Red < 0 ? 0 : Red > 63 ? 63 : Red;
  Green = Green < 0 ? 0 : Green > 127 ? 127 : Green;
  Blue = Blue < 0 ? 0 : Blue > 63 ? 63 : Blue;

  return col24(Red) << 18 | col24(Green) << 9 | col24(Blue) << 2;
}

void memorized::Draw(blitdata& BlitData) const
{
  drawlist::command C;
  C.Data = DEFAULT_BLITDATA;
  C.Data.Bitmap = BlitData.Bitmap;

  for(size_t c = 0; c < Entry.size(); ++c)
  {
    const entry& E = Entry[c];
    C.Source = E.Source;
    C.Op = E.Op;
    C.Data.Src = E.Src;
    C.Data.Dest = BlitData.Dest + E.Dest;
    C.Data.Border = E.Border;
    C.Data.Luminance = Combine(E.Luminance, BlitData.Luminance);
    C.Data.MaskColor = E.MaskColor;
    C.Data.CustomData = E.CustomData;
    drawlist::Replay(C);
  }
}

/* What the second memorized bitmap used to hold. It was the composite copied
   whole and the fog masked over it, then the pair luminated together on the
   way to the screen; here the fog is one more masked blit at the same
   luminance, over the same commands. */

void memorized::DrawFogged(blitdata& BlitData) const
{
  Draw(BlitData);

  blitdata B = DEFAULT_BLITDATA;
  B.Bitmap = BlitData.Bitmap;
  B.Dest = BlitData.Dest;
  B.Border = TILE_V2;
  B.Luminance = BlitData.Luminance;
  B.MaskColor = 0;
  igraph::GetFOWGraphic()->LuminanceMaskedBlit(B);
}

/* Field by field, and the widths are the ones both targets agree on: short and
   ushort go out as explicit little-endian bytes and int is four bytes
   everywhere the game builds. The one struct written whole is graphicid, whose
   layout is pinned member by member at compile time on every target
   (igraph.h:81-107) and which is already the save's encoding for a tile. */

void memorized::Save(outputfile& SaveFile) const
{
  SaveFile << ushort(Entry.size());

  for(size_t c = 0; c < Entry.size(); ++c)
  {
    const entry& E = Entry[c];
    SaveFile << E.Kind << E.Op;

    switch(E.Kind)
    {
     case SRC_TILE: SaveFile << E.Tile->first; break;
     case SRC_GRAPHIC: SaveFile << E.Graphic; break;
     case SRC_PIXELS: SaveFile << static_cast<cbitmap*>(E.Pixels); break;
    }

    SaveFile << short(E.Src.X) << short(E.Src.Y)
             << short(E.Dest.X) << short(E.Dest.Y)
             << short(E.Border.X) << short(E.Border.Y);
    SaveFile << uint(E.Luminance) << ushort(E.MaskColor) << uint(E.CustomData);
  }
}

void memorized::Load(inputfile& SaveFile)
{
  Clear();
  Entry.resize(ReadType<ushort>(SaveFile));

  for(size_t c = 0; c < Entry.size(); ++c)
  {
    entry& E = Entry[c];
    E.Source = 0;
    E.Pixels = 0;
    E.Graphic = 0;
    SaveFile >> E.Kind >> E.Op;

    switch(E.Kind)
    {
     case SRC_TILE:
     {
       graphicid GI;
       SaveFile >> GI;
       E.Tile = igraph::AddUser(GI);
       E.Source = E.Tile->second.Bitmap;
       break;
     }
     case SRC_GRAPHIC:
      SaveFile >> E.Graphic;
      E.Source = igraph::GetGraphic(E.Graphic);
      break;
     case SRC_PIXELS:
      SaveFile >> E.Pixels;
      E.Source = E.Pixels;
      break;
    }

    E.Src.X = ReadType<short>(SaveFile);
    E.Src.Y = ReadType<short>(SaveFile);
    E.Dest.X = ReadType<short>(SaveFile);
    E.Dest.Y = ReadType<short>(SaveFile);
    E.Border.X = ReadType<short>(SaveFile);
    E.Border.Y = ReadType<short>(SaveFile);
    E.Luminance = ReadType<uint>(SaveFile);
    E.MaskColor = ReadType<ushort>(SaveFile);
    E.CustomData = ReadType<uint>(SaveFile);
  }
}

outputfile& operator<<(outputfile& SaveFile, const memorized* Memorized)
{
  if(Memorized)
  {
    SaveFile.Put(1);
    Memorized->Save(SaveFile);
  }
  else
    SaveFile.Put(0);

  return SaveFile;
}

inputfile& operator>>(inputfile& SaveFile, memorized*& Memorized)
{
  if(SaveFile.Get())
  {
    Memorized = new memorized;
    Memorized->Load(SaveFile);
  }
  else
    Memorized = 0;

  return SaveFile;
}
