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

#include <iostream>
#include <cstdlib>

#ifdef BACKTRACE
#include <execinfo.h>
#endif

#ifndef WIN32
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#endif

#include "game.h"
#include "database.h"
#include "definesvalidator.h"
#include "devcons.h"
#include "feio.h"
#include "igraph.h"
#include "iconf.h"
#include "whandler.h"
#include "hscore.h"
#include "graphics.h"
#include "harness.h"
#include "script.h"
#include "specialkeys.h"
#include "message.h"
#include "namegen.h"
#include "proto.h"
#include "audio.h"
#include "sfx.h"

#include "dbgmsgproj.h"

#include "bugworkaround.h"

#ifdef BACKTRACE
void CrashHandler(int Signal)
{
  globalerrorhandler::DumpStackTraceToStdErr(Signal);
  exit(1);
}
#endif

void SkipGameScript(inputfile* pSaveFile){
  gamescript* gs=0;
  (*pSaveFile) >> gs; //dummy just to "seek" for next binary data TODO if dungeon and level was saved before it, this would not be necessary... re-structuring the savegame file (one day) would be good.
}

int main(int argc, char** argv)
{
#ifdef BACKTRACE
  signal(SIGABRT, CrashHandler);
  signal(SIGBUS, CrashHandler);
  signal(SIGFPE, CrashHandler);
  signal(SIGILL, CrashHandler);
  signal(SIGINT, CrashHandler);
  signal(SIGSEGV, CrashHandler);
  signal(SIGSYS, CrashHandler);
  signal(SIGTERM, CrashHandler);
  signal(SIGTRAP, CrashHandler);
  signal(SIGQUIT, CrashHandler);
#endif

  GetUserDataDir(); //just to properly initialize as soon as possible DBGMSG correct path b4 everywhere it may be used.

  if(argc > 1 && festring(argv[1]) == "--version")
  {
    std::cout << "Iter Vehemens ad Necem version " << IVAN_VERSION << std::endl;
    return 0;
  }

  if(argc > 1 && festring(argv[1]) == "--defgen")
  {
    std::cout << "Generate defines validator file. " << std::endl;
    game::InitGlobalValueMap();
    std::cout << "DONE: InitGlobalValueMap()" << std::endl;
    definesvalidator::GenerateDefinesValidator("generate");
    std::cout << "Finished: Generate DefinesValidator" << std::endl;
    return 0;
  }

  if(argc > 1 && festring(argv[1]) == "--defval")
  {
    std::cout << "Validate defines. " << std::endl;
    game::InitGlobalValueMap();
    std::cout << "DONE: InitGlobalValueMap()" << std::endl;
    definesvalidator::GenerateDefinesValidator("validate");
    std::cout << "Finished: Validate defines" << std::endl;
    return 0;
  }

  if(argc > 1 && festring(argv[1]) == "--help")
  {
    std::cout << "Command line options:" << std::endl;
    std::cout << "--defgen Generate defines validator source file. " << std::endl;
    std::cout << "--defval Validate defines. " << std::endl;
    std::cout << "--version Show current game version. " << std::endl;
    std::cout << "--record [file] Record every key the game reads to file. " << std::endl;
    std::cout << "--replay [file] Play back a recording instead of reading real input. " << std::endl;
    std::cout << "--trace [file] Write the game's JSONL trace to file: one record per game step, with the turn, the clock, the player, the creation counters and the draw counts. Nothing in it comes from the screen. " << std::endl;
    std::cout << "--frame-trace [file] Write the presentation's JSONL trace to file: one record per frame whose pixels differ from the frame before, with a hash of the double buffer. " << std::endl;
    std::cout << "--seed [number] Pin the random number seed. Continuing a saved game ignores this, as the seed is stored in the save. " << std::endl;
    std::cout << "--visual-seed [number|random] Seed the presentation generator, which decides no game state. Fixed by default so frames reproduce; 'random' is the arm that checks nothing leaks from it. " << std::endl;
    std::cout << "--shot [file.png] Write the screen to a PNG when the run ends, plus a .txt sidecar holding every string on it. " << std::endl;
    std::cout << "--shot-dir [dir] Write every frame that differs from the one before it to dir/frame-NNNNNN.png. Uncompressed, about 1.4MB each and several hundred frames per session. " << std::endl;
    std::cout << "--text [file] Log every string the game draws, in draw order, one line per string. " << std::endl;
    std::cout << "--headless Run with no window and no audio device. The game still renders into the double buffer, so --trace and --shot work. " << std::endl;
    std::cout << std::endl;
    std::cout << "Environment Variables:" << std::endl;
    std::cout << "IVAN_SHOWFPS=[true] # show FPS at top right" << std::endl;
    std::cout << "IVAN_DebugShowTinyDungeon=[true] #DEBUG always show tiny dungeon above stretched one" << std::endl;
    std::cout << "IVAN_LISTDRAWABOVE=[true] #DEBUG output the draw above priority list to text console terminal" << std::endl;
    std::cout << "IVAN_DebugGenDungeonLevelLoopID=[DungeonLevelIndex] #DEBUG DungeonLevelIndex must be an integer matching a some dungeon level" << std::endl;
    std::cout << "IVAN_DebugGenDungeonLevelLoopMax=[integer] #DEBUG generate the dungeon level how many times" << std::endl;
#ifdef WIZARD    
    std::cout << "IVAN_DebugStayOnDungeonLevel=[DungeonLevelIndex] #DEBUG wizard auto play AI will not leave that Dungeon Level after entering it" << std::endl;
#endif
    return 0;
  }

  /* After the options that return immediately, so none of them creates a
     recording, and before the seeding below and the first frame drawn by
     igraph::Init(). */

  harness::ParseArgs(argc, argv);

  audio::Init(game::GetMusicDir());

  culong Seed = harness::HasSeedOverride() ? harness::GetSeedOverride()
                                           : ulong(time(0));
  femath::SetSeed(Seed);

  /* fantasyname keeps a generator of its own, seeded from the wall clock when
     the library loads. It produces the player's default name, which reaches
     the screen and the save file name, so it has to be pinned to this seed
     too or no two runs agree. */

  NameGen::SetSeed(Seed);

  /* Deliberately not Seed. The presentation generator is a separate stream and
     seeding it from the game's would put the game's seed back in front of every
     visual draw, which is half of what docs/port-log.md §6.10 was about. */

  visualrand::SetSeed(harness::GetVisualSeed());

  game::InitGlobalValueMap();
  scriptsystem::Initialize();
  databasesystem::Initialize();
  game::InitLuxTable();
  ivanconfig::Initialize();
  igraph::Init();
  game::CreateBusyAnimationCache();
  globalwindowhandler::SetQuitMessageHandler(game::HandleQuitMessage);
  globalwindowhandler::SetScrshotDirectory(game::GetScrshotDir());
  specialkeys::init();
  bugfixdp::init();
  devcons::Init();
  definesvalidator::init();
  msgsystem::Init();
  protosystem::Initialize();
  igraph::LoadMenu();
  game::PrepareStretchRegionsLazy();

  /* Set off the main menu music */
  audio::SetPlaybackStatus(0);
  audio::ClearMIDIPlaylist();
  audio::LoadMIDIFile("mainmenu.mid", 0, 100);
  audio::SetPlaybackStatus(audio::PLAYING);

  for(int running = 1; running;)
  {
    int Select = iosystem::Menu(igraph::GetMenuGraphic(),
                                v2(RES.X / 2, RES.Y / 2 - 20),
                                CONST_S("\r"),
                                CONST_S("Start Game\r"
                                        "Continue Game\r"
                                        "Configuration\r"
                                        "Highscores\r"
                                        "Quit\r"),
                                LIGHT_GRAY,
                                CONST_S("Released under the GNU\r"
                                        "General Public License\r"
                                        "More info: see COPYING\r"),
                                CONST_S("IVAN v" IVAN_VERSION "\r"),
                                ivanconfig::GetExtraMenuGraphics());

    switch(Select)
    {
     case 0:
      igraph::AddOutlinesIfNeeded();
      if(game::Init())
      {
        igraph::UnLoadMenu();

        game::Run();
        game::DeInit();
        igraph::LoadMenu();
      }

      break;
     case 1:
      {
        iosystem::SetSkipSeekSave(&SkipGameScript);
        festring LoadName = iosystem::ContinueMenu(WHITE, LIGHT_GRAY, game::GetSaveDir(), game::GetSaveFileVersionHardcoded(), ivanconfig::IsAllowImportOldSavegame());

        if(LoadName.GetSize())
        {
          LoadName.Resize(LoadName.GetSize() - 4); // - ".sav"

          igraph::AddOutlinesIfNeeded();
          if(game::Init(LoadName))
          {
            igraph::UnLoadMenu();
            game::Run();
            game::DeInit();
            igraph::LoadMenu();
          }
        }

        break;
      }
     case 2:
      ivanconfig::Show();
      break;
     case 3:
      {
        highscore HScore(GetUserDataDir() + HIGH_SCORE_FILENAME);
        HScore.Draw();
        break;
      }
     case 4:
      running = 0;
      break;
    }
  }

  msgsystem::DeInit();
  harness::Shutdown();

  return 0;
}
