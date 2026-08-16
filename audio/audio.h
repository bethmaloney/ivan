/*
    audio.h : MIDI Audio Implementation for IVAN
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
#ifndef __AUDIO_H__
#define __AUDIO_H__


#ifdef USE_SDL
#include "SDL.h"
#endif

#include "error.h"
#include "festring.h"
#include <vector>

/* On Emscripten the page plays the music and nothing here synthesizes it
   (HARNESS.md §9.8), so the MIDI machinery is not merely unused -- it is not
   compiled at all. RtMidi cannot work on this target in any case: it would
   auto-select its dummy backend, which is 2,800 lines that answer "no devices"
   to everything.

   What that leaves behind is exactly the part the game actually calls: the
   playlist, the playback state, the volume and the intensity. Everything that
   needs a MIDI type or the audio thread is guarded out below, and none of it
   appears in main.cpp, dungeon.cpp, iconf.cpp, game.cpp or char.cpp. */

#ifndef __EMSCRIPTEN__
#include "RtMidi.h"
#endif

class musicfile
{
public:
   musicfile(cfestring& Filename, int LowThreshold, int HighThreshold);

   inline bool IsPlaying(void) { return isPlaying; }
   inline void SetPlayState(bool state) { isPlaying = state;}
   inline cfestring& GetFilename() const { return Filename; }

private:
   festring Filename;
   int LowThreshold;
   int HighThreshold;
   bool isPlaying;
};



class audio
{
public:

   /** Bitmap for different states*/
   typedef enum
   {
      STOPPED       = 0x00,
      PLAYING       = 0x01,
      RESUME_SONG  = 0x02,
   } eAudioPlaybackStates_t ;

   enum
   {
      MAX_MIDI_CHANNELS = 16
   };

   enum
   {
      MAX_MASTER_VOLUME  = 127,
      MAX_INTENSITY_VOLUME = 127
   };

   enum
   {
      US_PER_VOLUME_CHANGE  = 15000
   };

#ifndef __EMSCRIPTEN__
   static void error(RtMidiError::Type type, const std::string &errorText, void *userData );
#endif

   /**
    * @param musicDirectory path to the directory containing the MIDI files to load.
    */
   static void Init(cfestring& musicDirectory);
   static void DeInit(void);

#ifndef __EMSCRIPTEN__
   static int Loop(void *ptr);

   static int PlayMIDIFile(cfestring& filename, int32_t loops);

   static void SendMIDIEvent(std::vector<unsigned char>* message);
#endif

   /**
    * On Emscripten this reports one pseudo-device, because the page is the
    * only output there is. That is what keeps ivanconfig unchanged: it enables
    * the soundtrack when any device is present (iconf.cpp:1292) and names the
    * selected one in the options menu, and "no" stays available by cycling
    * past it exactly as it does natively.
    */
   static int GetMIDIOutputDevices(std::vector<std::string>& deviceNames);

   static int ChangeMIDIOutputDevice(int newPort);

   static cfestring& GetCurrentlyPlayedFile();

   /**
    * @param vol 0 - 128
    */
   static void SetVolumeLevel(int vol);

   static int GetVolumeLevel(void);

#ifndef __EMSCRIPTEN__
   static void SendVolumeMessage(int targetVolume);
#endif

   /**
    * @param intensity 0 - 100
    */
   static void IntensityLevel(int intensity);

   static void RemoveMIDIFile(cfestring& filename);

   /**
    * @param filename MIDI file location
    * @param intensitylow
    */
   static void LoadMIDIFile(cfestring& filename, int intensitylow, int intensityhigh);


   static void ClearMIDIPlaylist(cfestring& exceptFilename = CONST_S(""));

   static int IsPlaybackStopped(void);

   static void SetPlaybackStatus(uint8_t newStateBitmap);

#ifndef __EMSCRIPTEN__
   static void CalculateChannelVolumes(int intensity, int* deltaIntensity);
#endif

private:


   static bool isInit;
   static bool volumeChangeRequest;
   static int  MasterVolume; /** 0 - 127 */

#ifndef __EMSCRIPTEN__
   /* The three curves these two tables describe are the whole of the intensity
      system, and on Emscripten they have been applied already: they are what
      tools/music/split-stems.py cuts the rendered stems on, and what the three
      gain nodes in tools/web/music.js drive. Keeping a second copy here that
      nothing reads would be one more thing to drift. */

   static int  IntensityVolume[MAX_MIDI_CHANNELS];

   static int  InitialIntensityVolume[MAX_MIDI_CHANNELS];
   static int  DeltaVolumePerIntensity[MAX_MIDI_CHANNELS];
#endif

   static int  TargetIntensity;
   static int  CurrentIntensity;

   static volatile bool isTrackPlaying;

   static int CurrentPosition;

   static int  PlaybackState;
   static festring CurrentTrack;
   static festring MusDir;

   static std::vector<musicfile> Tracks;

#ifndef __EMSCRIPTEN__
   static RtMidiOut* midiout;
#endif

   static int CurrentMIDIOutPort;

};

#endif
