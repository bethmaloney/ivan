/*
    audio.cpp : MIDI Audio Implementation for IVAN
    Copyright (c) 2004-2016 Adrian M. Gin

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License v2 as published by
    the Free Software Foundation;

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

    The above copyright notice and this permission notice shall be
    included in all copies or substantial portions of the Software.

*/


#ifdef USE_SDL
#include "SDL.h"
#include "SDL_thread.h"
#include "SDL_timer.h"


#endif

#include "audio.h"
#include <iostream>
#include <cstdio>
#include <cstring>
#include "message.h"
#include "game.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include "midiplayback.h"
#endif


musicfile::musicfile(cfestring& Filename, int LowThreshold, int HighThreshold)
:  Filename(Filename), LowThreshold(LowThreshold), HighThreshold(HighThreshold)
{
   isPlaying = false;
}

#ifndef __EMSCRIPTEN__

/* Track picking deliberately does not use rand(): that is one process wide
   stream, and audio::Loop below runs on its own thread, so every draw made
   here would shift the draws made by the rest of the program by an amount
   decided by the scheduler. It must not use femath::Rand either, or which
   music the player has configured would change the game. This generator is
   touched only by the audio thread and by nothing else. */

static ulong NextTrackRand()
{
   static ulong State = 2463534242UL;

   State ^= State << 13;
   State ^= State >> 17;
   State ^= State << 5;
   return State;
}

#endif

int audio::MasterVolume;
int audio::TargetIntensity;
int audio::CurrentIntensity;
bool audio::isInit;

int audio::PlaybackState;
volatile bool audio::isTrackPlaying;

bool audio::volumeChangeRequest;

int audio::CurrentPosition;
int audio::CurrentMIDIOutPort;

std::vector<musicfile> audio::Tracks;

festring audio::CurrentTrack;
festring audio::MusDir;

#ifndef __EMSCRIPTEN__

RtMidiOut* audio::midiout = 0;

/** For each increase in intensity, the respective MIDI channel changes by the following amount */
int  audio::DeltaVolumePerIntensity[MAX_MIDI_CHANNELS] = {0, 0, 0, 0, 0, -1, -1, -1, -1, 0, -1, 1, 1, 1, 1, 1};
int  audio::IntensityVolume[MAX_MIDI_CHANNELS];

int  audio::InitialIntensityVolume[MAX_MIDI_CHANNELS] = {MAX_INTENSITY_VOLUME, MAX_INTENSITY_VOLUME, MAX_INTENSITY_VOLUME, MAX_INTENSITY_VOLUME, MAX_INTENSITY_VOLUME,
                                                         MAX_INTENSITY_VOLUME, MAX_INTENSITY_VOLUME, MAX_INTENSITY_VOLUME, MAX_INTENSITY_VOLUME, MAX_INTENSITY_VOLUME,
                                                         MAX_INTENSITY_VOLUME, 0, 0, 0, 0, 0};


void audio::error(RtMidiError::Type type, const std::string &errorText, void *userData )
{

}

#else

/* The browser plays the music; this decides what it should be (HARNESS.md
   §9.8). The split is the one sfx.cpp already makes for effects, and the same
   reasoning puts the cut in the same place: the playlist is built from the
   level scripts by dungeon::PrepareMusic and belongs to the game, while
   fetching, decoding, looping and mixing belong to the page.

   Fire and forget in both directions but one. Nothing here waits on the page,
   so asyncify has nothing to unwind and a slow fetch cannot stall a turn, and
   everything that can fail -- a missing render, a decode error, a context the
   autoplay policy still holds suspended -- fails on the JS side and stays
   quiet, which is what the RtMidi path does when no device will open.

   The exception is IvanMusicCurrentIndex, which has to answer *now* because
   dungeon::PrepareMusic branches on it. It is a plain synchronous EM_JS
   returning an int: no promise, no callback into wasm, nothing for asyncify.
   An index into the playlist rather than a string keeps the ownership of that
   memory on this side, where it already is. */

EM_JS(void, IvanMusicPlaylist, (const char* Names), {
  var Music = globalThis.ivanMusic;

  if(!Music)
    return;

  Music.setPlaylist(UTF8ToString(Names));
});

EM_JS(void, IvanMusicPlaying, (int State), {
  var Music = globalThis.ivanMusic;

  if(!Music)
    return;

  Music.setPlaying(!!State);
});

EM_JS(void, IvanMusicVolume, (int Level), {
  var Music = globalThis.ivanMusic;

  if(!Music)
    return;

  Music.setVolume(Level);
});

EM_JS(void, IvanMusicIntensity, (int Level), {
  var Music = globalThis.ivanMusic;

  if(!Music)
    return;

  Music.setIntensity(Level);
});

EM_JS(int, IvanMusicCurrentIndex, (), {
  var Music = globalThis.ivanMusic;

  if(!Music)
    return -1;

  return Music.currentIndex();
});

/* Pushed whole on every change rather than as add/remove deltas, because the
   list is at most a handful of names and a single authoritative copy cannot
   fall out of step with this one. dungeon::PrepareMusic mutates it several
   times per level change; music.js compares against what it already has and
   does nothing when they match, so the repeats cost nothing and the music does
   not restart. */

static void PushPlaylist(const std::vector<musicfile>& Tracks)
{
   festring Names;

   for(uint c = 0; c < Tracks.size(); ++c)
   {
      if(c)
         Names << ',';

      Names << Tracks[c].GetFilename();
   }

   IvanMusicPlaylist(Names.CStr());
}

#endif

void audio::Init(cfestring& musicDirectory)
{
   int audio_rate, audio_channels;
   unsigned short audio_format;
   int bits;

   int nPorts;
   std::string portName;
   std::vector<unsigned char> message;

   PlaybackState = audio::RESUME_SONG;
   CurrentMIDIOutPort = -1;
   CurrentIntensity = 0;
   MasterVolume = 0;
   TargetIntensity = 0;
   volumeChangeRequest = false;
   CurrentTrack.Empty();
   MusDir = musicDirectory;

#ifdef __EMSCRIPTEN__

   /* No device to open and no thread to run it. The page creates its own
      AudioContext on the first gesture, and what audio::Loop existed to do --
      wait for a track to end, then choose the next -- is an 'ended' listener
      there. Emscripten has no pthread here anyway: SDL_CreateThread would
      return null and the loop would simply never run, which is what has kept
      the browser build silent until now (§9.3).

      isInit is still set, because everything below reads it as "the playlist
      may be used". atexit stays too: EXIT_RUNTIME is on so main returning does
      run it, and DeInit stopping the music is worth having on the way out. */

   PlaybackState = 0x00;
   isInit = true;
   isTrackPlaying = false;
   PushPlaylist(Tracks);
   atexit(audio::DeInit);
   return;

#else

   // RtMidiOut constructor
   try
   {
      midiout = new RtMidiOut();
   } catch (RtMidiError &error)
   {
      error.printMessage();

      /* No usable MIDI backend (no sequencer, headless session, CI runner...).
         Continue without music rather than refusing to start. Every entry point
         below is guarded by isInit, so the rest of the game is unaffected. */
      midiout = 0;
      isInit = false;
      isTrackPlaying = false;
      std::cerr << "MIDI unavailable, continuing without music." << std::endl;
      return;
   }


   midiout->setErrorCallback(audio::error);

   //LoadMIDIFile("Track1.mid", 0, 100);
   //LoadMIDIFile("Track2.mid", 0, 100);



   SDL_Thread *thread;
   int         threadReturnValue;

   // Simply create a thread
   thread = SDL_CreateThread( audio::Loop, "AudioThread", (void *)NULL);

   PlaybackState = 0x00;

   isInit = true;
   isTrackPlaying = false;
   atexit(audio::DeInit);

#endif

}

void audio::DeInit(void)
{
   SetPlaybackStatus(STOPPED);
   isInit = false;

#ifndef __EMSCRIPTEN__
   if( midiout )
   {
      delete midiout;
   }
#endif

   Tracks.erase(Tracks.begin(), Tracks.end());

#ifdef __EMSCRIPTEN__
   PushPlaylist(Tracks);
#endif

}

#ifndef __EMSCRIPTEN__

int audio::Loop(void *ptr)
{

   std::vector<unsigned char> message;
   // Note On: 144, 64, 90

   while(1)
   {
      if( Tracks.size() && (PlaybackState & PLAYING) )
      {
         isTrackPlaying = true;
         int randomIndex = NextTrackRand() % Tracks.size();
         CurrentTrack = Tracks[randomIndex].GetFilename();

         festring MusFile = MusDir + CurrentTrack;

         PlayMIDIFile(MusFile, 1);
      }
      isTrackPlaying = false;
      SDL_Delay(1);
   }
   return 0;
}

#endif


cfestring& audio::GetCurrentlyPlayedFile()
{
#ifdef __EMSCRIPTEN__

   /* The page chose the track, so this is a readback rather than a field.
      dungeon::PrepareMusic calls it on every level change to decide whether
      the new area shares the track already playing, and keeps it running if so
      (dungeon.cpp:174) -- so an answer that is merely plausible would restart
      the music at every staircase.

      Resolved through the playlist by index: music.js holds the name it picked
      and looks it up in the list this side last pushed, which stays right
      across the reorder ClearMIDIPlaylist performs. Out of range means nothing
      is playing, and an empty string is what PrepareMusic already handles. */

   int Index = IvanMusicCurrentIndex();

   if(Index >= 0 && Index < int(Tracks.size()))
      CurrentTrack = Tracks[Index].GetFilename();
   else
      CurrentTrack.Empty();

#endif

   return CurrentTrack;
}


/*int audio::GetCurrentOutputDevice(void)
{
   return midiout->isPortOpen();
}*/

/* Port0 is NULL, and disabled. */
int audio::ChangeMIDIOutputDevice(int newPort)
{
   if( !isInit )
      return 0;

#ifdef __EMSCRIPTEN__

   /* One pseudo-device, so port 1 is the page and port 0 is still "no". The
      same two states the native path has, without a port to open. */

   if( newPort != CurrentMIDIOutPort )
   {
      CurrentMIDIOutPort = newPort;
      SetPlaybackStatus(newPort ? (audio::PLAYING | audio::RESUME_SONG) : 0x00);
   }

   return 0;

#else

   if( newPort != CurrentMIDIOutPort)
   {
      audio::SetPlaybackStatus(0x00);
      try {
         if( midiout->isPortOpen() )
         {
            midiout->closePort();
         }

         if( newPort != 0)
         {
            midiout->openPort(newPort-1);
            audio::SetPlaybackStatus(audio::PLAYING | audio::RESUME_SONG);
         }
         CurrentMIDIOutPort = newPort;

      }
      catch (RtMidiError &error) {
        error.printMessage();

        /* Losing the output port is not worth killing the game over. */
        CurrentMIDIOutPort = 0;
        audio::SetPlaybackStatus(0x00);
      }
   }

   return 0;

#endif
}

int audio::GetMIDIOutputDevices(std::vector<std::string>& deviceNames)
{
   if( !isInit )
      return 0;

#ifdef __EMSCRIPTEN__

   /* Named for what it is, because this string is what the options menu shows
      against "Use MIDI soundtrack" (iconf.cpp:600). Reporting one device is
      also what turns the soundtrack on by default here: Initialize enables it
      whenever the count is non-zero (iconf.cpp:1292). */

   deviceNames.push_back("Web Audio");

   return int(deviceNames.size());

#else

   int nPorts = midiout->getPortCount();
   std::string portName;

   for ( unsigned int i=0; i<nPorts; i++ ) {
     try {
       portName = midiout->getPortName(i);

     }
     catch (RtMidiError &error) {
       error.printMessage();
       continue;
     }
     deviceNames.push_back(portName);
   }

   return int(deviceNames.size());

#endif
}


#ifndef __EMSCRIPTEN__

void audio::SendVolumeMessage(int targetVolume)
{
   MIDI_CHAN_EVENT_t newVolume;


   for(int i = 0 ; i < MAX_MIDI_CHANNELS; ++i )
   {
      newVolume.eventType = MIDI_CONTROL_CHANGE | i;
      newVolume.parameter1 = CHANNEL_VOLUME;

      int midivolume  = (IntensityVolume[i] * targetVolume) / MAX_MASTER_VOLUME;
      if( midivolume >= MAX_MASTER_VOLUME )
      {
         midivolume = MAX_MASTER_VOLUME;
      }

      if( midivolume <= 0 )
      {
         midivolume = 0;
      }
      newVolume.parameter2 = midivolume;


      ::SendMIDIEvent(&newVolume);
   }

}


int audio::PlayMIDIFile(cfestring& filename, int32_t loops)
{
   if( !isInit )
      return 0;

   std::vector<unsigned char> message;
   MIDI_HEADER_CHUNK_t MIDIHdr;


   MPB_PlayMIDIFile(&MIDIHdr, const_cast<char*>(filename.CStr()));

   int usPerTick = MPB_SetTickRate(MIDIHdr.currentState.BPM, MIDIHdr.PPQ);
   int cumulativeWait = 0;
   int position = CurrentPosition;

   int volumeChangeDelay = US_PER_VOLUME_CHANGE;


   if( !(PlaybackState & RESUME_SONG) )
   {
      position = 0;
   }

   for (int32_t i = 0; i < loops; i++)
   {
      MPB_RePosition(&MIDIHdr, position, MPB_PB_NO_VOL);
      for(;;)
      {
         cumulativeWait += usPerTick;
         volumeChangeDelay -= cumulativeWait;

         if( cumulativeWait >= 1000 )
         {
            SDL_Delay(cumulativeWait / 1000);
            cumulativeWait = cumulativeWait - ((cumulativeWait / 1000)*1000);
         }

         if( (volumeChangeDelay < 0) || volumeChangeRequest)
         {
            volumeChangeDelay = US_PER_VOLUME_CHANGE;
            if( (CurrentIntensity != TargetIntensity) || volumeChangeRequest )
            {
               if( TargetIntensity > CurrentIntensity )
               {
                  CurrentIntensity++;
               }

               if( CurrentIntensity > TargetIntensity )
               {
                  CurrentIntensity--;
               }
               CalculateChannelVolumes(CurrentIntensity, &DeltaVolumePerIntensity[0]);
               SendVolumeMessage(MasterVolume);

               if( volumeChangeRequest )
               {
                  volumeChangeRequest = false;
               }

            }
         }

         if( PlaybackState & PLAYING )
         {
            MIDIHdr.masterClock += 1;
            CurrentPosition = MIDIHdr.masterClock;
         }
         else
         {
            MPB_PausePlayback(&MIDIHdr);
            MPB_ResetMIDI();
            MPB_CloseFile();
            return 0;
         }


         if (MPB_ContinuePlay(&MIDIHdr, MPB_PB_NO_VOL) == MPB_FILE_FINISHED)
         {
            MPB_PausePlayback(&MIDIHdr);
            break;
         }
      }

      //Reset playback pointer
      position = 0;
      CurrentPosition = 0;
   }

   MPB_CloseFile();

   return 0;
}

#endif


void audio::SetVolumeLevel(int vol)
{
   MasterVolume = vol;
   volumeChangeRequest = true;

#ifdef __EMSCRIPTEN__
   IvanMusicVolume(vol);
#endif
}

int audio::GetVolumeLevel(void)
{
   return MasterVolume;
}

void audio::IntensityLevel(int intensity)
{
   if( intensity != TargetIntensity )
   {
      TargetIntensity = intensity;

#ifdef __EMSCRIPTEN__
      /* character::Be recomputes this every turn from the player's worst body
         part (char.cpp:1062), so it arrives often and mostly unchanged. Only
         the changes are worth a call across the boundary, and music.js ramps
         to the new value over the same 15ms-per-step the native mixer takes
         (US_PER_VOLUME_CHANGE) rather than jumping. */

      IvanMusicIntensity(intensity);
#endif
   }
   /* Do a check to see if we change / cue MIDI file */
}


void audio::SetPlaybackStatus(uint8_t newStateBitmap)
{
   PlaybackState = newStateBitmap;

#ifdef __EMSCRIPTEN__

   /* No thread, so nothing to wait for -- and waiting is the one thing this
      must not do here. isTrackPlaying is only ever cleared by audio::Loop,
      which does not run on this target, so the spin below would be an
      unbreakable loop on the single thread the whole page shares if anything
      ever set it. */

   IvanMusicPlaying(PlaybackState & PLAYING);
   return;

#else

   if( !(PlaybackState & PLAYING))
   {
      //Wait until the track has finished playing
      while(isTrackPlaying)
      {

      }
   }

#endif
}


/* Both erasers take the iterator vector::erase hands back rather than reusing
   the one it invalidated. The old form is undefined either way, and
   RemoveMIDIFile's -- erase and then ++it -- also skipped the element after
   every match and could step past end(). It survived because nothing calls it
   and because ClearMIDIPlaylist's misuse happens to do the right thing on a
   vector, which is the kind of luck that stops holding the moment anything
   else changes. */

void audio::ClearMIDIPlaylist(cfestring& exceptFilename)
{
   for(auto it = Tracks.begin(); it != Tracks.end();)
   {
      if(!exceptFilename.IsEmpty() && it->GetFilename() == exceptFilename)
      {
         ++it;
      }
      else
      {
         it = Tracks.erase(it);
      }
   }

#ifdef __EMSCRIPTEN__
   PushPlaylist(Tracks);
#endif
}

void audio::RemoveMIDIFile(cfestring& filename)
{
   for(auto it = Tracks.begin(); it != Tracks.end();)
   {
      if(it->GetFilename() == filename)
         it = Tracks.erase(it);
      else
         ++it;
   }

#ifdef __EMSCRIPTEN__
   PushPlaylist(Tracks);
#endif
}

void audio::LoadMIDIFile(cfestring& filename, int intensitylow, int intensityhigh)
{
  Tracks.push_back(musicfile(filename, intensitylow, intensityhigh));

#ifdef __EMSCRIPTEN__
  PushPlaylist(Tracks);
#endif
}

#ifndef __EMSCRIPTEN__

void audio::SendMIDIEvent(std::vector<unsigned char>* message)
{
   if( !isInit )
      return;

   midiout->sendMessage( message );
}


void audio::CalculateChannelVolumes(int intensity, int* deltaIntensity)
{
   for(int i = 0 ; i < MAX_MIDI_CHANNELS; ++i)
   {
      IntensityVolume[i] = InitialIntensityVolume[i] + (deltaIntensity[i] * intensity);
      if(IntensityVolume[i] >= MAX_INTENSITY_VOLUME)
      {
         IntensityVolume[i]  = MAX_INTENSITY_VOLUME;
      }

      if( IntensityVolume[i] <= 0 )
      {
         IntensityVolume[i] = 0;
      }


   }



}



void SendMIDIEvent(MIDI_CHAN_EVENT_t* event)
{
   std::vector<unsigned char> message;
   message.push_back(event->eventType);
   message.push_back(event->parameter1);
   if( ((event->eventType & MIDI_MSG_TYPE_MASK) != MIDI_PROGRAM_CHANGE) && ((event->eventType & MIDI_MSG_TYPE_MASK) != MIDI_CHANNEL_PRESSURE))
   {
     message.push_back(event->parameter2);
   }
   audio::SendMIDIEvent( &message );
}

#endif
