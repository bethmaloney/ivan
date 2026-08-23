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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "harness.h"

#include "bitmap.h"
#include "felibdef.h"
#include "festring.h"
#include "graphics.h"

#ifndef IVAN_VERSION
#define IVAN_VERSION "unknown"
#endif

truth harness::Recording = false;
truth harness::Replaying = false;
truth harness::Tracing = false;
truth harness::CapturingText = false;
truth harness::Headless = false;
ulong harness::RandCount = 0;
ulong harness::GameRandCount = 0;
ulong harness::VisualRandCount = 0;
int harness::SeedDepth = 0;
ulong harness::NestedBrackets = 0;

/*
 * std::ofstream and std::ifstream are used deliberately instead of
 * outputfile/inputfile: outputfile writes to a .tmp, keeps a .bkp of the
 * previous file and only moves the result into place in Close(), which is
 * exactly wrong for an append style log that must survive a crash.
 *
 * These streams are constructed during static initialisation, i.e. before
 * ParseArgs() registers Shutdown() with atexit, so under atexit's LIFO order
 * Shutdown() always runs before their destructors.
 */

static std::ofstream RecordFile;
static std::ofstream TraceFile;
static std::ifstream ReplayFile;

static cchar* ReplayFileName = 0;
static ulong ReplayLine = 0;
static ulong ReplayCount = 0;
static truth ReplayExhausted = false;
static ulong ReplayTrailerKeys = 0;
static truth ReplayTrailerSeen = false;

static ulong FrameIndex = 0;
static ulong KeySeq = 0;
static ulong TraceIndex = 0;

static ulong SeedOverride = 0;
static truth SeedOverrideSet = false;
static truth SeedFromArg = false;

/* The presentation generator's seed (visualrand, femath.h). Fixed by default,
   because the frame hashes and the screenshots have to reproduce; --visual-seed
   random is the fuzz arm, which asserts that varying it moves no game state. */

static ulong VisualSeed = 0x1AA2;

static truth MouseWarningGiven = false;

static cchar* ShotName = 0;
static cchar* ShotDirName = 0;
static ulong ShotCount = 0;
static std::ofstream TextLogFile;

cint RecordVersion = 1;
cint TraceVersion = 1;
cint LineSize = 512;

/* FNV-1a, 64 bits. Not 32: over a corpus of the size a differential run
   produces a 32 bit digest collides with near certainty, and a collision is a
   silent false pass, which is the one failure this tool cannot detect. */

typedef unsigned long long hash64;
typedef const unsigned long long chash64;

/* Not ulong: typedef.h makes that 32 bits under MSVC and MinGW. */

chash64 FNVOffsetBasis = 14695981039346656037ULL;
chash64 FNVPrime = 1099511628211ULL;

static hash64 LastHash = 0;
static ulong LastRandCount = 0;

static inline void HashByte(hash64& Hash, uchar Byte)
{
  Hash ^= Byte;
  Hash *= FNVPrime;
}

static void HashInt(hash64& Hash, int Value)
{
  culong Bits = ulong(Value);

  for(int c = 0; c < 4; ++c)
    HashByte(Hash, uchar((Bits >> (c << 3)) & 0xFF));
}

[[noreturn]] static void Fail(cfestring& Message)
{
  std::cerr << "harness: " << Message.CStr() << std::endl;
  exit(1);
}

/* The value is rejected if it looks like an option, so that a flag written
   without one swallows the next flag instead of silently becoming its value:
   --record --seed 7 would otherwise record into a file called "--seed" and
   leave the seed unpinned, which is the opposite of what was asked for. A file
   genuinely named like an option can still be given as ./--seed. */

static cchar* NextArg(int argc, char** argv, int& Index)
{
  if(Index + 1 >= argc)
    Fail(festring("option ") << argv[Index] << " needs a value");

  cchar* Value = argv[Index + 1];

  if(Value[0] == '-' && Value[1] == '-' && Value[2])
    Fail(festring("option ") << argv[Index] << " needs a value, but was"
         " followed by " << Value);

  return argv[++Index];
}

/* std::ofstream::open truncates, so letting two of these name one file would
   have the recorder destroy the recording the replay is still reading, or one
   log overwrite the other. Comparing the spelling catches the way it is
   actually mistyped, --record x --replay x; two different spellings of one
   file are not caught, which is why nothing downstream trusts a recording it
   did not finish reading (see the trailer check in NextReplayKey). */

static void RejectSameFile(cchar* AName, cchar* A, cchar* BName, cchar* B)
{
  if(A && B && festring(A) == B)
    Fail(festring(AName) << " and " << BName << " are both " << A
         << "; they must be different files");
}

/* Blank lines and lines whose first non-blank character is '#' carry no
   records, so both the header search and the key reader drop them. */

static truth IsSkippable(cchar* Line)
{
  while(*Line == ' ' || *Line == '\t')
    ++Line;

  return !*Line || *Line == '#';
}

/* std::istream::getline() sets failbit on an overlong line, which would turn
   a corrupt recording into a silent stop. This truncates instead, so the line
   still reaches the parser and is reported as malformed.

   Terminated says whether the line ended at a newline rather than at end of
   file. A record cut off mid number still parses, so the caller needs this to
   tell a complete recording from one a crash interrupted. */

static truth ReadLine(std::ifstream& File, char* Buffer, int Size,
                      truth& Terminated)
{
  int C = File.get();

  if(C == EOF)
    return false;

  int Length = 0;

  for(; C != EOF && C != '\n'; C = File.get())
    if(C != '\r' && Length < Size - 1)
      Buffer[Length++] = char(C);

  Buffer[Length] = 0;
  Terminated = C == '\n';
  return true;
}

/* Cosmetic only, the decimal key on the same line is what replay reads.
   0x205 is both KEY_CONTROLLER_A and KEY_CONTROLLER_DIRECTION + 5; the
   direction naming wins because it is the common case. */

static void DescribeKey(int Key, char* Buffer, int Size)
{
  cchar* Name = 0;

  switch(Key)
  {
   case KEY_BACK_SPACE: Name = "KEY_BACK_SPACE"; break;
   case KEY_ESC: Name = "KEY_ESC"; break;
   case KEY_ENTER: Name = "KEY_ENTER"; break;
   case KEY_HOME: Name = "KEY_HOME"; break;
   case KEY_UP: Name = "KEY_UP"; break;
   case KEY_PAGE_UP: Name = "KEY_PAGE_UP"; break;
   case KEY_LEFT: Name = "KEY_LEFT"; break;
   case KEY_RIGHT: Name = "KEY_RIGHT"; break;
   case KEY_END: Name = "KEY_END"; break;
   case KEY_DOWN: Name = "KEY_DOWN"; break;
   case KEY_PAGE_DOWN: Name = "KEY_PAGE_DOWN"; break;
   case KEY_DELETE: Name = "KEY_DELETE"; break;
   case KEY_INSERT: Name = "KEY_INSERT"; break;
   case KEY_SPECIAL: Name = "KEY_SPECIAL"; break;
   case KEY_SPACE: Name = "KEY_SPACE"; break;
   case KEY_NUMPAD_5: Name = "KEY_NUMPAD_5"; break;
   case KEY_CONTROLLER_B: Name = "KEY_CONTROLLER_B"; break;
   case KEY_CONTROLLER_X: Name = "KEY_CONTROLLER_X"; break;
   case KEY_CONTROLLER_Y: Name = "KEY_CONTROLLER_Y"; break;
   case KEY_MOUSE_EVENT: Name = "KEY_MOUSE_EVENT"; break;
  }

  if(Name)
    snprintf(Buffer, Size, "%s", Name);
  else if(Key > KEY_CONTROLLER_DIRECTION && Key < KEY_CONTROLLER_DIRECTION + 10)
    snprintf(Buffer, Size, "KEY_CONTROLLER_DIRECTION+%d",
             Key - KEY_CONTROLLER_DIRECTION);
  else if(Key >= 0x20 && Key < 0x7F)
    snprintf(Buffer, Size, "'%c'", char(Key));
  else
    snprintf(Buffer, Size, "0x%X", Key);
}

static void OpenReplay(cchar* Name)
{
  ReplayFile.open(Name);

  if(!ReplayFile.is_open())
    Fail(festring("cannot open replay file ") << Name);

  ReplayFileName = Name;

  char Line[LineSize];
  truth Terminated = false;
  int Version = 0;
  ulong Seed = 0;

  for(;;)
  {
    if(!ReadLine(ReplayFile, Line, LineSize, Terminated))
      Fail(festring("replay file ") << Name << " ends before its header");

    ++ReplayLine;

    if(!Terminated)
      Fail(festring("replay file ") << Name << " is truncated at line "
           << ReplayLine);

    if(IsSkippable(Line))
      continue;

    if(sscanf(Line, "ivan-record %d seed=%lu", &Version, &Seed) != 2)
      Fail(festring("replay file ") << Name << " line " << ReplayLine
           << ": expected an ivan-record header, got \"" << Line << '"');

    break;
  }

  if(Version != RecordVersion)
    Fail(festring("replay file ") << Name << " is format version " << Version
         << ", this build writes version " << RecordVersion);

  if(SeedFromArg)
    std::cout << "harness: --seed " << SeedOverride
              << " overrides the seed " << Seed << " stored in " << Name
              << std::endl;
  else
  {
    SeedOverride = Seed;
    SeedOverrideSet = true;
  }

  harness::Replaying = true;
}

static void OpenRecord(cchar* Name)
{
  RecordFile.open(Name);

  if(!RecordFile.is_open())
    Fail(festring("cannot open record file ") << Name << " for writing");

  RecordFile << "# IVAN differential harness recording\n"
             << "# K <seq> <frame> <rng> <key>\n"
             << "# <frame> and <rng> are debug context only, replay never"
                " waits for them\n"
             << "# <frame> is the same count as \"frame\" in a --trace file,"
                " which omits a frame that repeats the one before it, so look"
                " for the last one not greater\n"
             << "ivan-record " << RecordVersion << " seed=" << SeedOverride
             << " ivan=" << IVAN_VERSION << '\n';
  RecordFile.flush();

  harness::Recording = true;
}

/* Defined with the rest of the capture code further down, but ParseArgs needs
   it to reject a --shot whose sidecar would overwrite another output. */

static std::string SidecarName(cchar* FileName);

static void OpenTextLog(cchar* Name)
{
  TextLogFile.open(Name);

  if(!TextLogFile.is_open())
    Fail(festring("cannot open text log ") << Name << " for writing");

  TextLogFile << "# IVAN text log: every string drawn, in draw order\n"
              << "# T <frame> <db|buf> <x> <y> <text>\n";
  TextLogFile.flush();
}

static void OpenTrace(cchar* Name)
{
  TraceFile.open(Name);

  if(!TraceFile.is_open())
    Fail(festring("cannot open trace file ") << Name << " for writing");

  harness::Tracing = true;

  if(getenv("IVAN_SHOWFPS"))
    std::cerr << "harness: IVAN_SHOWFPS is set, it draws a wall clock reading"
                 " into the double buffer and will make every frame hash"
                 " differ" << std::endl;
}

void harness::ParseArgs(int argc, char** argv)
{
  static truth Parsed = false;

  if(Parsed)
    return;

  Parsed = true;

  cchar* RecordName = 0;
  cchar* ReplayName = 0;
  cchar* TraceName = 0;
  cchar* TextName = 0;

  for(int c = 1; c < argc; ++c)
  {
    cfestring Arg(argv[c]);

    if(Arg == "--record")
      RecordName = NextArg(argc, argv, c);
    else if(Arg == "--replay")
      ReplayName = NextArg(argc, argv, c);
    else if(Arg == "--trace")
      TraceName = NextArg(argc, argv, c);
    else if(Arg == "--shot")
      ShotName = NextArg(argc, argv, c);
    else if(Arg == "--shot-dir")
      ShotDirName = NextArg(argc, argv, c);
    else if(Arg == "--text")
      TextName = NextArg(argc, argv, c);
    else if(Arg == "--headless")
    {
      /* Set here rather than below the early return, so --headless works on
         its own: it opens no file and pins no seed, and a plain headless run
         with no recording is a legitimate thing to ask for. */

      Headless = true;
    }
    else if(Arg == "--seed")
    {
      cchar* Value = NextArg(argc, argv, c);
      char* End;
      culong Seed = strtoul(Value, &End, 10);

      if(!*Value || *End)
        Fail(festring("--seed wants a decimal number, got \"") << Value << '"');

      SeedOverride = Seed;
      SeedOverrideSet = true;
      SeedFromArg = true;
    }
    else if(Arg == "--visual-seed")
    {
      cchar* Value = NextArg(argc, argv, c);

      if(festring(Value) == "random")
        VisualSeed = ulong(time(0)) ^ (ulong(clock()) << 16);
      else
      {
        char* End;
        culong Seed = strtoul(Value, &End, 10);

        if(!*Value || *End)
          Fail(festring("--visual-seed wants a decimal number or \"random\","
                        " got \"") << Value << '"');

        VisualSeed = Seed;
      }
    }
  }

  if(!RecordName && !ReplayName && !TraceName && !SeedOverrideSet
     && !ShotName && !ShotDirName && !TextName)
    return;

  /* Before anything is opened: the first open would already have truncated the
     file the second one wants. */

  RejectSameFile("--record", RecordName, "--replay", ReplayName);
  RejectSameFile("--record", RecordName, "--trace", TraceName);
  RejectSameFile("--replay", ReplayName, "--trace", TraceName);
  RejectSameFile("--record", RecordName, "--text", TextName);
  RejectSameFile("--replay", ReplayName, "--text", TextName);
  RejectSameFile("--trace", TraceName, "--text", TextName);

  /* The shot sidecar is derived from the shot name, so a shot called foo.txt
     would have its own text layer overwrite it, and one called foo.rec would
     eat the recording. Caught here rather than left to produce a confusing
     empty file later. */

  if(ShotName)
  {
    const std::string Sidecar = SidecarName(ShotName);

    if(Sidecar == ShotName)
      Fail(festring("--shot ") << ShotName << " collides with its own text"
           " sidecar; use a .png name");

    RejectSameFile("--shot sidecar", Sidecar.c_str(), "--record", RecordName);
    RejectSameFile("--shot sidecar", Sidecar.c_str(), "--replay", ReplayName);
    RejectSameFile("--shot sidecar", Sidecar.c_str(), "--trace", TraceName);
    RejectSameFile("--shot sidecar", Sidecar.c_str(), "--text", TextName);
  }

  atexit(Shutdown);

  /* The replay header carries the seed of the run that produced it. Without
     this the replay would reseed from time(0) and diverge on turn one. */

  if(ReplayName)
    OpenReplay(ReplayName);

  /* Any harness mode pins the seed, so a recording or a trace is always
     self describing. Note that continuing a saved game reseeds from the save
     and ignores this entirely. */

  if(!SeedOverrideSet)
  {
    SeedOverride = ulong(time(0));
    SeedOverrideSet = true;
    std::cout << "harness: no --seed given, using " << SeedOverride
              << std::endl;
  }

  if(RecordName)
    OpenRecord(RecordName);

  if(TraceName)
    OpenTrace(TraceName);

  if(TextName)
    OpenTextLog(TextName);

  /* Any capture mode needs the text layer: it is what makes a shot readable,
     and rawbitmap::Printf pays nothing for it when this stays false. */

  if(ShotName || ShotDirName || TextName)
    CapturingText = true;
}

void harness::Shutdown()
{
  /* Shutdown runs twice on the usual path: ReplayKey calls it explicitly when
     the recording runs out and then exits, which fires the atexit copy. The
     shot must be taken on the first call, while the double buffer still holds
     the screen the run ended on. */

  static truth ShotTaken = false;

  if(ShotName && !ShotTaken)
  {
    ShotTaken = true;
    WriteShot(ShotName, "run ended");
  }

  if(TextLogFile.is_open())
  {
    TextLogFile << "# end frames=" << FrameIndex << '\n';
    TextLogFile.close();
  }

  /* Said out loud because the number is much larger than it looks: a session
     that reaches the first dungeon level draws several hundred distinct
     frames, and at ~1.4MB apiece that is most of a gigabyte. */

  if(ShotDirName && ShotCount)
    std::cout << "harness: wrote " << ShotCount << " frame captures to "
              << ShotDirName << ", roughly "
              << ShotCount * 3 / 2 << "MB" << std::endl;

  if(RecordFile.is_open())
  {
    RecordFile << "# end keys=" << KeySeq << " frames=" << FrameIndex
               << " rng=" << RandCount << '\n';
    RecordFile.close();
  }

  if(TraceFile.is_open())
  {
    TraceFile.flush();
    TraceFile.close();
  }

  if(ReplayFile.is_open())
    ReplayFile.close();

  Recording = false;
  Tracing = false;
  CapturingText = false;

  /* Replaying is deliberately left set: the caller that drained the recording
     exits immediately after, and clearing it would let ReadKey and
     WaitForKeyEvent fall back into SDL_WaitEvent on a window nobody is
     feeding. NextReplayKey answers false from here on regardless. */

  ReplayExhausted = true;
}

void harness::RecordKey(int Key)
{
  if(!Recording)
    return;

  ++KeySeq;

  if(Key == KEY_MOUSE_EVENT && !MouseWarningGiven)
  {
    MouseWarningGiven = true;
    RecordFile << "# warning: mouse events are recorded but the pointer"
                  " position is not, so this recording will not replay"
                  " faithfully\n";
    std::cerr << "harness: recorded a mouse event, prefer keyboard only"
                 " recordings for differential runs" << std::endl;
  }

  char Name[64];
  DescribeKey(Key, Name, sizeof(Name));

  RecordFile << "K " << KeySeq << ' ' << FrameIndex << ' ' << RandCount << ' '
             << Key << "\t# " << Name << '\n';
  RecordFile.flush();
}

/* The trailer Shutdown() writes is the only thing telling a recording the
   recorder finished from one cut short at a line boundary, which is the shape
   a crash leaves: RecordKey flushes each key as it writes it, so a killed
   recorder loses nothing but the trailer. */

static void ReadTrailer(cchar* Line)
{
  while(*Line == ' ' || *Line == '\t')
    ++Line;

  ulong Keys;

  if(sscanf(Line, "# end keys=%lu", &Keys) == 1)
  {
    ReplayTrailerKeys = Keys;
    ReplayTrailerSeen = true;
  }
}

/*
 * Every way of failing below exits non zero rather than returning false.
 * Returning false means "the recording ended", which is how a replay finishes
 * normally, and the caller answers that by exiting 0. A damaged recording that
 * took the same route would make both sides of a differential run stop at the
 * same early key, emit matching short traces and pass.
 */

truth harness::NextReplayKey(int& Key)
{
  if(!Replaying || ReplayExhausted)
    return false;

  char Line[LineSize];
  truth Terminated = false;

  while(ReadLine(ReplayFile, Line, LineSize, Terminated))
  {
    ++ReplayLine;

    /* A line the writer never finished is not parsed: the fields it does have
       would otherwise deliver a plausible wrong key. */

    if(!Terminated)
      Fail(festring(ReplayFileName) << " line " << ReplayLine
           << " is truncated, so the recording is incomplete");

    if(IsSkippable(Line))
    {
      ReadTrailer(Line);
      continue;
    }

    ulong Seq, Frame, Rng;
    int Value;

    if(sscanf(Line, "K %lu %lu %lu %d", &Seq, &Frame, &Rng, &Value) != 4)
      Fail(festring(ReplayFileName) << " line " << ReplayLine
           << " is malformed: \"" << Line << '"');

    /* Frame and rng are context, but seq is a real integrity check: it catches
       an edited, spliced or partially written recording. */

    if(Seq != ReplayCount + 1)
      Fail(festring(ReplayFileName) << " line " << ReplayLine
           << " is out of sequence, expected key " << ReplayCount + 1
           << " but found " << Seq);

    Key = Value;
    ++ReplayCount;
    return true;
  }

  if(!ReplayTrailerSeen)
    Fail(festring(ReplayFileName) << " ends after " << ReplayCount
         << " keys with no \"# end\" trailer, so it was cut short; append"
            " \"# end keys=" << ReplayCount << "\" if that really is all of"
            " it");

  if(ReplayTrailerKeys != ReplayCount)
    Fail(festring(ReplayFileName) << " says it holds " << ReplayTrailerKeys
         << " keys but " << ReplayCount << " were read");

  ReplayExhausted = true;
  ReplayFile.close();

  std::cout << "harness: replay of " << ReplayFileName << " exhausted after "
            << ReplayCount << " keys, frame " << FrameIndex << ", rng "
            << RandCount << std::endl;

  return false;
}

/*
 * ---------------------------------------------------------------------------
 * Screen capture
 * ---------------------------------------------------------------------------
 *
 * PNG is written by hand rather than through bitmap::Save or libpng.
 *
 * bitmap::Save(cfestring&) writes a BMP, not a PNG despite the extension it
 * is usually given, and it writes through outputfile, which stages a .tmp and
 * only moves it into place on close - the wrong shape for a capture that has
 * to survive the crash it was taken to diagnose. It also emits no BMP row
 * padding, so it is only correct when the width happens to be a multiple of
 * four.
 *
 * libpng is linked into FeLib, but zlib only transitively, so compressing here
 * would mean linking one more library than the build currently needs. Deflate
 * "stored" blocks need no compressor at all: the result is a valid, universally
 * readable PNG that costs about 1.4MB at 800x600. That is the right trade for a
 * debugging capture - no new dependency, and nothing to go wrong.
 */

typedef unsigned int hash32;

static hash32 Crc32Table[256];
static truth Crc32TableReady = false;

static void InitCrc32Table()
{
  for(hash32 n = 0; n < 256; ++n)
  {
    hash32 C = n;

    for(int k = 0; k < 8; ++k)
      C = (C & 1) ? 0xEDB88320u ^ (C >> 1) : C >> 1;

    Crc32Table[n] = C;
  }

  Crc32TableReady = true;
}

static hash32 Crc32(const uchar* Data, ulong Length)
{
  if(!Crc32TableReady)
    InitCrc32Table();

  hash32 C = 0xFFFFFFFFu;

  for(ulong n = 0; n < Length; ++n)
    C = Crc32Table[(C ^ Data[n]) & 0xFF] ^ (C >> 8);

  return C ^ 0xFFFFFFFFu;
}

static hash32 Adler32(const uchar* Data, ulong Length)
{
  hash32 A = 1, B = 0;

  for(ulong n = 0; n < Length; ++n)
  {
    A = (A + Data[n]) % 65521u;
    B = (B + A) % 65521u;
  }

  return (B << 16) | A;
}

static void PushBE32(std::vector<uchar>& Out, hash32 Value)
{
  Out.push_back(uchar(Value >> 24));
  Out.push_back(uchar(Value >> 16));
  Out.push_back(uchar(Value >> 8));
  Out.push_back(uchar(Value));
}

static void WriteChunk(std::ofstream& Out, cchar* Type,
                       const std::vector<uchar>& Data)
{
  std::vector<uchar> Framed;
  Framed.reserve(Data.size() + 12);
  PushBE32(Framed, hash32(Data.size()));

  for(int c = 0; c < 4; ++c)
    Framed.push_back(uchar(Type[c]));

  Framed.insert(Framed.end(), Data.begin(), Data.end());

  /* The CRC covers the type and the payload but not the length. */

  PushBE32(Framed, Crc32(&Framed[4], hash32(Data.size()) + 4));
  Out.write(reinterpret_cast<cchar*>(&Framed[0]), std::streamsize(Framed.size()));
}

/* RGB565 to RGB888 with bit replication, so that white is 0xFFFFFF rather
   than 0xF8FCF8. bitmap::Save's BMP path shifts without replicating, which
   makes every screenshot subtly dark. */

static void Expand565(packcol16 Pixel, uchar* RGB)
{
  cint R = Pixel >> 11, G = (Pixel >> 5) & 0x3F, B = Pixel & 0x1F;

  RGB[0] = uchar((R << 3) | (R >> 2));
  RGB[1] = uchar((G << 2) | (G >> 4));
  RGB[2] = uchar((B << 3) | (B >> 2));
}

static truth WritePNG(cchar* FileName, bitmap* Buffer)
{
  std::ofstream Out(FileName, std::ios::binary | std::ios::trunc);

  if(!Out.is_open())
  {
    std::cerr << "harness: cannot open " << FileName << " for writing"
              << std::endl;
    return false;
  }

  cv2 Size = Buffer->GetSize();
  packcol16** Image = Buffer->GetImage();

  static const uchar Signature[8] =
    { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
  Out.write(reinterpret_cast<cchar*>(Signature), 8);

  std::vector<uchar> Header;
  PushBE32(Header, hash32(Size.X));
  PushBE32(Header, hash32(Size.Y));
  Header.push_back(8);  /* bit depth */
  Header.push_back(2);  /* colour type: truecolour RGB */
  Header.push_back(0);  /* deflate */
  Header.push_back(0);  /* adaptive filtering */
  Header.push_back(0);  /* no interlace */
  WriteChunk(Out, "IHDR", Header);

  /* Every scanline is prefixed with its filter type byte, which is what makes
     the raw stream wider than the pixel data. */

  std::vector<uchar> Raw;
  Raw.reserve(ulong(Size.Y) * (ulong(Size.X) * 3 + 1));

  for(int y = 0; y < Size.Y; ++y)
  {
    Raw.push_back(0);
    cpackcol16* Row = Image[y];

    for(int x = 0; x < Size.X; ++x)
    {
      uchar RGB[3];
      Expand565(Row[x], RGB);
      Raw.push_back(RGB[0]);
      Raw.push_back(RGB[1]);
      Raw.push_back(RGB[2]);
    }
  }

  std::vector<uchar> Data;
  Data.reserve(Raw.size() + Raw.size() / 8192 * 5 + 16);
  Data.push_back(0x78);  /* zlib: deflate, 32K window */
  Data.push_back(0x01);  /* no preset dictionary, fastest compression level */

  culong BlockMax = 65535;

  for(ulong Offset = 0; Offset < Raw.size() || !Offset; Offset += BlockMax)
  {
    culong Length = std::min(BlockMax, ulong(Raw.size()) - Offset);
    ctruth Final = Offset + Length >= Raw.size();

    Data.push_back(uchar(Final ? 1 : 0));  /* BFINAL, BTYPE 00 = stored */
    Data.push_back(uchar(Length & 0xFF));
    Data.push_back(uchar(Length >> 8));
    Data.push_back(uchar(~Length & 0xFF));
    Data.push_back(uchar((~Length >> 8) & 0xFF));
    Data.insert(Data.end(), Raw.begin() + Offset, Raw.begin() + Offset + Length);
  }

  PushBE32(Data, Adler32(Raw.empty() ? 0 : &Raw[0], ulong(Raw.size())));
  WriteChunk(Out, "IDAT", Data);
  WriteChunk(Out, "IEND", std::vector<uchar>());

  Out.flush();

  if(!Out.good())
  {
    std::cerr << "harness: write failed on " << FileName << std::endl;
    return false;
  }

  return true;
}

/*
 * The text layer.
 *
 * Text drawn to an offscreen bitmap (which is how felist draws menus and
 * inventory lists, and how the map notes overlay works) is tagged "buf"
 * rather than "db", because its coordinates are relative to that bitmap and
 * not to the screen. Both are kept: the string content is what makes a screen
 * readable, and dropping the offscreen ones would lose every menu.
 */

struct textentry
{
  const void* Target;
  int X;
  int Y;
  int Color;
  std::string Text;
};

/* Pending collects text as it is drawn; TraceFrame moves it to Frame when the
   frame it belongs to is blitted. Without the two stage handover a shot taken
   from Shutdown - which runs after the last blit - would find Pending already
   cleared and report no text on a screen that plainly has some. */

static std::vector<textentry> PendingText;
static std::vector<textentry> FrameText;
static ulong FrameTextFrame = 0;

static truth SortText(const textentry& A, const textentry& B)
{
  if(A.Y != B.Y)
    return A.Y < B.Y;

  return A.X < B.X;
}

static cchar* ColorName(int Color)
{
  switch(Color)
  {
   case RED: return "RED";
   case GREEN: return "GREEN";
   case BLUE: return "BLUE";
   case YELLOW: return "YELLOW";
   case PINK: return "PINK";
   case WHITE: return "WHITE";
   case LIGHT_GRAY: return "LIGHT_GRAY";
   case DARK_GRAY: return "DARK_GRAY";
   case BLACK: return "BLACK";
   default: return "-";
  }
}

void harness::RecordText(const void* Target, int X, int Y, int Color,
                         cchar* Text)
{
  if(!CapturingText || !Text || !*Text)
    return;

  /* A runaway draw loop must not turn a debugging aid into an out of memory
     abort. 4096 strings is an order of magnitude more than a full screen. */

  if(PendingText.size() >= 4096)
    return;

  textentry Entry;
  Entry.Target = Target;
  Entry.X = X;
  Entry.Y = Y;
  Entry.Color = Color;
  Entry.Text = Text;
  PendingText.push_back(Entry);
}

/* foo.png -> foo.txt, anything else -> itself with .txt appended, so the pair
   always lands beside each other whatever the caller passed. */

static std::string SidecarName(cchar* FileName)
{
  std::string Name(FileName);
  const size_t Dot = Name.rfind('.');

  if(Dot != std::string::npos && Name.find('/', Dot) == std::string::npos
     && Name.find('\\', Dot) == std::string::npos)
    Name.erase(Dot);

  return Name + ".txt";
}

static void WriteTextLayer(cchar* FileName, cchar* Reason, bitmap* Buffer)
{
  const std::string Name = SidecarName(FileName);
  std::ofstream Out(Name.c_str(), std::ios::trunc);

  if(!Out.is_open())
  {
    std::cerr << "harness: cannot open " << Name << " for writing" << std::endl;
    return;
  }

  cv2 Size = Buffer->GetSize();

  Out << "# IVAN screen text layer\n"
      << "# reason " << Reason << "\n"
      << "# frame " << FrameIndex << " rng " << harness::RandCount
      << " size " << Size.X << 'x' << Size.Y << '\n';

  if(FrameText.empty())
    Out << "# no strings were drawn for this frame\n";
  else if(FrameTextFrame != FrameIndex)
    Out << "# nothing was drawn since frame " << FrameTextFrame
        << ", so this is that frame's text; the screen has not changed its"
           " text since\n";

  /* A frame often draws the same slot twice - the side panel is redrawn after
     a move, so "Turn 0" and "Turn 1" both land at (704,360) - and only the
     last one is on the screen. Overdrawn strings are dropped from the layout
     view, which would otherwise read "Turn 0 Turn 1". They stay in the draw
     order list below, where seeing both is the point. */

  std::map<std::pair<const void*, std::pair<int, int> >, size_t> Last;

  for(size_t c = 0; c < FrameText.size(); ++c)
    Last[std::make_pair(FrameText[c].Target,
                        std::make_pair(FrameText[c].X, FrameText[c].Y))] = c;

  std::vector<textentry> Sorted;

  for(size_t c = 0; c < FrameText.size(); ++c)
    if(Last[std::make_pair(FrameText[c].Target,
                           std::make_pair(FrameText[c].X, FrameText[c].Y))] == c)
      Sorted.push_back(FrameText[c]);

  std::stable_sort(Sorted.begin(), Sorted.end(), SortText);

  /* The layout view. Strings sharing a y are one line, placed at x/8 because
     the font is 8 pixels wide, which reproduces the on screen columns. A
     string that would overlap the previous one is pushed right by a space
     rather than truncating it: readability beats pixel fidelity here. */

  Out << "\n--- layout (grouped by y, columns are x/8) ---\n";

  for(size_t c = 0; c < Sorted.size();)
  {
    cint Y = Sorted[c].Y;
    const void* Target = Sorted[c].Target;
    std::string Line;

    for(; c < Sorted.size() && Sorted[c].Y == Y
          && Sorted[c].Target == Target; ++c)
    {
      const size_t Column = size_t(std::max(0, Sorted[c].X) / 8);

      if(Line.size() < Column)
        Line.append(Column - Line.size(), ' ');
      else if(!Line.empty())
        Line += ' ';

      Line += Sorted[c].Text;
    }

    char Prefix[32];
    snprintf(Prefix, sizeof(Prefix), "%s y=%4d | ",
             Target == DOUBLE_BUFFER ? "db " : "buf", Y);
    Out << Prefix << Line << '\n';
  }

  /* Draw order matters when two strings land in the same place - the last one
     is the one visible - so the chronological list is kept as well. */

  Out << "\n--- draw order ---\n";

  for(size_t c = 0; c < FrameText.size(); ++c)
  {
    char Prefix[64];
    snprintf(Prefix, sizeof(Prefix), "%s (%4d,%4d) %-10s ",
             FrameText[c].Target == DOUBLE_BUFFER ? "db " : "buf",
             FrameText[c].X, FrameText[c].Y, ColorName(FrameText[c].Color));
    Out << Prefix << FrameText[c].Text << '\n';
  }

  Out.flush();
}

void harness::WriteShot(cchar* FileName, cchar* Reason)
{
  bitmap* Buffer = DOUBLE_BUFFER;

  if(!Buffer)
  {
    std::cerr << "harness: no double buffer yet, cannot capture " << FileName
              << std::endl;
    return;
  }

  if(WritePNG(FileName, Buffer))
    WriteTextLayer(FileName, Reason, Buffer);
}

/*
 * The double buffer is hashed, never PrepareBuffer(): the latter runs the
 * stretch regions and xBRZ, whose colour distances are computed in double, so
 * hashing it would drag both the user's scaling configuration and floating
 * point rounding differences between hosts into the comparison.
 *
 * This must be called at the top of BlitDBToScreen, before PrepareBuffer(),
 * which draws the DrawAboveAll overlays into the double buffer itself on
 * frames where no stretch region fired.
 */

/* The frame the text belongs to is the one about to be blitted, so the strings
   collected since the previous blit are handed over here. A frame that drew
   nothing leaves the previous handover in place: an unchanged screen still has
   the same text on it, and WriteTextLayer says which frame it came from. */

static void HandOverText()
{
  if(PendingText.empty())
    return;

  FrameText.swap(PendingText);
  PendingText.clear();
  FrameTextFrame = FrameIndex;

  if(!TextLogFile.is_open())
    return;

  for(size_t c = 0; c < FrameText.size(); ++c)
    TextLogFile << "T " << FrameIndex << ' '
                << (FrameText[c].Target == DOUBLE_BUFFER ? "db " : "buf") << ' '
                << FrameText[c].X << ' ' << FrameText[c].Y << ' '
                << FrameText[c].Text << '\n';

  TextLogFile.flush();
}

void harness::TraceFrame()
{
  ++FrameIndex;

  if(CapturingText)
    HandOverText();

  if(!Tracing && !ShotDirName)
    return;

  bitmap* Buffer = DOUBLE_BUFFER;

  if(!Buffer)
    return;

  /* Never cache the bitmap, its row pointers or the resolution: SetMode
     reallocates the double buffer, and the row pointer array lives in the
     same allocation as the pixels. */

  cv2 Size = Buffer->GetSize();
  packcol16** Image = Buffer->GetImage();
  hash64 Hash = FNVOffsetBasis;

  HashInt(Hash, Size.X);
  HashInt(Hash, Size.Y);

  for(int y = 0; y < Size.Y; ++y)
  {
    cpackcol16* Row = Image[y];

    /* Byte order is defined by the pixel value, not by the host, so a big
       endian build and a WASM build agree. */

    for(int x = 0; x < Size.X; ++x)
    {
      HashByte(Hash, uchar(Row[x]));
      HashByte(Hash, uchar(Row[x] >> 8));
    }
  }

  char Buf[192];

  /* Not "is TraceIndex zero": --shot-dir can be given without --trace, and
     then nothing ever increments TraceIndex, so the repeat test below would
     never fire and every frame would be written to disk. */

  static truth FirstFrameSeen = false;

  if(!FirstFrameSeen)
  {
    FirstFrameSeen = true;

    /* The resolution is unknown until SetMode has run, so the meta line waits
       for the first frame. */

    if(Tracing)
    {
      snprintf(Buf, sizeof(Buf),
               "{\"frame\":-1,\"hash\":\"0000000000000000\",\"rng\":0,"
               "\"index\":-1,\"ver\":%d,\"seed\":%lu,\"w\":%d,\"h\":%d}\n",
               TraceVersion, SeedOverride, Size.X, Size.Y);
      TraceFile << Buf;
    }
  }
  else if(Hash == LastHash && RandCount == LastRandCount)
    return;

  LastHash = Hash;
  LastRandCount = RandCount;

  /* Frames that repeat the one before them are skipped, which is what keeps a
     shot directory to the frames that actually look different. Each PNG is
     stored uncompressed, so at 800x600 that is about 1.4MB apiece - fine for
     stepping through a handful of turns, expensive for a whole session. */

  if(ShotDirName)
  {
    char Path[512];
    snprintf(Path, sizeof(Path), "%s/frame-%06lu.png", ShotDirName, FrameIndex);

    char Reason[64];
    snprintf(Reason, sizeof(Reason), "--shot-dir frame %lu", FrameIndex);
    WriteShot(Path, Reason);
    ++ShotCount;
  }

  if(!Tracing)
    return;

  /* "frame" is the BlitDBToScreen count: the same counter, from the same
     origin, as the frame column of a recording, so the number printed beside a
     key can be looked up here directly. A frame whose hash and rng both repeat
     the previous one is not emitted, so that lookup is the last "frame" not
     greater than the one wanted. "index" numbers the emitted records. */

  snprintf(Buf, sizeof(Buf),
           "{\"frame\":%lu,\"hash\":\"%016llx\",\"rng\":%lu,\"index\":%lu,"
           "\"grng\":%lu,\"nest\":%lu,\"depth\":%d}\n",
           FrameIndex, Hash, RandCount, TraceIndex++,
           GameRandCount, NestedBrackets, SeedDepth);
  TraceFile << Buf;

  /* Flushed per line on purpose: a SIGSEGV in a build without BACKTRACE never
     reaches atexit, and the frames just before a crash are the ones wanted. */

  TraceFile.flush();
}

truth harness::HasSeedOverride() { return SeedOverrideSet; }
ulong harness::GetSeedOverride() { return SeedOverride; }
ulong harness::GetVisualSeed() { return VisualSeed; }
