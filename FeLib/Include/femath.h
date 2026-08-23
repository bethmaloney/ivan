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

#ifndef __FEMATH_H__
#define __FEMATH_H__

#include <algorithm>
#include <vector>
#include <cmath>

#include "v2.h"
#include "rect.h"

/*
 * Never draw twice in one expression unless the two draws are interchangeable.
 *
 * C++ leaves function arguments, and the operands of arithmetic and relational
 * operators, unsequenced with respect to each other - so which draw of the
 * shared Mersenne Twister stream reaches which slot is the compiler's choice,
 * not the program's. GCC and Clang make opposite choices, and both are right.
 * Write
 *
 *     clong X = RAND_N(XSize);         // not v2(RAND_N(XSize), RAND_N(YSize))
 *     clong Y = RAND_N(YSize);
 *     v2 Pos(X, Y);
 *
 * and the draw order is the order you read. Note this is unspecified behaviour
 * rather than undefined: every build is internally consistent and reproduces
 * itself perfectly, which is why it survived twenty-five years and every
 * determinism test in this repo. It surfaces only when two *compilers* are
 * compared, which is exactly what a WASM port does - the player's eye colour
 * came out a different colour under Emscripten (docs/port-log.md §9.4).
 *
 * Interchangeable draws are fine and are left alone: RAND()%36 + RAND()%36 has
 * the same distribution and the same value whichever half is drawn first, and
 * &&, || and ?: sequence their operands by definition.
 */

/*
 * Two generators, and which one a draw lands on is a correctness question, not
 * a style one. RAND is the game's: the seed reproduces it, saves persist it and
 * anything it decides is shared by two players who typed the same seed. VRAND
 * is the presentation's, and nothing it returns may reach game state, a save
 * file or the game trace (docs/port-log.md §6.10c).
 *
 * The rule for choosing is not "is this drawing pixels" - fluid::imagedata's
 * blood drip walked past that reading twice. It is: could the number of times
 * this runs depend on anything outside the game? A draw behind a visibility
 * test depends on the camera, which depends on the player's window size and
 * zoom, and that is §6.10 - the defect this split exists to make unwritable.
 */

#define RAND femath::Rand
#define RAND_N femath::RandN
#define RAND_2 (femath::Rand() & 1)
#define RAND_4 (femath::Rand() & 3)
#define RAND_8 (femath::Rand() & 7)
#define RAND_16 (femath::Rand() & 15)
#define RAND_32 (femath::Rand() & 31)
#define RAND_64 (femath::Rand() & 63)
#define RAND_128 (femath::Rand() & 127)
#define RAND_256 (femath::Rand() & 255)
#define RAND_GOOD femath::RandGood

#define VRAND visualrand::Rand
#define VRAND_16 (visualrand::Rand() & 15)
#define KRAND visualrand::KeyedRand
#define KRAND_16 (visualrand::KeyedRand() & 15)
#define KRAND_32 (visualrand::KeyedRand() & 31)

class outputfile;
class inputfile;
template <class type> struct fearray;

/* One Mersenne Twister. Two instances exist and the difference between them is
   the whole of docs/port-log.md §6.10: femath's decides the game and is what a
   shared seed reproduces, visualrand's decides nothing that outlives the frame
   it is drawn on. */

class mtgen
{
 public:
  explicit mtgen(ulong* Counter = 0) : Counter(Counter) {}
  void SetSeed(ulong);
  long Draw();

 private:
  /* Counted here rather than in the callers so that a stream handed out by
     reference - bitmap::CreateLightning takes one - is still counted. Null on
     the game's, whose counter also has to decide grng; femath::Rand does it. */

  ulong* Counter;
  ulong mt[624];
  long mti = 625; /* > 624, so Draw() seeds itself rather than read garbage */

  friend class femath; /* SaveSeed needs the raw state; see the note there */
};

class femath
{
 public:
  static long Rand();
  static void SetSeed(ulong);
  static long RandN(long N) { return long(double(N) * Rand() / 0x80000000); }
  static long RandGood(long N) { return long(double(N) * Rand() / 0x80000000); }
  static double RandReal(double N = 1.) { return Rand() * (1. / 0x80000000) * N; }
  static double NormalDistributedRand(double StandardDeviation = 1.);
  static int WeightedRand(long*, long);
  static int WeightedRand(const std::vector<long>&, long);
  static double CalculateAngle(v2);
  static void CalculateEnvironmentRectangle(rect&, const rect&, v2, int);
  static truth Clip(int&, int&, int&, int&, int&, int&, int, int, int, int);
  static void SaveSeed();
  static void LoadSeed();
  static long SumArray(const fearray<long>&);
  static int LoopRoll(int, int);
  static void GenerateFractalMap(int**, int, int, int);

  /* Use this rather than std::random_shuffle for anything the game depends on.
     Its two argument form draws from std::rand, which is a second generator:
     SetSeed does not reach it, harness::CountRand cannot see it, and the audio
     thread draws from it concurrently, so a shuffle decided by it is not
     reproducible even from a pinned seed. */

  template <class type> static void Shuffle(type First, type Last)
  {
    for(type i = Last; i - First > 1; --i)
      std::iter_swap(i - 1, First + RandN(long(i - First)));
  }

 protected:
  static mtgen Game;
  static ulong mtb[];
  static long mtib;
};

/*
 * The presentation streams. Two of them, because they answer to different
 * things:
 *
 *   VRAND  free-running. The caller wants "some" randomness and does not care
 *          that two runs differ - fluid drips, explosion mirroring, particles,
 *          the wand beams. Seeded once from --visual-seed and never again.
 *   KRAND  keyed. The caller reseeds from an object identity first, so an item
 *          draws the same flames on every frame - bitmap::CreateFlames,
 *          CreateFlies, CreateLightning, object::RandomizeSparklePos. These are
 *          upstream's original SaveSeed users and were never isolation
 *          brackets; the seed is a key.
 *
 * Sharing one generator between the two was tried and is wrong, for a reason
 * that is about testing rather than correctness: a keyed site reseeds
 * immediately before drawing, so its own output is a pure function of its key
 * whatever ran before it, but it lands on the free-running stream as well.
 * Measured on autoplay-200, 681 keyed reseeds against 3,152 draws - which left
 * --visual-seed reaching nothing and the fuzz arm passing vacuously
 * (docs/port-log.md §6.10c).
 *
 * There is no SaveSeed here and there is not meant to be. Restoring state is
 * what the game's generator needs so a discarded draw stays discarded; these
 * have nothing to discard, and the absence of the mechanism is what makes the
 * brackets' nesting hazard (harness.h) unreachable from presentation.
 */

class visualrand
{
 public:
  static long Rand();
  static void SetSeed(ulong);
  static long KeyedRand();
  static void SetKey(ulong);

  /* For the one draw routine both disciplines call, bitmap::CreateLightning's
     four argument form: its caller says which stream it is tracing on. */

  static mtgen& FreeStream();
  static mtgen& KeyedStream();

 private:
  static mtgen Free;
  static mtgen Keyed;
};

struct interval
{
  long Randomize() const
  { return Min < Max ? Min + RAND() % (Max - Min + 1) : Min; }
  long Min;
  long Max;
};

struct region
{
  /* Both halves draw, and the two arguments of v2() are unsequenced, so which
     draw becomes X and which becomes Y would otherwise be the compiler's
     choice - see the note above the RAND macros. This one decides where every
     generated room sits and how big it is. */

  v2 Randomize() const
  {
    clong RX = X.Randomize();
    clong RY = Y.Randomize();
    return v2(RX, RY);
  }
  interval X;
  interval Y;
};

void ReadData(interval&, inputfile&);
void ReadData(region&, inputfile&);

outputfile& operator<<(outputfile&, const interval&);
inputfile& operator>>(inputfile&, interval&);
outputfile& operator<<(outputfile&, const region&);
inputfile& operator>>(inputfile&, region&);

template <class controller> class mapmath
{
 public:
  static truth DoLine(int, int, int, int, int = 0);
  static void DoArea();
  static void DoQuadriArea(int, int, int, int, int);
};

template <class controller>
inline truth mapmath<controller>::DoLine(int X1, int Y1,
                                         int X2, int Y2, int Flags)
{
  if(!(Flags & SKIP_FIRST))
    controller::Handler(X1, Y1);

  cint DeltaX = abs(X2 - X1);
  cint DeltaY = abs(Y2 - Y1);
  cint DoubleDeltaX = DeltaX << 1;
  cint DoubleDeltaY = DeltaY << 1;
  cint XChange = X1 < X2 ? 1 : -1;
  cint YChange = Y1 < Y2 ? 1 : -1;
  int x = X1, y = Y1;

  if(DeltaX >= DeltaY)
  {
    int c = DeltaX;
    cint End = X2;

    while(x != End)
    {
      x += XChange;
      c += DoubleDeltaY;

      if(c >= DoubleDeltaX)
      {
        c -= DoubleDeltaX;
        y += YChange;
      }

      if(!controller::Handler(x, y))
        return x == End && !(Flags & ALLOW_END_FAILURE);
    }
  }
  else
  {
    int c = DeltaY;
    cint End = Y2;

    while(y != End)
    {
      y += YChange;
      c += DoubleDeltaX;

      if(c >= DoubleDeltaY)
      {
        c -= DoubleDeltaY;
        x += XChange;
      }

      if(!controller::Handler(x, y))
        return y == End && !(Flags & ALLOW_END_FAILURE);
    }
  }

  return true;
}

struct basequadricontroller
{
  static cint OrigoDeltaX[4];
  static cint OrigoDeltaY[4];
  static int OrigoX, OrigoY;
  static int StartX, StartY;
  static int XSize, YSize;
  static int RadiusSquare;
  static truth SectorCompletelyClear;
};

template <class controller>
struct quadricontroller : public basequadricontroller
{
  static truth Handler(int, int);
  static int GetStartX(int I)
  {
    SectorCompletelyClear = true;
    return StartX = (OrigoX << 1) + OrigoDeltaX[I];
  }
  static int GetStartY(int I)
  {
    return StartY = (OrigoY << 1) + OrigoDeltaY[I];
  }
};

template <class controller>
truth quadricontroller<controller>::Handler(int x, int y)
{
  cint HalfX = x >> 1, HalfY = y >> 1;

  if(HalfX >= 0 && HalfY >= 0 && HalfX < XSize && HalfY < YSize)
  {
    ulong& SquareTick = controller::GetTickReference(HalfX, HalfY);
    cint SquarePartIndex = (x & 1) + ((y & 1) << 1);
    culong Mask = SquarePartTickMask[SquarePartIndex];

    if((SquareTick & Mask) < controller::ShiftedTick[SquarePartIndex])
    {
      SquareTick = (SquareTick & ~Mask)
                   | controller::ShiftedQuadriTick[SquarePartIndex];
      int DeltaX = OrigoX - HalfX, DeltaY = OrigoY - HalfY;

      if(DeltaX * DeltaX + DeltaY * DeltaY <= RadiusSquare)
      {
        if(SectorCompletelyClear)
        {
          if(controller::Handler(x, y))
            return true;
          else
            SectorCompletelyClear = false;
        }
        else
          return mapmath<controller>::DoLine(StartX, StartY,
                                             x, y,
                                             SKIP_FIRST
                                             |ALLOW_END_FAILURE);
      }
    }
  }

  return false;
}

cint ChangeXArray[4][3] = { { -1,  0, -1 },
                            {  0,  1,  1 },
                            { -1, -1,  0 },
                            {  1,  0,  1 } };
cint ChangeYArray[4][3] = { { -1, -1,  0 },
                            { -1, -1,  0 },
                            {  0,  1,  1 },
                            {  0,  1,  1 } };

template <class controller>
inline void mapmath<controller>::DoArea()
{
  int Buffer[2][2048];
  int* OldStack = Buffer[0];
  int* NewStack = Buffer[1];

  for(int c1 = 0; c1 < 4; ++c1)
  {
    cint* ChangeX = ChangeXArray[c1], * ChangeY = ChangeYArray[c1];
    int OldStackPos = 0, NewStackPos = 0;
    int StartX = controller::GetStartX(c1);
    int StartY = controller::GetStartY(c1);

    for(int c2 = 0; c2 < 3; ++c2)
    {
      OldStack[OldStackPos] = StartX + ChangeX[c2];
      OldStack[OldStackPos + 1] = StartY + ChangeY[c2];
      OldStackPos += 2;
    }

    while(OldStackPos)
    {
      while(OldStackPos)
      {
        OldStackPos -= 2;
        cint X = OldStack[OldStackPos], Y = OldStack[OldStackPos + 1];

        if(controller::Handler(X, Y))
          for(int c2 = 0; c2 < 3; ++c2)
          {
            NewStack[NewStackPos] = X + ChangeX[c2];
            NewStack[NewStackPos + 1] = Y + ChangeY[c2];
            NewStackPos += 2;
          }
      }

      OldStackPos = NewStackPos;
      NewStackPos = 0;
      int* T = OldStack;
      OldStack = NewStack;
      NewStack = T;
    }
  }
}

template <class controller>
inline void mapmath<controller>::DoQuadriArea(int OrigoX, int OrigoY,
                                              int RadiusSquare,
                                              int XSize, int YSize)
{
  basequadricontroller::OrigoX = OrigoX;
  basequadricontroller::OrigoY = OrigoY;
  basequadricontroller::RadiusSquare = RadiusSquare;
  basequadricontroller::XSize = XSize;
  basequadricontroller::YSize = YSize;

  for(int c = 0; c < 4; ++c)
    controller::Handler((OrigoX << 1) + basequadricontroller::OrigoDeltaX[c],
                        (OrigoY << 1) + basequadricontroller::OrigoDeltaY[c]);

  mapmath<quadricontroller<controller>>::DoArea();
}

/* Chance for n < Max to be returned is (1-CC)*CC^n,
   for n == Max chance is CC^n. */

inline int femath::LoopRoll(int ContinueChance, int Max)
{
  int R;
  for(R = 0; RAND_N(100) < ContinueChance && R < Max; ++R);
  return R;
}

template <class type, class predicate>
type*& ListFind(type*& Start, predicate Predicate)
{
  type** E;
  for(E = &Start; *E && !Predicate(*E); E = &(*E)->Next);
  return *E;
}

template <class type>
struct pointercomparer
{
  pointercomparer(const type* Element) : Element(Element) { }
  truth operator()(const type* E) const { return E == Element; }
  const type* Element;
};

#endif
