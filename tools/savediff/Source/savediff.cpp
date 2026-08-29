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

/* savediff compares two saved games and reports where they diverge. It is the
 * oracle half of the differential test harness: run the same input script
 * against two builds, save at the same point in both, and diff the results.
 *
 * The tool is deliberately standalone. It links nothing - not FeLib, not SDL,
 * not libpng - because it has to work when the game build is broken and it has
 * to read saves produced by a different build of the game entirely. The only
 * IVAN header it uses is typedef.h, which is header-only.
 *
 * The save format is raw binary with no field tags (see FeLib/Include/save.h),
 * so only the leading fields of each file can be labelled. Everything past the
 * first polymorphic field is reported as byte offsets. */

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>

#ifdef WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#include "typedef.h"

#define SAVEDIFF_VERSION "1.0"
#define KNOWN_SAVE_FILE_VERSION 139
#define BLOCK_SIZE 4096
#define MAX_AREA_SIDE 4096
#define MAX_AREA_SQUARES 4000000
#define MAX_ENTRY_COUNT 65536
#define MAX_TEAM_COUNT 4096
#define MAX_RELATION_COUNT 65536
#define KNOWN_DUNGEONS 15
#define WORLD_MAP_INDEX 255 /* Main/Include/ivandef.h WORLD_MAP */
#define MAX_KNOWN_LEVEL 12
#define TIME_SPENT_LIMIT 10000000

enum { KIND_SAV, KIND_WM, KIND_LEVEL, KIND_OTHER };
enum { V_SAME, V_DIFF, V_SUSPECT, V_ONLY_A, V_ONLY_B };
enum { T_I32, T_U8, T_V2, T_LONG, T_ULONG, T_F64 };

static int   WordSize = 8;
static int   Window = 64;
static truth SummaryMode = false;
static truth JsonMode = false;
static truth IncludeBackups = false;
static truth IgnoreTimeSpent = false;

static std::string Format(cchar* Fmt, ...)
{
  char Buffer[512];
  va_list AP;
  va_start(AP, Fmt);
  vsnprintf(Buffer, sizeof(Buffer), Fmt, AP);
  va_end(AP);
  return std::string(Buffer);
}

/* Raw readers. RAW_SAVE_LOAD dumps object representations straight to disk, so
 * these are host-endian by construction - exactly like the game's own loader. */

static truth Fits(const std::vector<uchar>& Data, ulong Offset, ulong Size)
{
  return Offset + Size >= Offset && Offset + Size <= Data.size();
}

static int32_t GetI32(const std::vector<uchar>& Data, ulong Offset)
{
  int32_t Value = 0;
  memcpy(&Value, &Data[Offset], 4);
  return Value;
}

static int64_t GetI64(const std::vector<uchar>& Data, ulong Offset)
{
  int64_t Value = 0;
  memcpy(&Value, &Data[Offset], 8);
  return Value;
}

static uint64_t GetU64(const std::vector<uchar>& Data, ulong Offset)
{
  uint64_t Value = 0;
  memcpy(&Value, &Data[Offset], 8);
  return Value;
}

static double GetF64(const std::vector<uchar>& Data, ulong Offset)
{
  double Value = 0;
  memcpy(&Value, &Data[Offset], 8);
  return Value;
}

static int64_t GetLong(const std::vector<uchar>& Data, ulong Offset)
{
  return WordSize == 4 ? GetI32(Data, Offset) : GetI64(Data, Offset);
}

static uint64_t GetULong(const std::vector<uchar>& Data, ulong Offset)
{
  return WordSize == 4 ? uint64_t(uint32_t(GetI32(Data, Offset))) : GetU64(Data, Offset);
}

static int TypeSize(int Type)
{
  switch(Type)
  {
   case T_I32: return 4;
   case T_U8: return 1;
   case T_V2: return 8;
   case T_LONG: case T_ULONG: return WordSize;
   case T_F64: return 8;
  }

  return 0;
}

static std::string RenderType(const std::vector<uchar>& Data, ulong Offset, int Type)
{
  switch(Type)
  {
   case T_I32: return Format("%d", GetI32(Data, Offset));
   case T_U8: return Format("%u", Data[Offset]);
   case T_V2: return Format("(%d,%d)", GetI32(Data, Offset), GetI32(Data, Offset + 4));
   case T_LONG: return Format("%lld", (long long)GetLong(Data, Offset));
   case T_ULONG: return Format("%llu", (unsigned long long)GetULong(Data, Offset));
   case T_F64: return Format("%.17g", GetF64(Data, Offset));
  }

  return "?";
}

/* A labelled scalar in a decoded prefix. */

struct field
{
  field(const std::string& Name, ulong Offset, ulong Size, const std::string& Text)
  : Name(Name), Offset(Offset), Size(Size), Text(Text) { }
  std::string Name;
  ulong Offset;
  ulong Size;
  std::string Text;
};

/* A raw per-square array, so a byte offset can be turned into a coordinate. */

struct mapblock
{
  mapblock(const std::string& Name, ulong Offset, ulong Size, int ElementSize, int XSize, int YSize)
  : Name(Name), Offset(Offset), Size(Size), ElementSize(ElementSize), XSize(XSize), YSize(YSize) { }
  std::string Name;
  ulong Offset;
  ulong Size;
  int ElementSize;
  int XSize, YSize;
};

struct decoded
{
  decoded() : Kind(KIND_OTHER), Ok(false), DecodedEnd(0), Anchor(0),
              AnchorFound(false), AnchorUnique(false), AnchorCandidates(0),
              XSize(0), YSize(0), Teams(0), Dungeons(0) { }
  int Kind;
  truth Ok;
  std::string Note;
  ulong DecodedEnd;
  ulong Anchor;
  truth AnchorFound;
  truth AnchorUnique;
  int AnchorCandidates;
  int XSize, YSize;
  int Teams, Dungeons;
  std::vector<field> Fields;
  std::vector<mapblock> Blocks;
};

struct reader
{
  reader(const std::vector<uchar>& Data, decoded& Out) : Data(Data), Out(Out), Pos(0), Bad(false) { }

  truth Need(ulong Size)
  {
    if(Bad || !Fits(Data, Pos, Size))
    {
      if(!Bad)
        Out.Note = Format("file ends inside the decodable prefix (at offset %lu)", Pos);

      Bad = true;
      return false;
    }

    return true;
  }

  int Read(const std::string& Name, int Type)
  {
    cint Size = TypeSize(Type);

    if(!Need(Size))
      return 0;

    Out.Fields.push_back(field(Name, Pos, Size, RenderType(Data, Pos, Type)));
    Pos += Size;
    return Size;
  }

  int32_t I32(const std::string& Name)
  {
    culong At = Pos;
    return Read(Name, T_I32) ? GetI32(Data, At) : 0;
  }

  int U8(const std::string& Name)
  {
    culong At = Pos;
    return Read(Name, T_U8) ? Data[At] : 0;
  }

  uint64_t ULong(const std::string& Name)
  {
    culong At = Pos;
    return Read(Name, T_ULONG) ? GetULong(Data, At) : 0;
  }

  void V2(const std::string& Name) { Read(Name, T_V2); }

  const std::vector<uchar>& Data;
  decoded& Out;
  ulong Pos;
  truth Bad;
};

/* game::Save, Main/Source/game.cpp:3454 onward. Written as one uninterrupted
 * run of scalars, which makes it findable by pattern once the variable-length
 * gamescript in front of it has been walked. */

struct anchorentry
{
  cchar* Name;
  int Type;
};

static const anchorentry AnchorLayout[] =
{
  { "CurrentDungeonIndex", T_I32 },
  { "CurrentLevelIndex", T_I32 },
  { "Camera", T_V2 },
  { "WizardMode", T_U8 },
  { "SeeWholeMapCheatMode", T_I32 },
  { "GoThroughWallsCheat", T_U8 },
  { "Tick", T_ULONG },
  { "Turn", T_LONG },
  { "InWilderness", T_U8 },
  { "NextCharacterID", T_ULONG },
  { "NextItemID", T_ULONG },
  { "NextTrapID", T_ULONG },
  { "NecroCounter", T_I32 },
  { "SumoWrestling", T_U8 },
  { "PlayerSumoChampion", T_U8 },
  { "TouristHasSpider", T_U8 },
  { "GlobalRainTimeModifier", T_LONG },
  { "Seed", T_LONG },
  { "AveragePlayerArmStrengthExperience", T_F64 },
  { "AveragePlayerLegStrengthExperience", T_F64 },
  { "AveragePlayerDexterityExperience", T_F64 },
  { "AveragePlayerAgilityExperience", T_F64 },
  { "Teams", T_I32 },
  { "Dungeons", T_I32 },
  { "StoryState", T_I32 },
  { "GloomyCaveStoryState", T_I32 },
  { "XinrochTombStoryState", T_I32 },
  { "FreedomStoryState", T_I32 },
  { "AslonaStoryState", T_I32 },
  { "RebelStoryState", T_I32 },
  { "PlayerIsChampion", T_U8 },
  { "HasBoat", T_U8 },
  { "PlayerRunning", T_U8 }
};

#define ANCHOR_FIELDS (int(sizeof(AnchorLayout) / sizeof(AnchorLayout[0])))

static void AnchorOffsets(std::vector<ulong>& Offsets, ulong& Span)
{
  Offsets.clear();
  Span = 0;

  for(int c = 0; c < ANCHOR_FIELDS; ++c)
  {
    Offsets.push_back(Span);
    Span += TypeSize(AnchorLayout[c].Type);
  }
}

static int AnchorIndex(cchar* Name)
{
  for(int c = 0; c < ANCHOR_FIELDS; ++c)
    if(!strcmp(AnchorLayout[c].Name, Name))
      return c;

  return -1;
}

struct anchorprobe
{
  std::vector<ulong> Offset;
  ulong Span;
  ulong DungeonIndexAt, LevelIndexAt, TeamsAt, DungeonsAt;
  int Teams, Dungeons;
};

static truth AnchorMatches(const std::vector<uchar>& Data, ulong At, const anchorprobe& Probe)
{
  cint DungeonIndex = GetI32(Data, At + Probe.DungeonIndexAt);

  /* A save taken on the world map is the common case for a harness run, and it
   * never holds 1..KNOWN_DUNGEONS: game::CurrentDungeonIndex is a zeroed static
   * that game::TryTravel alone assigns, so it is 0 until the player first
   * enters a dungeon and WORLD_MAP once they have come back out. Rejecting
   * those left every scalar field of such a save unlabelled. The Teams,
   * Dungeons and U8 checks below carry the false positive load. */

  if(DungeonIndex != 0 && DungeonIndex != WORLD_MAP_INDEX
     && (DungeonIndex < 1 || DungeonIndex > KNOWN_DUNGEONS))
    return false;

  cint LevelIndex = GetI32(Data, At + Probe.LevelIndexAt);

  if(LevelIndex < 0 || LevelIndex > MAX_KNOWN_LEVEL)
    return false;

  if(GetI32(Data, At + Probe.TeamsAt) != Probe.Teams)
    return false;

  if(GetI32(Data, At + Probe.DungeonsAt) != Probe.Dungeons)
    return false;

  for(int c = 0; c < ANCHOR_FIELDS; ++c)
    if(AnchorLayout[c].Type == T_U8 && Data[At + Probe.Offset[c]] > 1)
      return false;

  return true;
}

/* Main/Source/game.cpp:3452. Fixed prefix, then the gamescript, which is
 * walkable only as far as the dungeonscript map count. */

static void DecodeSav(const std::vector<uchar>& Data, decoded& Out)
{
  Out.Kind = KIND_SAV;
  reader R(Data, Out);
  cint Version = R.I32("SaveFileVersion");

  if(R.Bad)
    return;

  if(Version < 1 || Version > 10000)
  {
    Out.Note = Format("implausible SaveFileVersion %d - not an IVAN .sav, or wrong word size", Version);
    return;
  }

  cint TeamsPresent = R.U8("GameScript.TeamsPresent");
  int Teams = 0;

  if(TeamsPresent)
    Teams = R.I32("GameScript.Teams");

  const uint64_t TeamCount = R.ULong("GameScript.Team.Count");

  if(R.Bad)
    return;

  if(TeamCount > MAX_TEAM_COUNT)
  {
    Out.Note = Format("implausible GameScript team count %llu - decoding abandoned",
                      (unsigned long long)TeamCount);
    return;
  }

  for(ulong c = 0; c < ulong(TeamCount); ++c)
  {
    R.I32(Format("GameScript.Team[%lu].Index", c));

    if(R.U8(Format("GameScript.Team[%lu].KillEvilnessPresent", c)))
      R.I32(Format("GameScript.Team[%lu].KillEvilness", c));

    const uint64_t RelationCount = R.ULong(Format("GameScript.Team[%lu].Relation.Count", c));

    if(R.Bad)
      return;

    if(RelationCount > MAX_RELATION_COUNT)
    {
      Out.Note = Format("implausible relation count %llu in team %lu - decoding abandoned",
                        (unsigned long long)RelationCount, c);
      return;
    }

    for(ulong r = 0; r < ulong(RelationCount); ++r)
    {
      R.I32(Format("GameScript.Team[%lu].Relation[%lu].Team", c, r));
      R.I32(Format("GameScript.Team[%lu].Relation[%lu].Value", c, r));
    }

    if(R.Bad)
      return;
  }

  const uint64_t DungeonCount = R.ULong("GameScript.Dungeon.Count");

  if(R.Bad)
    return;

  if(DungeonCount > MAX_TEAM_COUNT)
  {
    Out.Note = Format("implausible GameScript dungeon count %llu - decoding abandoned",
                      (unsigned long long)DungeonCount);
    return;
  }

  Out.Ok = true;
  Out.Teams = Teams;
  Out.Dungeons = int(DungeonCount) + 1;
  Out.DecodedEnd = R.Pos;
  Out.Note = "structural decoding stops here: the next field is a dungeonscript "
             "(polymorphic, see Main/Source/script.cpp)";

  /* Everything past the gamescript sits at an offset nobody can compute, so
   * find the scalar run by pattern and require the match to be unique. Two
   * independently-derived integers cross-check every candidate. */

  anchorprobe Probe;
  AnchorOffsets(Probe.Offset, Probe.Span);
  Probe.DungeonIndexAt = Probe.Offset[AnchorIndex("CurrentDungeonIndex")];
  Probe.LevelIndexAt = Probe.Offset[AnchorIndex("CurrentLevelIndex")];
  Probe.TeamsAt = Probe.Offset[AnchorIndex("Teams")];
  Probe.DungeonsAt = Probe.Offset[AnchorIndex("Dungeons")];
  Probe.Teams = Teams;
  Probe.Dungeons = Out.Dungeons;

  if(Data.size() < Probe.Span)
    return;

  culong Last = Data.size() - Probe.Span;
  ulong FirstHit = 0;

  for(ulong At = R.Pos; At <= Last; ++At)
    if(AnchorMatches(Data, At, Probe))
    {
      if(!Out.AnchorCandidates)
        FirstHit = At;

      ++Out.AnchorCandidates;

      if(Out.AnchorCandidates > 1)
        break;
    }

  if(Out.AnchorCandidates != 1)
    return;

  Out.AnchorFound = true;
  Out.AnchorUnique = true;
  Out.Anchor = FirstHit;

  for(int c = 0; c < ANCHOR_FIELDS; ++c)
  {
    culong At = FirstHit + Probe.Offset[c];
    Out.Fields.push_back(field(AnchorLayout[c].Name, At, TypeSize(AnchorLayout[c].Type),
                               RenderType(Data, At, AnchorLayout[c].Type)));
  }

  Out.DecodedEnd = FirstHit + Probe.Span;
  Out.Note = "structural decoding stops after the scalar run: the next field is "
             "PlayerMassacreMap (Main/Source/game.cpp:3469)";
}

/* Main/Source/area.cpp:40. Both .wm and level files start here - there is no
 * version header and no magic number, so the dimensions are all we can check. */

static void DecodeArea(const std::vector<uchar>& Data, int Kind, decoded& Out)
{
  Out.Kind = Kind;
  reader R(Data, Out);
  cint XSize = R.I32("XSize");
  cint YSize = R.I32("YSize");

  if(R.Bad)
    return;

  if(XSize < 1 || YSize < 1 || XSize > MAX_AREA_SIDE || YSize > MAX_AREA_SIDE
     || double(XSize) * YSize > MAX_AREA_SQUARES)
  {
    Out.Note = Format("implausible area dimensions %dx%d - refusing to decode", XSize, YSize);
    return;
  }

  culong Squares = ulong(XSize) * ulong(YSize);
  const uint64_t EntryCount = R.ULong("EntryMap.Count");

  if(R.Bad)
    return;

  if(EntryCount > MAX_ENTRY_COUNT)
  {
    Out.Note = Format("implausible EntryMap count %llu - refusing to decode",
                      (unsigned long long)EntryCount);
    return;
  }

  for(ulong c = 0; c < ulong(EntryCount); ++c)
  {
    R.I32(Format("EntryMap[%lu].Key", c));
    R.V2(Format("EntryMap[%lu].Pos", c));
  }

  if(R.Bad)
    return;

  /* Raw blocks, all Alloc2D order: flat index c is square (c / YSize, c % YSize). */

  ulong Need = Squares;

  if(Kind == KIND_WM)
    Need = Squares * 6;

  if(!Fits(Data, R.Pos, Need))
  {
    Out.Note = Format("%dx%d area needs %lu bytes of map data at offset %lu but only %lu remain "
                      "- dimensions rejected, falling back to raw byte offsets",
                      XSize, YSize, Need, R.Pos, ulong(Data.size()) - R.Pos);
    return;
  }

  Out.Ok = true;
  Out.XSize = XSize;
  Out.YSize = YSize;
  Out.Blocks.push_back(mapblock("FlagMap", R.Pos, Squares, 1, XSize, YSize));
  R.Pos += Squares;

  if(Kind == KIND_WM)
  {
    Out.Blocks.push_back(mapblock("TypeBuffer", R.Pos, Squares, 1, XSize, YSize));
    R.Pos += Squares;
    Out.Blocks.push_back(mapblock("AltitudeBuffer", R.Pos, Squares * 2, 2, XSize, YSize));
    R.Pos += Squares * 2;
    Out.Blocks.push_back(mapblock("ContinentBuffer", R.Pos, Squares, 1, XSize, YSize));
    R.Pos += Squares;
    Out.Blocks.push_back(mapblock("PossibleLocationBuffer", R.Pos, Squares, 1, XSize, YSize));
    R.Pos += Squares;
    Out.Note = "structural decoding stops here: the next field is the per-square "
               "wsquare::Save loop (Main/Source/worldmap.cpp:122)";
  }
  else
    Out.Note = "structural decoding stops here: the next field is Room, a vector of "
               "polymorphic room* (Main/Source/level.cpp:819)";

  Out.DecodedEnd = R.Pos;
}

static void Decode(const std::vector<uchar>& Data, int Kind, decoded& Out)
{
  if(Kind == KIND_SAV)
    DecodeSav(Data, Out);
  else if(Kind == KIND_WM || Kind == KIND_LEVEL)
    DecodeArea(Data, Kind, Out);
  else
    Out.Note = "unrecognised role - no decoder";
}

static const field* FindField(const decoded& Dec, ulong Offset)
{
  for(std::vector<field>::const_iterator i = Dec.Fields.begin(); i != Dec.Fields.end(); ++i)
    if(Offset >= i->Offset && Offset < i->Offset + i->Size)
      return &*i;

  return 0;
}

static const field* FindFieldByName(const decoded& Dec, const std::string& Name)
{
  for(std::vector<field>::const_iterator i = Dec.Fields.begin(); i != Dec.Fields.end(); ++i)
    if(i->Name == Name)
      return &*i;

  return 0;
}

static const mapblock* FindBlock(const decoded& Dec, ulong Offset)
{
  for(std::vector<mapblock>::const_iterator i = Dec.Blocks.begin(); i != Dec.Blocks.end(); ++i)
    if(Offset >= i->Offset && Offset < i->Offset + i->Size)
      return &*i;

  return 0;
}

/* FNV-1a 64. Hand-rolled on purpose: the harness uses the same constants, so
 * hashes are comparable across both tools without pulling in a library. */

#define FNV_OFFSET_BASIS 0xCBF29CE484222325ULL
#define FNV_PRIME 0x100000001B3ULL

static uint64_t Fnv1a64(const uchar* Data, ulong Size)
{
  uint64_t Hash = FNV_OFFSET_BASIS;

  for(ulong c = 0; c < Size; ++c)
  {
    Hash ^= Data[c];
    Hash *= FNV_PRIME;
  }

  return Hash;
}

struct dungeoninfo
{
  cchar* Name;
  int Levels;
};

/* Script/define.dat:1186, and the Levels field of each script under
 * Script/dungeons. */

static const dungeoninfo DungeonTable[] =
{
  { 0, 0 },
  { "ELPURI_CAVE", 13 },
  { "ATTNAM", 5 },
  { "NEW_ATTNAM", 2 },
  { "UNDER_WATER_TUNNEL", 5 },
  { "EMPTY_AREA", 1 },
  { "XINROCH_TOMB", 11 },
  { "BLACK_MARKET", 2 },
  { "ASLONA_CASTLE", 4 },
  { "REBEL_CAMP", 1 },
  { "GOBLIN_FORT", 6 },
  { "FUNGAL_CAVE", 5 },
  { "PYRAMID", 5 },
  { "MONDEDR", 1 },
  { "IRINOX", 1 },
  { "DARK_FOREST", 1 }
};

static truth AllDigits(const std::string& What)
{
  if(What.empty())
    return false;

  for(ulong c = 0; c < What.size(); ++c)
    if(What[c] < '0' || What[c] > '9')
      return false;

  return true;
}

/* dungeon::SaveLevel concatenates dungeon index and level index with no
 * separator (Main/Source/dungeon.cpp:221), so ".110" is genuinely ambiguous.
 * Never parse it back to drive logic - list every reading and let a human
 * decide. */

static std::string DescribeLevelRole(const std::string& Digits)
{
  std::string Result;

  for(int D = 1; D <= KNOWN_DUNGEONS; ++D)
  {
    const std::string Prefix = Format("%d", D);

    if(Digits.size() <= Prefix.size() || Digits.compare(0, Prefix.size(), Prefix))
      continue;

    const std::string Rest = Digits.substr(Prefix.size());

    if(!AllDigits(Rest) || Format("%d", atoi(Rest.c_str())) != Rest)
      continue;

    if(atoi(Rest.c_str()) >= DungeonTable[D].Levels)
      continue;

    if(!Result.empty())
      Result += " | ";

    Result += Format("%s(%d) level %s", DungeonTable[D].Name, D, Rest.c_str());
  }

  return Result.empty() ? std::string("no valid dungeon/level reading") : Result;
}

struct entry
{
  entry() : Kind(KIND_OTHER) { }
  std::string Path;
  std::string Role;
  int Kind;
};

static truth ReadWholeFile(const std::string& Path, std::vector<uchar>& Out)
{
  FILE* File = fopen(Path.c_str(), "rb");

  if(!File)
    return false;

  Out.clear();
  char Buffer[65536];

  for(;;)
  {
    const size_t Got = fread(Buffer, 1, sizeof(Buffer), File);

    if(!Got)
      break;

    Out.insert(Out.end(), Buffer, Buffer + Got);
  }

  ctruth Failed = ferror(File) != 0;
  fclose(File);
  return !Failed;
}

static truth ListDirectory(const std::string& Path, std::vector<std::string>& Out)
{
#ifdef WIN32
  const std::string Pattern = Path + "/*";
  WIN32_FIND_DATA FindData;
  HANDLE Find = ::FindFirstFile(Pattern.c_str(), &FindData);

  if(Find == INVALID_HANDLE_VALUE)
    return false;

  do
  {
    if(!(FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && FindData.cFileName[0] != '.')
      Out.push_back(FindData.cFileName);
  }
  while(::FindNextFile(Find, &FindData));

  ::FindClose(Find);
  return true;
#else
  DIR* Dir = opendir(Path.c_str());

  if(!Dir)
    return false;

  for(dirent* Entry = readdir(Dir); Entry; Entry = readdir(Dir))
  {
    if(Entry->d_name[0] == '.')
      continue;

    struct stat Status;

    if(!stat((Path + "/" + Entry->d_name).c_str(), &Status) && S_ISDIR(Status.st_mode))
      continue;

    Out.push_back(Entry->d_name);
  }

  closedir(Dir);
  return true;
#endif
}

static truth Exists(const std::string& Path)
{
#ifdef WIN32
  return ::GetFileAttributes(Path.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
  struct stat Status;
  return !stat(Path.c_str(), &Status);
#endif
}

static truth IsDirectory(const std::string& Path)
{
#ifdef WIN32
  cuint Attributes = ::GetFileAttributes(Path.c_str());
  return Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat Status;
  return !stat(Path.c_str(), &Status) && S_ISDIR(Status.st_mode);
#endif
}

static truth EndsWith(const std::string& What, cchar* Suffix)
{
  const std::string S = Suffix;
  return What.size() >= S.size() && !What.compare(What.size() - S.size(), S.size(), S);
}

static int ClassifyRole(const std::string& Role)
{
  std::string Tail = Role;
  culong Dot = Tail.rfind('.');

  /* ".AutoSave.sav" and ".sav" behave identically; only the last token matters. */

  if(Dot != std::string::npos)
    Tail = Tail.substr(Dot + 1);

  if(Tail == "sav")
    return KIND_SAV;

  if(Tail == "wm")
    return KIND_WM;

  if(AllDigits(Tail))
    return KIND_LEVEL;

  return KIND_OTHER;
}

static cchar* KindName(int Kind)
{
  switch(Kind)
  {
   case KIND_SAV: return "sav";
   case KIND_WM: return "worldmap";
   case KIND_LEVEL: return "level";
  }

  return "other";
}

static cchar* VerdictName(int Verdict)
{
  switch(Verdict)
  {
   case V_SAME: return "SAME";
   case V_DIFF: return "DIFF";
   case V_SUSPECT: return "SUSPECT";
   case V_ONLY_A: return "ONLY_A";
   case V_ONLY_B: return "ONLY_B";
  }

  return "?";
}

/* Split at the FIRST dot: the stem is the run name, everything after it is the
 * role. game::SaveName stamps the stem with a timestamp (game.cpp:3671), so two
 * identical runs started a second apart have different stems - pairing must
 * ignore it entirely. */

static void SplitName(const std::string& Name, std::string& Stem, std::string& Role)
{
  culong Dot = Name.find('.');

  if(Dot == std::string::npos)
  {
    Stem = Name;
    Role.clear();
  }
  else
  {
    Stem = Name.substr(0, Dot);
    Role = Name.substr(Dot + 1);
  }
}

static truth CollectEntries(const std::string& Dir, const std::string& WantStem,
                            std::map<std::string, entry>& Out, std::string& Stem,
                            std::vector<std::string>& Skipped, std::string& Error)
{
  std::vector<std::string> Names;

  if(!ListDirectory(Dir, Names))
  {
    Error = Format("cannot read directory %s", Dir.c_str());
    return false;
  }

  std::set<std::string> Candidates;
  std::map<std::string, std::map<std::string, entry> > ByStem;

  for(ulong c = 0; c < Names.size(); ++c)
  {
    std::string ThisStem, Role;
    SplitName(Names[c], ThisStem, Role);

    if(Role.empty())
      continue;

    ctruth Backup = EndsWith(Role, ".bkp") || EndsWith(Role, ".tmp");

    if(Backup && !IncludeBackups)
    {
      Skipped.push_back(Names[c]);
      continue;
    }

    entry Entry;
    Entry.Path = Dir + "/" + Names[c];
    Entry.Role = Role;
    Entry.Kind = Backup ? KIND_OTHER : ClassifyRole(Role);

    if(Entry.Kind == KIND_OTHER && !Backup)
      continue;

    ByStem[ThisStem][Role] = Entry;

    if(!Backup)
      Candidates.insert(ThisStem);
  }

  if(!WantStem.empty())
  {
    if(!ByStem.count(WantStem))
    {
      Error = Format("no save files with stem \"%s\" in %s", WantStem.c_str(), Dir.c_str());
      return false;
    }

    Stem = WantStem;
  }
  else if(Candidates.size() == 1)
    Stem = *Candidates.begin();
  else if(Candidates.empty())
  {
    Error = Format("no IVAN save files found in %s", Dir.c_str());
    return false;
  }
  else
  {
    Error = Format("%lu save games in %s - pass --stem-a/--stem-b to pick one:",
                   ulong(Candidates.size()), Dir.c_str());

    for(std::set<std::string>::const_iterator i = Candidates.begin(); i != Candidates.end(); ++i)
      Error += "\n  " + *i;

    return false;
  }

  Out = ByStem[Stem];
  return true;
}

static void PrintHexWindow(const std::vector<uchar>& A, const std::vector<uchar>& B, ulong At)
{
  culong Longest = A.size() > B.size() ? A.size() : B.size();
  ulong From = At > ulong(Window) ? At - Window : 0;
  From &= ~ulong(7);
  ulong To = At + Window;

  if(To > Longest)
    To = Longest;

  printf("  %-8s  %-24s   %s\n", "offset", "A", "B");

  for(ulong Row = From; Row < To; Row += 8)
  {
    char Marks[64];
    memset(Marks, ' ', sizeof(Marks));
    truth AnyMark = false;
    printf("  %08lx  ", Row);

    for(int c = 0; c < 8; ++c)
    {
      culong O = Row + c;

      if(O < A.size())
        printf("%02x ", A[O]);
      else
        printf("-- ");
    }

    printf("   ");

    for(int c = 0; c < 8; ++c)
    {
      culong O = Row + c;

      if(c)
        printf(" ");

      if(O < B.size())
        printf("%02x", B[O]);
      else
        printf("--");

      cuchar Left = O < A.size() ? A[O] : 0;
      cuchar Right = O < B.size() ? B[O] : 0;

      if(O < Longest && (O >= A.size() || O >= B.size() || Left != Right))
      {
        Marks[c * 3] = Marks[c * 3 + 1] = O == At ? '^' : '~';
        Marks[27 + c * 3] = Marks[27 + c * 3 + 1] = O == At ? '^' : '~';
        AnyMark = true;
      }
    }

    printf("\n");

    if(AnyMark)
    {
      int Length = 50;

      while(Length > 0 && Marks[Length - 1] == ' ')
        --Length;

      Marks[Length] = 0;
      printf("            %s\n", Marks);
    }
  }
}

struct result
{
  result() : Kind(KIND_OTHER), Verdict(V_SAME), HasA(false), HasB(false),
             Identified(false), Square(false), SizeA(0), SizeB(0), HashA(0), HashB(0),
             FirstDiff(0), LastDiff(0), DiffBytes(0), DiffBlocks(0), TotalBlocks(0) { }
  std::string Role;
  std::string PathA, PathB;
  int Kind;
  int Verdict;
  truth HasA, HasB;
  truth Identified;             /* a named field or map square, not just a note */
  truth Square;                 /* the name is a map coordinate, not a scalar */
  ulong SizeA, SizeB;
  uint64_t HashA, HashB;
  ulong FirstDiff, LastDiff, DiffBytes, DiffBlocks, TotalBlocks;
  std::string Label;
  std::string ValueA, ValueB;
  std::string Detail;
  std::vector<uchar> BlockDiff;
};

struct rolebefore
{
  rolebefore(const std::map<std::string, entry>& A, const std::map<std::string, entry>& B) : A(A), B(B) { }

  int KindOf(const std::string& Role) const
  {
    std::map<std::string, entry>::const_iterator i = A.find(Role);

    if(i != A.end())
      return i->second.Kind;

    i = B.find(Role);
    return i != B.end() ? i->second.Kind : KIND_OTHER;
  }

  bool operator()(const std::string& Left, const std::string& Right) const
  {
    cint L = KindOf(Left), R = KindOf(Right);
    return L != R ? L < R : Left < Right;
  }

  const std::map<std::string, entry>& A;
  const std::map<std::string, entry>& B;
};

static std::string JsonEscape(const std::string& What)
{
  std::string Out;

  for(ulong c = 0; c < What.size(); ++c)
  {
    cchar C = What[c];

    if(C == '"' || C == '\\')
    {
      Out += '\\';
      Out += C;
    }
    else if(C == '\n')
      Out += "\\n";
    else if(uchar(C) < 0x20)
      Out += Format("\\u%04x", uchar(C));
    else
      Out += C;
  }

  return Out;
}

/* game.cpp:3495 writes GetTimeSpent(), wall-clock seconds since load. Two
 * byte-identical runs differ here unless they save within the same second. It
 * lies past every anchor, so it can only be recognised by shape - which makes
 * this a heuristic, and it is reported as one. */

static truth LooksLikeTimeSpent(const std::vector<uchar>& A, const std::vector<uchar>& B,
                                const decoded& Dec, const result& Res, ulong& Where)
{
  if(Res.Kind != KIND_SAV || Res.SizeA != Res.SizeB)
    return false;

  if(Res.LastDiff - Res.FirstDiff + 1 > 8)
    return false;

  /* The anchor is deliberately not required here. Whether one was found
   * decides how much the shape is worth believing, not whether the shape is
   * there, so the caller applies that: it downgrades the verdict only with an
   * anchor and otherwise reports the resemblance without acting on it. */

  if(!Dec.Ok || Res.FirstDiff < Dec.DecodedEnd)
    return false;

  culong Lowest = Res.LastDiff >= 7 ? Res.LastDiff - 7 : 0;
  int64_t Best = TIME_SPENT_LIMIT;
  truth Found = false;

  /* Several 8-byte windows can span the differing bytes. The real field is the
   * one whose high bytes are zero, so prefer the smallest plausible reading. */

  for(ulong At = Lowest; At <= Res.FirstDiff; ++At)
  {
    if(!Fits(A, At, 8))
      break;

    const int64_t Left = GetI64(A, At);
    const int64_t Right = GetI64(B, At);

    if(Left < 0 || Right < 0 || Left >= TIME_SPENT_LIMIT || Right >= TIME_SPENT_LIMIT)
      continue;

    const int64_t Largest = Left > Right ? Left : Right;

    if(!Found || Largest < Best)
    {
      Best = Largest;
      Where = At;
      Found = true;
    }
  }

  return Found;
}

static void Compare(const std::vector<uchar>& A, const std::vector<uchar>& B, result& Res)
{
  culong Shortest = A.size() < B.size() ? A.size() : B.size();
  culong Longest = A.size() > B.size() ? A.size() : B.size();
  Res.TotalBlocks = (Longest + BLOCK_SIZE - 1) / BLOCK_SIZE;
  Res.BlockDiff.assign(Res.TotalBlocks, 0);
  truth Found = false;

  for(ulong c = 0; c < Longest; ++c)
  {
    if(c < Shortest && A[c] == B[c])
      continue;

    if(!Found)
    {
      Res.FirstDiff = c;
      Found = true;
    }

    Res.LastDiff = c;
    ++Res.DiffBytes;
    Res.BlockDiff[c / BLOCK_SIZE] = 1;
  }

  for(ulong c = 0; c < Res.TotalBlocks; ++c)
    if(Res.BlockDiff[c])
      ++Res.DiffBlocks;

  Res.Verdict = Found ? V_DIFF : V_SAME;
}

static void Describe(const std::vector<uchar>& A, const std::vector<uchar>& B,
                     const decoded& DecA, const decoded& DecB, result& Res)
{
  const field* FieldA = FindField(DecA, Res.FirstDiff);

  if(FieldA)
  {
    const field* FieldB = FindFieldByName(DecB, FieldA->Name);
    Res.Identified = true;
    Res.Label = FieldA->Name;
    Res.ValueA = FieldA->Text;
    Res.ValueB = FieldB ? FieldB->Text : std::string("<not decoded>");
    return;
  }

  const mapblock* Block = FindBlock(DecA, Res.FirstDiff);

  if(Block)
  {
    culong Index = (Res.FirstDiff - Block->Offset) / Block->ElementSize;
    culong At = Block->Offset + Index * Block->ElementSize;
    Res.Identified = true;
    Res.Square = true;
    Res.Label = Format("%s at square (%lu,%lu)", Block->Name.c_str(),
                       Index / ulong(Block->YSize), Index % ulong(Block->YSize));

    /* B can be shorter than A - the blocks were sized against A alone. */

    if(!Fits(A, At, Block->ElementSize) || !Fits(B, At, Block->ElementSize))
      return;

    if(Block->ElementSize == 1)
    {
      Res.ValueA = Format("%u", A[At]);
      Res.ValueB = Format("%u", B[At]);
    }
    else if(Block->ElementSize == 2)
    {
      int16_t Left, Right;
      memcpy(&Left, &A[At], 2);
      memcpy(&Right, &B[At], 2);
      Res.ValueA = Format("%d", Left);
      Res.ValueB = Format("%d", Right);
    }

    return;
  }

  if(DecA.Ok && Res.FirstDiff >= DecA.DecodedEnd)
    Res.Label = Format("past the decodable prefix (ends at offset %lu)", DecA.DecodedEnd);
}

static void PrintDetail(const std::string& Role, const std::vector<uchar>& A,
                        const std::vector<uchar>& B, const decoded& DecA,
                        const decoded& DecB, const result& Res)
{
  printf("\n--- %s (%s): %s ---\n", Role.c_str(), KindName(Res.Kind), VerdictName(Res.Verdict));

  if(Res.Kind == KIND_LEVEL)
  {
    std::string Digits = Role;
    culong Dot = Digits.rfind('.');

    if(Dot != std::string::npos)
      Digits = Digits.substr(Dot + 1);

    printf("  ambiguous name, possible readings: %s\n", DescribeLevelRole(Digits).c_str());
  }

  if(Res.Kind == KIND_SAV && DecA.Ok)
  {
    const field* Version = FindFieldByName(DecA, "SaveFileVersion");

    if(Version && atoi(Version->Text.c_str()) != KNOWN_SAVE_FILE_VERSION)
      printf("  note: SaveFileVersion is %s, this tool's field tables were derived from %d\n",
             Version->Text.c_str(), KNOWN_SAVE_FILE_VERSION);

    if(DecA.AnchorUnique)
      printf("  scalar-run anchor at offset %lu (unique match, cross-checked on Teams=%d Dungeons=%d)\n",
             DecA.Anchor, DecA.Teams, DecA.Dungeons);
    else if(DecA.AnchorCandidates)
      printf("  scalar-run anchor NOT unique (%d+ candidates) - field labels past the "
             "gamescript are unavailable\n", DecA.AnchorCandidates);
    else
      printf("  scalar-run anchor not found - field labels past the gamescript are unavailable\n");
  }

  if((Res.Kind == KIND_WM || Res.Kind == KIND_LEVEL) && DecA.Ok)
  {
    if(DecB.Ok && (DecB.XSize != DecA.XSize || DecB.YSize != DecA.YSize))
      printf("  area size differs: A is %dx%d squares, B is %dx%d\n",
             DecA.XSize, DecA.YSize, DecB.XSize, DecB.YSize);
    else
      printf("  area is %dx%d squares\n", DecA.XSize, DecA.YSize);
  }

  if(!DecA.Ok && !DecA.Note.empty())
    printf("  A not decodable: %s\n", DecA.Note.c_str());

  if(!DecB.Ok && !DecB.Note.empty() && DecB.Note != DecA.Note)
    printf("  B not decodable: %s\n", DecB.Note.c_str());

  culong Largest = Res.SizeA > Res.SizeB ? Res.SizeA : Res.SizeB;
  printf("  size: A=%lu B=%lu\n", Res.SizeA, Res.SizeB);
  printf("  first difference at offset %lu (0x%lx)\n", Res.FirstDiff, Res.FirstDiff);
  printf("  differing bytes: %lu of %lu (%.4f%%)\n", Res.DiffBytes, Largest,
         Largest ? Res.DiffBytes * 100.0 / Largest : 0.0);
  printf("  differing %d KiB blocks: %lu of %lu\n", BLOCK_SIZE / 1024, Res.DiffBlocks, Res.TotalBlocks);

  if(!Res.Label.empty())
  {
    if(!Res.ValueA.empty() || !Res.ValueB.empty())
      printf("  %s%s: A = %s  B = %s\n", Res.Square ? "" : "field ", Res.Label.c_str(),
             Res.ValueA.c_str(), Res.ValueB.c_str());
    else if(Res.Identified)
      printf("  %s\n", Res.Label.c_str());
    else
      printf("  location: %s\n", Res.Label.c_str());
  }

  if(!Res.Detail.empty())
    printf("  %s\n", Res.Detail.c_str());

  PrintHexWindow(A, B, Res.FirstDiff);
}

static void PrintJson(const result& Res, const std::string& PathA, const std::string& PathB,
                      const decoded& DecA, const decoded& DecB)
{
  printf("{\"type\":\"role\",\"role\":\"%s\",\"kind\":\"%s\",\"verdict\":\"%s\"",
         JsonEscape(Res.Role).c_str(), KindName(Res.Kind), VerdictName(Res.Verdict));
  printf(",\"path_a\":\"%s\",\"path_b\":\"%s\"",
         JsonEscape(PathA).c_str(), JsonEscape(PathB).c_str());

  if(Res.HasA)
    printf(",\"size_a\":%lu,\"hash_a\":\"0x%016llx\"", Res.SizeA, (unsigned long long)Res.HashA);
  else
    printf(",\"size_a\":null,\"hash_a\":null");

  if(Res.HasB)
    printf(",\"size_b\":%lu,\"hash_b\":\"0x%016llx\"", Res.SizeB, (unsigned long long)Res.HashB);
  else
    printf(",\"size_b\":null,\"hash_b\":null");

  if(Res.Verdict == V_DIFF || Res.Verdict == V_SUSPECT)
  {
    printf(",\"first_diff\":%lu,\"diff_bytes\":%lu,\"diff_blocks\":%lu,\"total_blocks\":%lu",
           Res.FirstDiff, Res.DiffBytes, Res.DiffBlocks, Res.TotalBlocks);

    if(!Res.Label.empty())
      printf(",\"field\":\"%s\",\"value_a\":\"%s\",\"value_b\":\"%s\"",
             JsonEscape(Res.Label).c_str(), JsonEscape(Res.ValueA).c_str(), JsonEscape(Res.ValueB).c_str());

    if(!Res.Detail.empty())
      printf(",\"note\":\"%s\"", JsonEscape(Res.Detail).c_str());
  }

  /* Emitted whatever the verdict: a role that decoded into nothing is worth
   * knowing about even when the two sides agree byte for byte. */

  if(Res.HasA && !DecA.Ok && !DecA.Note.empty())
    printf(",\"decode_note_a\":\"%s\"", JsonEscape(DecA.Note).c_str());

  if(Res.HasB && !DecB.Ok && !DecB.Note.empty())
    printf(",\"decode_note_b\":\"%s\"", JsonEscape(DecB.Note).c_str());

  printf("}\n");
}

static void PrintKnownNondeterminism()
{
  printf(
    "Known sources of divergence that are NOT bugs in the game logic:\n"
    "\n"
    "  game::Save writes GetTimeSpent() (Main/Source/game.cpp:3495), wall-clock\n"
    "  seconds elapsed since the game was loaded. Two byte-identical runs differ\n"
    "  here unless both save inside the same second. It sits past every anchor,\n"
    "  so savediff can only recognise it by shape and reports it as SUSPECT.\n"
    "  Use --ignore-timespent to keep the exit status clean.\n"
    "\n"
    "  game::SaveName stamps the file stem with strftime(\"%%Y%%m%%d_%%H%%M%%S\")\n"
    "  when no base name is given (game.cpp:3671). Runs started a second apart\n"
    "  produce entirely different filenames - which is why savediff pairs files\n"
    "  by role suffix and never by name.\n"
    "\n"
    "  game::Save draws from the RNG and reseeds from that draw (game.cpp:3458).\n"
    "  Saving is not observation-neutral. Both runs must save at the same input\n"
    "  position, and a divergent Seed field means the RNG streams had ALREADY\n"
    "  diverged before the save, not that saving broke something.\n"
    "\n"
    "  game::LoadLevel reads a bone file gated on !(RAND() & 7) (game.cpp:5794).\n"
    "  If the two runs see different Bones directory contents they genuinely\n"
    "  diverge and no amount of seed pinning helps. Empty both Bones directories\n"
    "  before a differential run - savediff warns if either is non-empty.\n"
    "\n"
    "  outputfile keeps the previous save generation as <name>.bkp and writes\n"
    "  through <name>.tmp (FeLib/Source/save.cpp:70). .bkp depends on save\n"
    "  history rather than final state and is excluded by default. A leftover\n"
    "  .tmp means a save crashed halfway.\n"
    "\n"
    "  Saves are host-ABI dependent: SAVE_COMPATIBILITY is dead code, so long\n"
    "  and ulong are written at native width. A save from an ILP32 build (such\n"
    "  as the WASM port) needs --word-size 32 and will not be byte-comparable\n"
    "  with an LP64 save at all.\n");
}

static void PrintHelp()
{
  printf(
    "savediff " SAVEDIFF_VERSION " - IVAN save game differential\n"
    "\n"
    "Usage: savediff [options] <dir-a> <dir-b>\n"
    "       savediff [options] <file-a> <file-b>\n"
    "\n"
    "Compares two saved games and reports where they diverge. A saved game is\n"
    "several files - <stem>.sav, <stem>.wm and one <stem>.<dungeon><level> per\n"
    "generated level - so the directory form is the normal one. Files are paired\n"
    "by the role suffix after the first dot, never by name, because game::SaveName\n"
    "puts a timestamp in the stem.\n"
    "\n"
    "Options:\n"
    "  --summary               verdict and hash table only, plus a block bitmap\n"
    "  --json                  one JSON object per role on its own line (JSONL)\n"
    "  --window N              hexdump context around the first difference (64)\n"
    "  --stem-a NAME           save name in dir A, if it holds more than one\n"
    "  --stem-b NAME           save name in dir B, if it holds more than one\n"
    "  --include-backups       also compare .bkp and .tmp files\n"
    "  --ignore-timespent      SUSPECT verdicts do not affect the exit status\n"
    "  --word-size 32|64       width of long/ulong in the saves (default 64)\n"
    "  --known-nondeterminism  print the inventory of legitimate divergences\n"
    "  --version               print version and exit\n"
    "  --help                  this text\n"
    "\n"
    "Exit status: 0 all files match, 1 something differs, 2 error.\n"
    "\n"
    "--json ends with a summary object whose \"differ\" is the exit status as a\n"
    "boolean, so gating on either gives the same answer. It also carries\n"
    "\"exit\", \"suspect\" for whether any verdict was SUSPECT, and\n"
    "\"hard_differ\" for a difference that was not a SUSPECT. A file the decoder\n"
    "could not read is reported in \"decode_note_a\"/\"decode_note_b\" whatever\n"
    "the verdict, and on stderr when the detail section is not printed.\n"
    "\n"
    "The save format carries no field tags, so savediff labels only the leading\n"
    "fields of each file and reports byte offsets past that. It never guesses:\n"
    "if the .sav scalar run cannot be located uniquely, or an area header fails\n"
    "its sanity checks, it says so and falls back to raw offsets.\n"
    "\n"
    "savediff links nothing and can be built without cmake:\n"
    "  g++ -std=c++11 -O2 -I FeLib/Include -o savediff tools/savediff/Source/savediff.cpp\n");
}

/* game::GetBoneDir is GetUserDataDir() + "Bones/", a sibling of the save
 * directory, but people point savediff at all sorts of paths - so look for
 * Bones both beside and inside the directory given. */

static void WarnAboutBones(const std::string& Dir, cchar* Side)
{
  std::string Trimmed = Dir;

  while(Trimmed.size() > 1 && Trimmed[Trimmed.size() - 1] == '/')
    Trimmed.resize(Trimmed.size() - 1);

  culong Slash = Trimmed.find_last_of('/');
  const std::string Parent = Slash == std::string::npos ? std::string(".") : Trimmed.substr(0, Slash);
  std::vector<std::string> Names;

  if(!ListDirectory(Trimmed + "/Bones", Names) || Names.empty())
  {
    Names.clear();

    if(!ListDirectory(Parent + "/Bones", Names) || Names.empty())
      return;
  }

  fprintf(stderr, "warning: side %s has a non-empty Bones directory (%lu files). game::LoadLevel\n",
          Side, ulong(Names.size()));
  fprintf(stderr, "         reads bone files on !(RAND() & 7) (game.cpp:5794), so the two runs\n");
  fprintf(stderr, "         can diverge for reasons unrelated to the code under test. Empty both\n");
  fprintf(stderr, "         Bones directories before a differential run.\n");
}

int main(int argc, char** argv)
{
  std::vector<std::string> Positional;
  std::string StemA, StemB;

  for(int c = 1; c < argc; ++c)
  {
    const std::string Arg = argv[c];

    if(Arg == "--help" || Arg == "-h")
    {
      PrintHelp();
      return 0;
    }

    if(Arg == "--version")
    {
      printf("savediff " SAVEDIFF_VERSION "\n");
      return 0;
    }

    if(Arg == "--known-nondeterminism")
    {
      PrintKnownNondeterminism();
      return 0;
    }

    if(Arg == "--summary")
      SummaryMode = true;
    else if(Arg == "--json")
      JsonMode = true;
    else if(Arg == "--include-backups")
      IncludeBackups = true;
    else if(Arg == "--ignore-timespent")
      IgnoreTimeSpent = true;
    else if(Arg == "--window" && c + 1 < argc)
      Window = atoi(argv[++c]);
    else if(Arg == "--stem-a" && c + 1 < argc)
      StemA = argv[++c];
    else if(Arg == "--stem-b" && c + 1 < argc)
      StemB = argv[++c];
    else if(Arg == "--word-size" && c + 1 < argc)
    {
      WordSize = atoi(argv[++c]) / 8;

      if(WordSize != 4 && WordSize != 8)
      {
        fprintf(stderr, "savediff: --word-size must be 32 or 64\n");
        return 2;
      }
    }
    else if(Arg.size() > 1 && Arg[0] == '-')
    {
      fprintf(stderr, "savediff: unknown option %s (try --help)\n", Arg.c_str());
      return 2;
    }
    else
      Positional.push_back(Arg);
  }

  if(Positional.size() != 2)
  {
    fprintf(stderr, "savediff: expected two paths (try --help)\n");
    return 2;
  }

  if(Window < 0 || Window > 1 << 20)
  {
    fprintf(stderr, "savediff: --window out of range\n");
    return 2;
  }

  for(int c = 0; c < 2; ++c)
    if(!Exists(Positional[c]))
    {
      fprintf(stderr, "savediff: no such file or directory: %s\n", Positional[c].c_str());
      return 2;
    }

  if(IsDirectory(Positional[0]) != IsDirectory(Positional[1]))
  {
    fprintf(stderr, "savediff: compare two save directories or two files, not one of each\n");
    return 2;
  }

  std::map<std::string, entry> FilesA, FilesB;
  std::vector<std::string> SkippedA, SkippedB;
  std::string NameA = Positional[0], NameB = Positional[1];
  ctruth DirectoryMode = IsDirectory(Positional[0]);

  if(DirectoryMode)
  {
    std::string Error, FoundStemA, FoundStemB;

    if(!CollectEntries(Positional[0], StemA, FilesA, FoundStemA, SkippedA, Error)
       || !CollectEntries(Positional[1], StemB, FilesB, FoundStemB, SkippedB, Error))
    {
      fprintf(stderr, "savediff: %s\n", Error.c_str());
      return 2;
    }

    NameA = Positional[0] + "  (save name \"" + FoundStemA + "\")";
    NameB = Positional[1] + "  (save name \"" + FoundStemB + "\")";
    WarnAboutBones(Positional[0], "A");
    WarnAboutBones(Positional[1], "B");
  }
  else
  {
    /* Single-file mode. The role is taken from each name independently so that
     * files with unrelated names can still be compared. */

    entry EntryA, EntryB;
    std::string Stem;
    culong SlashA = Positional[0].find_last_of('/');
    culong SlashB = Positional[1].find_last_of('/');
    SplitName(SlashA == std::string::npos ? Positional[0] : Positional[0].substr(SlashA + 1),
              Stem, EntryA.Role);
    SplitName(SlashB == std::string::npos ? Positional[1] : Positional[1].substr(SlashB + 1),
              Stem, EntryB.Role);
    EntryA.Path = Positional[0];
    EntryB.Path = Positional[1];
    EntryA.Kind = EntryB.Kind = ClassifyRole(EntryA.Role);

    if(EntryA.Role.empty() || ClassifyRole(EntryB.Role) != EntryA.Kind)
      EntryA.Kind = EntryB.Kind = KIND_OTHER;

    FilesA[EntryA.Role] = EntryA;
    FilesB[EntryA.Role] = EntryB;
    NameA = Positional[0];
    NameB = Positional[1];
  }

  std::set<std::string> RoleSet;

  for(std::map<std::string, entry>::const_iterator i = FilesA.begin(); i != FilesA.end(); ++i)
    RoleSet.insert(i->first);

  for(std::map<std::string, entry>::const_iterator i = FilesB.begin(); i != FilesB.end(); ++i)
    RoleSet.insert(i->first);

  /* Report the main save first, then the world map, then the levels. */

  std::vector<std::string> Roles(RoleSet.begin(), RoleSet.end());
  std::stable_sort(Roles.begin(), Roles.end(), rolebefore(FilesA, FilesB));

  if(!JsonMode)
  {
    printf("savediff " SAVEDIFF_VERSION "\n");
    printf("A: %s\n", NameA.c_str());
    printf("B: %s\n", NameB.c_str());
    printf("word size: %d-bit (long and ulong are %d bytes)\n", WordSize * 8, WordSize);

    if(!SkippedA.empty() || !SkippedB.empty())
      printf("skipped %lu backup/temporary files (--include-backups to compare them)\n",
             ulong(SkippedA.size() + SkippedB.size()));

    printf("\n%-20s %-8s %12s %12s  %-18s %-18s\n",
           "ROLE", "VERDICT", "SIZE A", "SIZE B", "HASH A", "HASH B");
  }

  truth AnyDiff = false;
  truth AnySuspect = false;
  std::vector<result> Detailed;

  for(std::vector<std::string>::const_iterator i = Roles.begin(); i != Roles.end(); ++i)
  {
    result Res;
    Res.Role = *i;
    std::map<std::string, entry>::const_iterator A = FilesA.find(*i);
    std::map<std::string, entry>::const_iterator B = FilesB.find(*i);
    Res.HasA = A != FilesA.end();
    Res.HasB = B != FilesB.end();
    Res.Kind = Res.HasA ? A->second.Kind : B->second.Kind;
    std::vector<uchar> DataA, DataB;

    if(Res.HasA && !ReadWholeFile(A->second.Path, DataA))
    {
      fprintf(stderr, "savediff: cannot read %s\n", A->second.Path.c_str());
      return 2;
    }

    if(Res.HasB && !ReadWholeFile(B->second.Path, DataB))
    {
      fprintf(stderr, "savediff: cannot read %s\n", B->second.Path.c_str());
      return 2;
    }

    decoded DecA, DecB;

    if(Res.HasA)
    {
      Res.SizeA = DataA.size();
      Res.HashA = Fnv1a64(DataA.empty() ? 0 : &DataA[0], DataA.size());
      Decode(DataA, Res.Kind, DecA);
    }

    if(Res.HasB)
    {
      Res.SizeB = DataB.size();
      Res.HashB = Fnv1a64(DataB.empty() ? 0 : &DataB[0], DataB.size());
      Decode(DataB, Res.Kind, DecB);
    }

    if(!Res.HasA)
      Res.Verdict = V_ONLY_B;
    else if(!Res.HasB)
      Res.Verdict = V_ONLY_A;
    else
    {
      Compare(DataA, DataB, Res);

      if(Res.Verdict == V_DIFF)
      {
        Describe(DataA, DataB, DecA, DecB, Res);
        ulong Where;

        if(!Res.Identified && LooksLikeTimeSpent(DataA, DataB, DecA, Res, Where))
        {
          if(DecA.AnchorUnique)
          {
            Res.Verdict = V_SUSPECT;
            Res.Detail = Format("SUSPECT: %lu-byte divergence at offset %lu is consistent with "
                                "game::Save's GetTimeSpent() field (game.cpp:3495) - reading the "
                                "8 bytes at %lu gives %lld vs %lld, both plausible elapsed-second "
                                "counts. This may not be a real divergence.",
                                Res.LastDiff - Res.FirstDiff + 1, Res.FirstDiff, Where,
                                (long long)GetI64(DataA, Where), (long long)GetI64(DataB, Where));
          }
          else

            /* Reported, but the verdict is left alone. Without an anchor the
             * offset cannot be tied to a field, and a Seed divergence excused
             * as a clock reading is a false pass, which is the one answer this
             * tool must never give. */

            Res.Detail = Format("%lu-byte divergence at offset %lu has the shape of "
                                "game::Save's GetTimeSpent() field (game.cpp:3495) - the 8 bytes "
                                "at %lu read %lld vs %lld, both plausible elapsed-second counts. "
                                "The scalar-run anchor was not found in this file, so the offset "
                                "cannot be tied to that field and this stays a DIFF that "
                                "--ignore-timespent will not suppress.",
                                Res.LastDiff - Res.FirstDiff + 1, Res.FirstDiff, Where,
                                (long long)GetI64(DataA, Where), (long long)GetI64(DataB, Where));
        }
      }
    }

    if(Res.Verdict == V_SUSPECT)
      AnySuspect = true;
    else if(Res.Verdict != V_SAME)
      AnyDiff = true;

    if(JsonMode)
      PrintJson(Res, Res.HasA ? A->second.Path : std::string(),
                Res.HasB ? B->second.Path : std::string(), DecA, DecB);
    else
    {
      char SizeTextA[32], SizeTextB[32], HashTextA[32], HashTextB[32];

      if(Res.HasA)
      {
        snprintf(SizeTextA, sizeof(SizeTextA), "%lu", Res.SizeA);
        snprintf(HashTextA, sizeof(HashTextA), "0x%016llx", (unsigned long long)Res.HashA);
      }
      else
      {
        strcpy(SizeTextA, "-");
        strcpy(HashTextA, "-");
      }

      if(Res.HasB)
      {
        snprintf(SizeTextB, sizeof(SizeTextB), "%lu", Res.SizeB);
        snprintf(HashTextB, sizeof(HashTextB), "0x%016llx", (unsigned long long)Res.HashB);
      }
      else
      {
        strcpy(SizeTextB, "-");
        strcpy(HashTextB, "-");
      }

      printf("%-20s %-8s %12s %12s  %-18s %-18s\n", Res.Role.c_str(), VerdictName(Res.Verdict),
             SizeTextA, SizeTextB, HashTextA, HashTextB);

      if(SummaryMode && Res.Verdict != V_SAME && Res.TotalBlocks)
      {
        printf("  blocks: ");

        for(ulong b = 0; b < Res.TotalBlocks; ++b)
          putchar(Res.BlockDiff[b] ? '#' : '.');

        printf("  (%lu of %lu differ)\n", Res.DiffBlocks, Res.TotalBlocks);
      }
    }

    if(!JsonMode && !SummaryMode && Res.Verdict != V_SAME)
    {
      Res.PathA = Res.HasA ? A->second.Path : std::string();
      Res.PathB = Res.HasB ? B->second.Path : std::string();
      Detailed.push_back(Res);
    }
    else
    {
      /* PrintDetail is what normally reports a file the decoder could not read,
       * and it does not run for this role. Saying nothing would let two files
       * that are not saves at all pass as a match: both runs failing to write
       * one, or the whole comparison being run at the wrong --word-size, look
       * from the outside exactly like agreement. */

      if(Res.HasA && !DecA.Ok && !DecA.Note.empty())
        fprintf(stderr, "warning: %s in A is not decodable: %s\n",
                Res.Role.c_str(), DecA.Note.c_str());

      if(Res.HasB && !DecB.Ok && !DecB.Note.empty() && DecB.Note != DecA.Note)
        fprintf(stderr, "warning: %s in B is not decodable: %s\n",
                Res.Role.c_str(), DecB.Note.c_str());
    }
  }

  /* Details come after the whole table so the verdicts stay readable as a
   * block. The files are re-read rather than held: a save directory can be a
   * few hundred kilobytes per role and there is no reason to keep it all. */

  for(ulong c = 0; c < Detailed.size(); ++c)
  {
    const result& Res = Detailed[c];

    if(Res.Verdict == V_ONLY_A || Res.Verdict == V_ONLY_B)
    {
      printf("\n--- %s (%s): %s ---\n", Res.Role.c_str(), KindName(Res.Kind), VerdictName(Res.Verdict));
      printf("  this file exists only in %s. The two runs did not generate the same\n",
             Res.Verdict == V_ONLY_A ? "A" : "B");
      printf("  set of levels, which is itself a divergence.\n");

      if(Res.Kind == KIND_LEVEL)
      {
        std::string Digits = Res.Role;
        culong Dot = Digits.rfind('.');

        if(Dot != std::string::npos)
          Digits = Digits.substr(Dot + 1);

        printf("  possible readings: %s\n", DescribeLevelRole(Digits).c_str());
      }

      continue;
    }

    std::vector<uchar> DataA, DataB;
    decoded DecA, DecB;

    if(!ReadWholeFile(Res.PathA, DataA) || !ReadWholeFile(Res.PathB, DataB))
    {
      fprintf(stderr, "savediff: a file vanished while it was being compared\n");
      return 2;
    }

    Decode(DataA, Res.Kind, DecA);
    Decode(DataB, Res.Kind, DecB);
    PrintDetail(Res.Role, DataA, DataB, DecA, DecB, Res);
  }

  for(ulong c = 0; c < SkippedA.size(); ++c)
    if(EndsWith(SkippedA[c], ".tmp"))
      fprintf(stderr, "warning: %s/%s exists - a save crashed halfway through writing\n",
              Positional[0].c_str(), SkippedA[c].c_str());

  for(ulong c = 0; c < SkippedB.size(); ++c)
    if(EndsWith(SkippedB[c], ".tmp"))
      fprintf(stderr, "warning: %s/%s exists - a save crashed halfway through writing\n",
              Positional[1].c_str(), SkippedB[c].c_str());

  /* One predicate decides both the exit status and the JSON summary. "differ"
   * used to be AnyDiff alone, so a run whose only divergence was a SUSPECT
   * said differ:false and exited 1, and a CI step keyed on either one of them
   * disagreed with a step keyed on the other. "hard_differ" keeps the narrow
   * reading for anyone who wants it. */

  cint Status = AnyDiff || (AnySuspect && !IgnoreTimeSpent) ? 1 : 0;

  if(JsonMode)
    printf("{\"type\":\"summary\",\"roles\":%lu,\"differ\":%s,\"suspect\":%s,"
           "\"hard_differ\":%s,\"exit\":%d}\n",
           ulong(Roles.size()), Status ? "true" : "false",
           AnySuspect ? "true" : "false", AnyDiff ? "true" : "false", Status);
  else if(!AnyDiff && !AnySuspect)
    printf("\nall %lu file%s match\n", ulong(Roles.size()), Roles.size() == 1 ? "" : "s");

  return Status;
}
