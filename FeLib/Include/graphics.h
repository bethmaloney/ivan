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

#ifndef __GRAPHICS_H__
#define __GRAPHICS_H__

#include <vector>

#ifdef USE_SDL
#include "SDL.h"
#endif

#include "v2.h"

#define DOUBLE_BUFFER graphics::GetDoubleBuffer()
#define RES graphics::GetRes()
#define FONT graphics::GetDefaultFont()

class bitmap;
class rawbitmap;
class festring;

typedef void (*drawabove)(bitmap*);

class graphics
{
 public:
  friend class bitmap;
  static void Init();
  static void DeInit();
  static void SetAllowMouseInFullScreen(bool b);
  static int GetScale(){return Scale;}

#ifdef USE_SDL
  static void SetScale(int);
  static void SwitchMode();
  static void SetMode(cchar*, cchar*, v2, int, int, truth);
#endif

#ifdef __DJGPP__
  static void SwitchMode() { }
  static void SetMode(cchar*, cchar*, v2, truth);
#endif

  static void Stretch(bool, bitmap*, blitdata&, bool);
  static void DrawRectangleOutlineAround(bitmap* bmpAt, v2 v2TopLeft, v2 v2Border, col16 color, bool wide);
  static void BlitDBToScreen();

  static void DrawAboveAll(bitmap* bmpBuffer);
  static void AddDrawAboveAll(drawabove da, int iPriority, const char* desc);

  static v2 GetRes() { return Res; }
  static bitmap* GetDoubleBuffer() { return DoubleBuffer; }
  static void LoadDefaultFont(cfestring&);
  static rawbitmap* GetDefaultFont() { return DefaultFont; }
  static void SetSwitchModeHandler(void (*What)()){ SwitchModeHandler = What; }

  static int AddStretchRegion(blitdata B,const char* strId);
  static int GetTotSRegions();
  static void SetSpecialListItemAltPos(bool b){bSpecialListItemAltPos=b;}
  static void SetAllowStretchedBlit(){bAllowStretchedRegionsBlit=true;} //as the dungeon shows most of the time,
  static void SetDenyStretchedBlit(){bAllowStretchedRegionsBlit=false;} //it should be denied only during a few moments.
  static bool isStretchedRegionsAllowed();
  static void PrepareBeforeDrawingFelist();
  static void DrawAtDoubleBufferBeforeFelistPage();
  static bitmap* PrepareBuffer();

  //TODO utility class for sregion?
  static bool IsSRegionEnabled(int iIndex);
  static void SetSRegionEnabled(int iIndex, bool b);
  static void SetSRegionUseXBRZ(int iIndex, bool b);
  static void SetSRegionDrawAfterFelist(int iIndex, bool b);
  static void SetSRegionDrawAlways(int iIndex, bool b);
  static void SetSRegionDrawBeforeFelistPage(int iIndex, bool, bool);
  static void SetSRegionDrawRectangleOutline(int iIndex, bool b);
  static void SetSRegionSrcBitmapOverride(int iIndex, bitmap* bmp, int iStretch, v2 v2Dest);
  static void SetSRegionListItem(int iIndex);
  static int  SetSRegionBlitdata(int iIndex, blitdata B);
  static void SetSRegionClearSquaresAt(int iIndex, v2 v2Size, std::vector<v2> vv2);

#ifdef USE_SDL
  #if SDL_MAJOR_VERSION == 1
    static SDL_Surface* Screen;
    #if SDL_BYTEORDER == SDL_BIG_ENDIAN
      static SDL_Surface* TempSurface;
    #endif
  #else
    public:
      static SDL_Window* GetWindow(){return Window;};
    private:
      static SDL_Window* Window;
      static SDL_Renderer *Renderer;
      static SDL_Texture *Texture;
  #endif
#endif

 private:
  static void (*SwitchModeHandler)();
#ifdef __DJGPP__
  static ulong BufferSize;
  static ushort ScreenSelector;
  static struct vesainfo
  {
    void Retrieve();
    ulong Signature HARDWARE_LAYOUT;
    ushort Version HARDWARE_LAYOUT;
    ulong OEMString HARDWARE_LAYOUT;
    ulong Capabilities HARDWARE_LAYOUT;
    ulong ModeList HARDWARE_LAYOUT;
    ushort Memory HARDWARE_LAYOUT;
    uchar Shit[493] HARDWARE_LAYOUT;
  } VesaInfo;
  static struct modeinfo
  {
    void Retrieve(ushort);
    ushort Attribs1 HARDWARE_LAYOUT;
    uchar AWindowAttribs HARDWARE_LAYOUT;
    uchar BWindowAttribs HARDWARE_LAYOUT;
    ushort Granularity HARDWARE_LAYOUT;
    ushort WindowSize HARDWARE_LAYOUT;
    ushort WindowASegment HARDWARE_LAYOUT;
    ushort WindowBSegment HARDWARE_LAYOUT;
    ulong WindowMoveFunction HARDWARE_LAYOUT;
    ushort BytesPerLine HARDWARE_LAYOUT;
    ushort Width HARDWARE_LAYOUT;
    ushort Height HARDWARE_LAYOUT;
    uchar CharWidth HARDWARE_LAYOUT;
    uchar CharHeight HARDWARE_LAYOUT;
    uchar Planes HARDWARE_LAYOUT;
    uchar BitsPerPixel HARDWARE_LAYOUT;
    uchar Banks HARDWARE_LAYOUT;
    uchar MemoryModel HARDWARE_LAYOUT;
    uchar BankSize HARDWARE_LAYOUT;
    uchar ImagePages HARDWARE_LAYOUT;
    uchar Reserved1 HARDWARE_LAYOUT;
    uchar RedBits HARDWARE_LAYOUT;
    uchar RedShift HARDWARE_LAYOUT;
    uchar GreenBits HARDWARE_LAYOUT;
    uchar GreenShift HARDWARE_LAYOUT;
    uchar BlueBits HARDWARE_LAYOUT;
    uchar BlueShift HARDWARE_LAYOUT;
    uchar ResBits HARDWARE_LAYOUT;
    uchar ResShift HARDWARE_LAYOUT;
    uchar Attribs2 HARDWARE_LAYOUT;
    ulong PhysicalLFBAddress HARDWARE_LAYOUT;
    ulong OffScreenMem HARDWARE_LAYOUT;
    ushort OffScreenMemSize HARDWARE_LAYOUT;
    uchar Reserved2[206] HARDWARE_LAYOUT;
  } ModeInfo;
#endif
  static bitmap* DoubleBuffer;
  static bitmap* StretchedBuffer;
  static truth bAllowStretchedRegionsBlit;
  static truth bSpecialListItemAltPos;
  static v2 Res;
  static int Scale;
  static int ColorDepth;
  static rawbitmap* DefaultFont;
};

#endif
