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
#include <memory>
#include <utility>

#include <cstdarg>
#include <cctype>
#include <regex>

#include <ctype.h>

#include "festring.h"
#include "felist.h"
#include "graphics.h"
#include "save.h"
#include "bitmap.h"
#include "sfx.h"
#include "feio.h"
#include "harness.h"
#include "dbgmsgproj.h"

/* SOUND SYSTEM */

#ifndef NOSOUND

int soundeffects::SoundState = 0;
std::vector<SoundFile> soundeffects::files;
std::vector<SoundInfo> soundeffects::patterns;
bool soundeffects::bIsEnabled=false;
long soundeffects::lSfxVol=0;
festring soundeffects::fsDataDir="";

struct SoundFile
{
  festring filename;
  std::unique_ptr<Mix_Chunk*> chunk = std::unique_ptr<Mix_Chunk*>(new (Mix_Chunk*)(NULL));
  //Mix_Music *music;

  SoundFile() = default;
  SoundFile(SoundFile&) = delete;
  SoundFile(SoundFile&&) = default;
  ~SoundFile()
  {
    if(chunk.get() && *chunk) Mix_FreeChunk(*chunk);
  }
};

struct SoundInfo
{
  std::vector<int> sounds;
  std::regex re;
  bool bValid = false; // a default-constructed re is never matched against

  SoundInfo() = default;
  SoundInfo(SoundInfo&) = delete;
  SoundInfo(SoundInfo&&) = default;
  ~SoundInfo() = default;
};

bool eol = false;
festring getstr(FILE *f, truth word)
{
  if(eol && word) return CONST_S("");
  festring s;
  while(1)
  {
    char c = fgetc(f);
    if(c == EOF) return s;
    if(c == 13) continue;
    if(c == 10 && s != "") return eol = true, s;
    if(c == 10) continue;
    if(c == ' ' && word && s != "") return s;
    s = s + c;
  }
}

FILE *debf = NULL;
void soundeffects::initSound()
{
  if(SoundState == 0)
  {
    /* --headless: an audio device is the other half of what a display is, and
       under Emscripten it is the same kind of problem - the port's audio
       backend wants an AudioContext, which node does not have. Failing to open
       one is not harmless here either: the failure path below calls
       iosystem::AlertConfirmMsg, which draws a dialog and then blocks on
       GET_KEY, and during a replay that eats a recorded key and desynchronises
       the run. So headless declines to open a device rather than trying and
       handling the failure. -1 is the state initSound already uses for
       "sound unavailable, do not retry". */

    if(harness::IsHeadless())
    {
      SoundState = -1;
      return;
    }

    festring fsSndDbgFile = GetUserDataDir() + "SndDebug.txt";
    debf = fopen(fsSndDbgFile.CStr(), "wt"); //"a");
    if(debf)fprintf(debf, "This file can be used to diagnose problems with sound.\n");

    if(Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 8000) != 0)
    {
      std::vector<festring> vfsCritMsgs;
      vfsCritMsgs.push_back(festring()<<Mix_GetError());
      bool bDummy = iosystem::AlertConfirmMsg("Unable to initialize audio:",vfsCritMsgs,false);
      if(debf)fprintf(debf, "Unable to initialize audio: %s\n", Mix_GetError());
      SoundState = -1;
      return;
    }
    Mix_AllocateChannels(16);
    SoundState = -2;

    /**
     * The last matching pattern will win (if more than one matches).
     * Sound files are chosen randomly (if there is more than one).
     */

    // new config file (new syntax)
    festring cfgfileNew = fsDataDir + "Sound/SoundEffects.cfg";
    FILE *fNew = fopen(cfgfileNew.CStr(), "rt");

    if(!fNew){
      SoundState = -1;
//      ABORT("Failed to open SFX cfg file: %s",cfgfileNew.CStr());
    }

    truth bDbg=false; //TODO global command line for debug messages
#ifdef DBGMSG
    bDbg=true;
#endif
    if(bDbg)std::cout << "Sound Effects (new) config file setup:" << std::endl;
    if(fNew)
    {
      festring Line;
      while((Line = getstr(fNew, false)) != "")
      {
        cchar* c = Line.CStr();
        if(Line.IsEmpty())continue; //empty lines will be ignored (good for formating, easy readability)
        if(c[0]=='#')continue; // lines beggining with '#' will be skipped, ignored like a comment

        if(bDbg)std::cout << "LINE:'" << Line.CStr() <<"'"<< std::endl;

        SoundInfo si;
        int iPart=0;
        festring Pattern, AllFiles, Description, TmpPart;
        for(int i=0;i<Line.GetSize();i++)
        {
          if( c[i]!=';' || i==(Line.GetSize()-1) ) // skip separator and add last char
          {
            TmpPart = TmpPart + c[i];
          }

          if( c[i]==';' || i==(Line.GetSize()-1) ) // prepare part if it is the separator or the last char
          {
            switch(iPart){
            case 0: // description, good for sorting in groups by similarity (like everything about 'door: ')
              Description = TmpPart;
              if(bDbg)std::cout << "Desc:'" << Description.CStr() <<"'"<< std::endl;
              break;
            case 1: // files
              AllFiles = TmpPart;
              if(bDbg)std::cout << "Fles:'" << AllFiles.CStr() <<"'"<< std::endl;
              break;
            case 2: // regex
              Pattern = TmpPart;
              if(bDbg)std::cout << "Ptrn:'" << Pattern.CStr() <<"'"<< std::endl;
              break;
            }
            TmpPart.Empty(); //reset
            iPart++;
          }
        }

        // configure the regex
        try
        {
          si.re.assign(Pattern.CStr(), Pattern.GetSize(), std::regex::ECMAScript);
          si.bValid = true;
        }
        catch(const std::regex_error& e)
        {
          si.bValid = false;
          if(debf) fprintf(debf, "regex compilation failed for '%s': %s\n", Pattern.CStr(), e.what());
        }

        // configure the assigned files, now they are separated with ',' and the filename now accepts spaces.
        festring FileName;
        truth bFoundDot=false;
        for(int i=0;i<AllFiles.GetSize();i++)
        {
          if( FileName.IsEmpty() && AllFiles[i] == ' ' )continue; //skip spaces from start TODO remove trailing spaces for each file
          if( bFoundDot && AllFiles[i] == ' ' )continue; //skip spaces from end TODO this "after dot" trick will prevent more than one dot per file :/

          if( AllFiles[i]!=',' || i==(AllFiles.GetSize()-1) ) // skip separator and add last char
          {
            FileName << AllFiles[i];
          }

          if(AllFiles[i]=='.')bFoundDot=true;

          if( AllFiles[i]==',' || i==(AllFiles.GetSize()-1) ) // prepare part if it is the separator or the last char
          {
            si.sounds.push_back(addFile(FileName));
            if(bDbg)std::cout <<"'"<<FileName.CStr()<<"'"<< " - " <<"'"<<Pattern.CStr()<<"'"<< std::endl;

            //reset
            FileName.Empty();
            bFoundDot=false;
          }
        }
        if(bDbg)std::cout << "SInfoSize=" << si.sounds.size() << std::endl;

        if(si.sounds.size() != 0) patterns.push_back(std::move(si));
        if(bDbg)std::cout << "PtrnSize=" << patterns.data()->sounds.size() << std::endl;
      }

      fclose(fNew);
      SoundState = 1;
    }

    //Mix_HookMusicFinished(changeMusic); // will this music type be used again one day? TODO may be as environment/ambient sounds!

    if(debf) fclose(debf);
    
    DBG1(SoundState);
  }
}

void soundeffects::deInitSound()
{
  Mix_AllocateChannels(0);

  int freq, chans;
  Uint16 fmt;
  for(int times = Mix_QuerySpec(&freq, &fmt, &chans); times >= 0; times--)
    Mix_CloseAudio();

  while(Mix_Init(0))
    Mix_Quit();

  SoundState = 2;
}

int soundeffects::addFile(festring filename) {
  for(int i=0; i < int(files.size()); i++)
    if(files[i].filename == filename) return i;
  SoundFile p;
  p.filename = filename;
  *p.chunk = NULL;
  //p.music = NULL;
  files.push_back(std::move(p));
  DBG2(files.size(),filename.CStr());
  return files.size() - 1;
}

/* Private on purpose. rand() is one process wide stream, and this is reached
   only when the player has sound effects configured, so drawing from a shared
   generator would let that setting shift every later draw. femath::Rand is
   wrong for the same reason: which sounds exist must not change the game. */

static ulong NextSoundRand()
{
  static ulong State = 88675123UL;

  State ^= State << 13;
  State ^= State >> 17;
  State ^= State << 5;
  return State;
}

SoundFile* soundeffects::findMatchingSound(festring Buffer)
{
  if(Buffer.IsEmpty() || Buffer.CStr()[0]=='"') //skips all chat messages lowering config file regex complexity
    return NULL;

  DBG1(Buffer.CStr());
  for(int i = patterns.size() - 1; i >= 0; i--){
    if(patterns[i].bValid)
      if(std::regex_search(Buffer.CStr(), Buffer.CStr() + Buffer.GetSize(), patterns[i].re)){
        SoundFile* p = &files[patterns[i].sounds[NextSoundRand() % patterns[i].sounds.size()]];
        DBG1(p->filename.CStr());
        return p;
      }
  }
  return NULL;
}

void soundeffects::playSound(festring Buffer)
{
  DBG3(bIsEnabled,lSfxVol,Buffer.CStr());
  
  if(!bIsEnabled)
    return;
  if(lSfxVol==0)
    return;
  
  if(fsDataDir.IsEmpty())ABORT("Data dir not set for SFX");
  
  initSound();
  DBG1(SoundState);
  if(SoundState == 1)
  {
    SoundFile *sf = findMatchingSound(Buffer);
    if(!sf) return;

    if(!*sf->chunk)
    {
      festring sndfile = fsDataDir + "Sound/" + sf->filename;
      *sf->chunk = Mix_LoadWAV(sndfile.CStr());
    }

    if(*sf->chunk)
    {
      for(int iChannel=0; iChannel<16; iChannel++)
      {
        if(!Mix_Playing(iChannel))
        {
          Mix_Volume(iChannel, lSfxVol);
          Mix_PlayChannel(iChannel, *sf->chunk, 0);
          DBG3(iChannel,lSfxVol,sf->filename.CStr());
          //TODO why this causes SEGFAULT?!! -> //DBGEXEC( fprintf(debf, "Mix_PlayChannel(%d, \"%s\", 0);\n", iChannel, sf->filename.CStr()) );
          //TODO? Mix_SetPosition(i, angle, dist);
          return;
        }
      }
    }
  }
}

#endif
