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

#ifndef __WHANDLER_H__
#define __WHANDLER_H__

#ifdef USE_SDL
#include <vector>
#include "SDL.h"
#endif

#include <queue>

#include "felibdef.h"
#include "festring.h"
#include "harness.h"

#define GET_KEY globalwindowhandler::GetKey
#define READ_KEY globalwindowhandler::ReadKey
#define GET_TICK globalwindowhandler::GetTick
#define WAIT_FOR_KEY_DOWN globalwindowhandler::WaitForKeyDown
#define WAIT_FOR_KEY_UP globalwindowhandler::WaitForKeyUp

struct mouseclick{
 int btn=-1;
 v2 pos;
 int wheelY=0;
 truth IsMotion=false;
};

class globalwindowhandler
{
 public:
  static bool IsKeyPressed(int iSDLScanCode);
  static void ResetKeyTimeout(){SetKeyTimeout(0,iRestWaitKey);}
  static void CheckKeyTimeout();
  static void SuspendKeyTimeout();
  static void ResumeKeyTimeout();
  static truth IsKeyTimeoutEnabled();
  static void SetKeyTimeout(int iTimeoutMillis,int iDefaultReturnedKey);
  static mouseclick GetLastMouseEvent() { return LastMouseEvent; } // after GetKey() returns KEY_MOUSE_EVENT
  static void SetPlayInBackground(truth b){playInBackground=b;}
  static float GetFPS(bool bInsta);
  static truth HasKeysOnBuffer();
  static uint PollEvents(SDL_Event* pEvent = NULL);
  static uint UpdateMouse();
  static int GetKey(truth = true);
  static int ReadKey();
  static truth WaitForKeyEvent(uint Key);
  static truth WaitForKeyDown(){return WaitForKeyEvent(SDL_KEYDOWN);}
  static truth WaitForKeyUp  (){return WaitForKeyEvent(SDL_KEYUP  );}
  static v2 GetMouseLocation();
  static bool IsMouseAtRect(v2, v2, bool = true, v2 = v2());
  static truth IsLastSDLkeyEventWasKeyUp();
  static void InstallControlLoop(truth (*)());
  static void DeInstallControlLoop(truth (*)());
  static ulong GetTick() { return Tick; }
  static truth ControlLoopsInstalled() { return Controls; }
  static void EnableControlLoops() { ControlLoopsEnabled = true; }
  static void DisableControlLoops() { ControlLoopsEnabled = false; }
  static truth ShiftIsDown();
  static void SetScrshotDirectory(cfestring& DirectoryName){ ScrshotDirectoryName = DirectoryName; }
  static festring ScrshotNameHandler(); // Number successive screenshots based on existing filenames
  static void SetAddFrameSkip(int i);
#ifdef USE_SDL
  static void Init();
  static void SetQuitMessageHandler(truth (*What)()){ QuitMessageHandler = What; }
  static ulong ReadTick() { return SDL_GetTicks() / 40; }
  static void SetFunctionKeyHandler(bool (*What)(SDL_Keycode)){ FunctionKeyHandler = What; }
  static void SetControlKeyHandler(bool (*What)(SDL_Keycode)){ ControlKeyHandler = What; }
#endif

#ifdef USE_SDL

  /* Tick is a wall clock reading, but it is not used as one: it picks the
     animation frame of every animated terrain, item and body part drawn, and
     char.cpp and rain.cpp let it reach game state. A replay must therefore
     count draws rather than read the clock, or two replays of one recording
     animate differently and none of their frame hashes can be compared. */

  static ulong UpdateTick()
  {
    return Tick = harness::IsReplaying() ? Tick + 1 : ReadTick();
  }

#endif

  const static int iRestWaitKey;
  static void AddKeyToBuffer(int KeyPressed);

  static bool ControllerEnabled() {
    #if SDL_MAJOR_VERSION == 2
    return controllers.size() > 0;
    #else
    return false;
    #endif
    }
  static v2 GetControllerDirection() {
    #if SDL_MAJOR_VERSION == 2
    return controller_direction;
    #else
    return v2(0, 0);
    #endif
    }

  static ulong GetClock()
  {
    return SDL_GetTicks();
  }

  static void WaitUntil(ulong t);

 private:
#ifdef USE_SDL
  static int ChkCtrlKey(SDL_Event* Event);
  static void ProcessMessage(SDL_Event*);
  static void ProcessKeyDownMessage(SDL_Event* Event);
  static std::vector<int> KeyBuffer;
  static truth (*QuitMessageHandler)();
  static bool (*FunctionKeyHandler)(SDL_Keycode);
  static bool (*ControlKeyHandler)(SDL_Keycode);
#endif
  static truth (*ControlLoop[MAX_CONTROLS])();
  static int Controls;
  static ulong Tick;
  static truth ControlLoopsEnabled;
  static truth playInBackground;
  static festring ScrshotDirectoryName;
  static void BufferMouseEvent(mouseclick mc);
  static std::queue<mouseclick> MouseBuffer;
  static mouseclick LastMouseEvent;
#if SDL_MAJOR_VERSION == 2
  static std::vector<SDL_GameController*> controllers;
  static v2 controller_direction;
#endif
};

#endif
