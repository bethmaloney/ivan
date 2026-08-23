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

#include "drawlist.h"

bitmap* drawlist::Target = 0;
std::vector<drawlist::command> drawlist::Commands;
bitmap* drawlist::Scratch = 0;
truth drawlist::Taking = false;
ulong drawlist::Barriers = 0;
ulong drawlist::Aliases = 0;
ulong drawlist::Recorded = 0;
ulong drawlist::Renders = 0;
ulong drawlist::Peak = 0;
ulong drawlist::Composites = 0;
ulong drawlist::Taken = 0;
ulong drawlist::Sublists = 0;
const bitmap* (*drawlist::Stabilise)(const bitmap*) = 0;

namespace drawlist
{
  void Record(op, const bitmap*, cblitdata&);
  truth Reaches(const bitmap*, bitmap*);
}

namespace
{
  /* The buffer a taken list grows into, kept between takes. A sublist is a
     local of lsquare::UpdateMemorized, so holding it in the object would start
     every square from capacity 0: 47,019 reallocations over autoplay-2000's
     15,317 takes, against 12 for the frame list, which Commands already keeps
     across windows by being a static itself. */

  std::vector<drawlist::command> TakeBuffer;
}

void drawlist::Open(bitmap* Bitmap)
{
  /* Belt and braces: Close() always empties the list, and ~capture reaches it
     while unwinding too, so a map render that leaves through an exception
     replays the partial list into the target rather than discarding it. */

  Commands.clear();
  Target = Bitmap;
}

void drawlist::Close()
{
  Flush();
  Target = 0;
  ++Renders;
}

void drawlist::Flush()
{
  if(Commands.empty())
    return;

  /* Suspending capture is what stops the replay recording itself, and it is
     needed for more than the blits: DrawRectangle delegates to four line
     calls, each of which is a write method in its own right. */

  bitmap* Suspended = Target;
  Target = 0;

  for(size_t c = 0; c < Commands.size(); ++c)
    Replay(Commands[c]);

  Target = Suspended;
  Commands.clear();
}

void drawlist::Replay(const command& C)
{
  switch(C.Op)
  {
   case OP_NORMAL_BLIT: C.Source->NormalBlit(C.Data); break;
   case OP_LUMINANCE_BLIT: C.Source->LuminanceBlit(C.Data); break;
   case OP_NORMAL_MASKED_BLIT: C.Source->NormalMaskedBlit(C.Data); break;
   case OP_LUMINANCE_MASKED_BLIT: C.Source->LuminanceMaskedBlit(C.Data); break;
   case OP_ALPHA_MASKED_BLIT: C.Source->AlphaMaskedBlit(C.Data); break;
   case OP_ALPHA_LUMINANCE_BLIT: C.Source->AlphaLuminanceBlit(C.Data); break;
   case OP_MASKED_PRIORITY_BLIT: C.Source->MaskedPriorityBlit(C.Data); break;
   case OP_ALPHA_PRIORITY_BLIT: C.Source->AlphaPriorityBlit(C.Data); break;
   case OP_FAST_BLIT: C.Source->FastBlit(C.Data.Bitmap); break;
   case OP_FAST_BLIT_POS: C.Source->FastBlit(C.Data.Bitmap, C.Data.Dest); break;
   case OP_FILL:
    C.Data.Bitmap->Fill(C.Data.Dest, C.Data.Border, C.Data.MaskColor);
    break;
   case OP_RECTANGLE:
    C.Data.Bitmap->DrawRectangle(C.Data.Dest, C.Data.Border, C.Data.MaskColor,
                                 C.Data.CustomData != 0);
    break;
   case OP_CLEAR_TO_COLOR: C.Data.Bitmap->ClearToColor(C.Data.MaskColor); break;
   case OP_ALPHA_PUT_PIXEL:
    C.Data.Bitmap->AlphaPutPixel(C.Data.Dest.X, C.Data.Dest.Y, C.Data.MaskColor,
                                 C.Data.Luminance,
                                 static_cast<alpha>(C.Data.CustomData));
    break;
   case OP_FILL_PRIORITY:
    C.Data.Bitmap->FillPriority(static_cast<priority>(C.Data.CustomData));
    break;
  }
}

void drawlist::Record(op Op, const bitmap* Source, cblitdata& Data)
{
  command C;
  C.Source = Taking ? Stabilise(Source) : Source;
  C.Data = Data;
  C.Op = Op;
  Commands.push_back(C);

  if(Taking)
  {
    ++Taken;
    return;
  }

  ++Recorded;

  if(Commands.size() > Peak)
    Peak = Commands.size();
}

/* The alias barrier. Linear because the list is short -- 65 commands per map
   render on autoplay-2000, 1,783 at its peak -- and because a hit empties it,
   so the scan that found one is the last long one. Every hit the corpora
   produce is fluid::Draw blitting into igraph::FlagBuffer, 983 of them, and
   without this the frame trace moves at line 2,143 (§10.1). Deferring the
   character composite raised that count from 679: a fluid drawn on a body part
   now leaves a command naming FlagBuffer where it used to run at once, and the
   list it waits in is no longer emptied by a read barrier. */

void drawlist::Barrier(const bitmap* Written)
{
  if(Taking)
    return;

  if(Written == Target)
  {
    /* A write into the target the list cannot carry. Replaying first is what
       keeps it in order; it then runs immediately, as it always did. */

    ++Aliases;
    Flush();
    return;
  }

  for(size_t c = 0; c < Commands.size(); ++c)
    if(Commands[c].Source == Written)
    {
      ++Aliases;
      Flush();
      return;
    }
}

/* Whether a write is one the list carries, and the two barriers on the way.
   The destination is tested first, and that ordering is what removed the read
   barrier: humanoid::DrawBodyParts reads the target into the open composite's
   tile, and a write the list carries needs no barrier whatever it reads,
   because the read happens during the replay with everything before it already
   replayed. Only a write that has to run now can be out of order.

   A taken list replays into no target and holds no order to protect, so
   neither barrier fires inside one. */

truth drawlist::Reaches(const bitmap* Source, bitmap* Dest)
{
  if(Taking)
    return Dest == Target;

  if(Dest == Target || Dest == Scratch)
    return true;

  if(Source == Target || (Scratch && Source == Scratch))
  {
    ++Barriers;
    Flush();
  }

  Barrier(Dest);
  return false;
}

truth drawlist::Intercept(op Op, const bitmap* Source, cblitdata& Data)
{
  if(!Reaches(Source, Data.Bitmap))
    return false;

  Record(Op, Source, Data);
  return true;
}

truth drawlist::InterceptFastBlit(const bitmap* Source, bitmap* Dest)
{
  if(!Reaches(Source, Dest))
    return false;

  blitdata B = DEFAULT_BLITDATA;
  B.Bitmap = Dest;
  Record(OP_FAST_BLIT, Source, B);
  return true;
}

truth drawlist::InterceptFastBlitPos(const bitmap* Source, bitmap* Dest, v2 Pos)
{
  if(!Reaches(Source, Dest))
    return false;

  blitdata B = DEFAULT_BLITDATA;
  B.Bitmap = Dest;
  B.Dest = Pos;
  Record(OP_FAST_BLIT_POS, Source, B);
  return true;
}

truth drawlist::InterceptFill(bitmap* Dest, int X, int Y, int Width, int Height,
                              col16 Color)
{
  if(!Reaches(0, Dest))
    return false;

  blitdata B = DEFAULT_BLITDATA;
  B.Bitmap = Dest;
  B.Dest = v2(X, Y);
  B.Border = v2(Width, Height);
  B.MaskColor = Color;
  Record(OP_FILL, 0, B);
  return true;
}

truth drawlist::InterceptRectangle(bitmap* Dest, int Left, int Top, int Right,
                                   int Bottom, col16 Color, truth Wide)
{
  if(!Reaches(0, Dest))
    return false;

  blitdata B = DEFAULT_BLITDATA;
  B.Bitmap = Dest;
  B.Dest = v2(Left, Top);
  B.Border = v2(Right, Bottom);
  B.MaskColor = Color;
  B.CustomData = Wide;
  Record(OP_RECTANGLE, 0, B);
  return true;
}

truth drawlist::InterceptClearToColor(bitmap* Dest, col16 Color)
{
  if(!Reaches(0, Dest))
    return false;

  blitdata B = DEFAULT_BLITDATA;
  B.Bitmap = Dest;
  B.MaskColor = Color;
  Record(OP_CLEAR_TO_COLOR, 0, B);
  return true;
}

truth drawlist::InterceptFillPriority(bitmap* Dest, priority Priority)
{
  if(!Reaches(0, Dest))
    return false;

  blitdata B = DEFAULT_BLITDATA;
  B.Bitmap = Dest;
  B.CustomData = Priority;
  Record(OP_FILL_PRIORITY, 0, B);
  return true;
}

truth drawlist::InterceptAlphaPutPixel(bitmap* Dest, int X, int Y, col16 Color,
                                       col24 Luminance, alpha Alpha)
{
  if(!Reaches(0, Dest))
    return false;

  blitdata B = DEFAULT_BLITDATA;
  B.Bitmap = Dest;
  B.Dest = v2(X, Y);
  B.Luminance = Luminance;
  B.MaskColor = Color;
  B.CustomData = Alpha;
  Record(OP_ALPHA_PUT_PIXEL, 0, B);
  return true;
}

/* Never while Taking: a taken list resolves every source to an identity that
   outlives the frame, and a shared scratch tile has none. Never when the stamp
   would not itself be recorded either -- game::CharacterEntryDrawer composites
   into a felist page, and its parts have to be in the tile before it copies
   them out. */

drawlist::composite::composite(bitmap* Buffer, const bitmap* Into)
: Outer(Scratch)
{
  if(Active() && !Taking && Into == Target)
  {
    Scratch = Buffer;
    ++Composites;
  }
}

drawlist::sublist::sublist(bitmap* Bitmap, const bitmap* (*Stabiliser)(const bitmap*))
: Outer(Target)
{
  Commands.swap(Held);
  Commands.swap(TakeBuffer);
  Target = Bitmap;
  Stabilise = Stabiliser;
  Taking = true;
  ++Sublists;
}

const std::vector<drawlist::command>& drawlist::sublist::Take() const
{
  return Commands;
}

drawlist::sublist::~sublist()
{
  Commands.clear();
  Commands.swap(TakeBuffer);
  Commands.swap(Held);
  Target = Outer;
  Stabilise = 0;
  Taking = false;
}
