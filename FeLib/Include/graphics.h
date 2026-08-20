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
 public:
  static SDL_Window* GetWindow(){return Window;};
 private:
  static SDL_Window* Window;
  static SDL_Renderer *Renderer;
  static SDL_Texture *Texture;
#endif

 private:
  static void (*SwitchModeHandler)();
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
