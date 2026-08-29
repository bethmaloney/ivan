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

#ifndef __IGRAPH_H__
#define __IGRAPH_H__

#include <cstddef>
#include <cstring>
#include <map>
#include <vector>

#include "ivandef.h"
#include "femath.h"

class bitmap;
class rawbitmap;
class outputfile;
class inputfile;
class festring;

struct graphicid
{
  /* Every byte of this object is significant: operator< below memcmps the
     whole struct because it is a std::map key. So padding is a real input to
     the graphics cache ordering, and = default left it uninitialized -
     object::UpdatePictures builds its key on the stack, so the padding carried
     whatever the last call left there. It was an input to the save content too
     until SAVE_FILE_VERSION 139, where the serializer became field by field
     and stopped writing it (igraph.cpp).

     This struct is no longer packed, so it needs one byte of tail padding, and
     Padding below is that byte declared rather than left to the compiler.
     Packing removed it, but packing also removes the alignment guarantee on
     every member, and Colorize() is handed Color, Alpha, RustData and BurnData
     as pointers - see felibdef.h. Zeroing costs one memset per key construction
     and keeps the members aligned. */

  graphicid() { memset(this, 0, sizeof(*this)); }
  bool operator<(const graphicid&) const;
  ushort BitmapPosX;
  ushort BitmapPosY;
  packcol16 Color[4];
  uchar Frame;
  uchar FileIndex;
  ushort SpecialFlags;
  packalpha Alpha[4];
  packalpha BaseAlpha;
  uchar SparkleFrame;
  uchar SparklePosX;
  uchar SparklePosY;
  packcol16 OutlineColor;
  packalpha OutlineAlpha;
  uchar FlyAmount;
  v2 Position;
  uchar RustData[4];
  uchar BurnData[4];
  ushort Seed;
  uchar WobbleData;
  uchar Padding;
};

/* The layout is not the save format any more - that is field by field since
   version 139 - but it is still the layout the browser tile registry will read
   out of the module's memory, and it is still what operator< memcmps.
   Declaration order is guaranteed - all members are public, so [class.mem] puts
   later ones at higher addresses - but the padding is not, and a build flag
   moving it is exactly what docs/port-log.md §7.7 was. These pin both, on every
   target, at compile time. Padding is the last member rather than a hole so
   that sizeof is the sum of the members: with a hole, adding a uchar leaves
   sizeof at 48 and nothing here fires - and a member added without a line here
   is a member the serializer will not write. */

#define GRAPHICID_AT(Field, Offset)\
  static_assert(offsetof(graphicid, Field) == Offset, "graphicid layout moved: " #Field)

GRAPHICID_AT(BitmapPosX, 0);
GRAPHICID_AT(BitmapPosY, 2);
GRAPHICID_AT(Color, 4);
GRAPHICID_AT(Frame, 12);
GRAPHICID_AT(FileIndex, 13);
GRAPHICID_AT(SpecialFlags, 14);
GRAPHICID_AT(Alpha, 16);
GRAPHICID_AT(BaseAlpha, 20);
GRAPHICID_AT(SparkleFrame, 21);
GRAPHICID_AT(SparklePosX, 22);
GRAPHICID_AT(SparklePosY, 23);
GRAPHICID_AT(OutlineColor, 24);
GRAPHICID_AT(OutlineAlpha, 26);
GRAPHICID_AT(FlyAmount, 27);
GRAPHICID_AT(Position, 28);
GRAPHICID_AT(RustData, 36);
GRAPHICID_AT(BurnData, 40);
GRAPHICID_AT(Seed, 44);
GRAPHICID_AT(WobbleData, 46);
GRAPHICID_AT(Padding, 47);

#undef GRAPHICID_AT

static_assert(sizeof(graphicid) == 48, "graphicid size moved");
static_assert(alignof(graphicid) == 4, "graphicid alignment moved");

inline bool graphicid::operator<(const graphicid& GI) const
{
  return memcmp(this, &GI, sizeof(graphicid)) < 0;
}

outputfile& operator<<(outputfile&, const graphicid&);
inputfile& operator>>(inputfile&, graphicid&);

struct tile
{
  tile() = default;
  tile(bitmap* Bitmap) : Bitmap(Bitmap), Users(1) { }
  bitmap* Bitmap;
  long Users;
};

typedef std::map<graphicid, tile> tilemap;

struct graphicdata
{
  graphicdata() : AnimationFrames(0) { }
  ~graphicdata();
  void Save(outputfile&) const;
  void Load(inputfile&);
  void Retire();
  int AnimationFrames;
  bitmap** Picture;
  tilemap::iterator* GraphicIterator;
};

outputfile& operator<<(outputfile&, const graphicdata&);
inputfile& operator>>(inputfile&, graphicdata&);

class igraph
{
 public:
  static void Init();
  static void DeInit();
  static cbitmap* GetWTerrainGraphic() { return Graphic[GR_WTERRAIN]; }
  static cbitmap* GetFOWGraphic() { return Graphic[GR_FOW]; }
  static const rawbitmap* GetCursorRawGraphic() { return RawGraphic[GR_CURSOR]; }
  static cbitmap* GetSymbolGraphic() { return Graphic[GR_SYMBOL]; }
  static bitmap* GetTileBuffer() { return TileBuffer; }
  static void DrawCursor(v2, int, int = 0);
  static tilemap::iterator AddUser(const graphicid&);
  static void AddUser(tilemap::iterator I) { ++I->second.Users; }
  static void RemoveUser(tilemap::iterator);

  /* The tilemap read backwards. A recorded draw command holds the bitmap the
     blit was called on, and a saved one cannot: lsquare's map memory needs the
     graphicid that minted the tile, which is the only identity for a tile that
     survives a save. Maintained by AddUser and RemoveUser, so it is exactly as
     long as TileMap. */

  static truth IdentifyTile(const bitmap*, tilemap::iterator&);
  static const rawbitmap* GetHumanoidRawGraphic() { return RawGraphic[GR_HUMANOID]; }
  static const rawbitmap* GetCharacterRawGraphic() { return RawGraphic[GR_CHARACTER]; }
  static const rawbitmap* GetEffectRawGraphic() { return RawGraphic[GR_EFFECT]; }
  static const rawbitmap* GetRawGraphic(int I) { return RawGraphic[I]; }
  static cbitmap* GetGraphic(int I) { return Graphic[I]; }
  static int IdentifyGraphic(const bitmap*);
  static cint* GetBodyBitmapValidityMap(int);
  static bitmap* GetFlagBuffer() { return FlagBuffer; }
  static std::vector<bitmap*> GetMenuGraphic() { return vMenu; }
  static void LoadMenu();
  static void UnLoadMenu();
  static bitmap* GetSilhouetteCache(int I1, int I2, int I3) { return SilhouetteCache[I1][I2][I3]; }
  static cbitmap* GetBackGround() { return BackGround; }
  static void BlitBackGround(v2, v2);
  static void BlitBackGround(bitmap* bmpAt, v2 Pos, v2 Border);
  static void CreateBackGround(int);
  static bitmap* GenerateScarBitmap(int, int, int);
  static cbitmap* GetSmileyGraphic() { return Graphic[GR_SMILEY]; }
  static void AddOutlinesIfNeeded();
 private:
  static void EditBodyPartTile(rawbitmap*, rawbitmap*, v2, int);
  static v2 RotateTile(rawbitmap*, rawbitmap*, v2, v2, int);
  static void CreateBodyBitmapValidityMaps();
  static void CreateSilhouetteCaches();
  static col16 GetBackGroundColor(int);
  static rawbitmap* RawGraphic[RAW_TYPES];
  static bitmap* Graphic[GRAPHIC_TYPES];
  static bitmap* TileBuffer;
  static cchar* RawGraphicFileName[];
  static cchar* GraphicFileName[];
  static tilemap TileMap;
  static std::map<const bitmap*, tilemap::iterator> TileByBitmap;
  static uchar RollBuffer[256];
  static bitmap* FlagBuffer;
  static int** BodyBitmapValidityMap;
  static std::vector<bitmap*> vMenu;
  static bitmap* SilhouetteCache[HUMANOID_BODYPARTS][CONDITION_COLORS][SILHOUETTE_TYPES];
  static rawbitmap* ColorizeBuffer[2];
  static bitmap* Cursor[CURSOR_TYPES];
  static bitmap* BigCursor[CURSOR_TYPES];
  static col16 CursorColor[CURSOR_TYPES];
  static bitmap* BackGround;
  static int CurrentColorType;
};

#endif
