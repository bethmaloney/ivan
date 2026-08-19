if(EMSCRIPTEN)
  # Nothing on this target synthesizes MIDI: the page plays pre-rendered stems
  # instead (docs/port-log.md §9.8), so RtMidi, the MIDI parser, the playback engine
  # and their helpers are not compiled at all rather than compiled and unused.
  #
  # That is about 4,700 lines, and none of it could have worked here. RtMidi
  # auto-selects its dummy backend when no API macro is set (RtMidi.h:571) and
  # answers "no devices" to everything, and the playback engine is driven by
  # audio::Loop on an SDL thread that Emscripten never creates. What is left is
  # audio.cpp, whose remaining job -- the playlist, the playback state, the
  # volume and the intensity -- is the part the game actually calls.
  set(FEAUDIO_SOURCES audio.cpp audio.h)
else()
  file(GLOB FEAUDIO_SOURCES *.h *.cpp)
endif()

add_library(FeAudio ${FEAUDIO_SOURCES})
target_link_libraries(FeAudio FeLib)

# Add RtMidi dependencies on Windows
if(WIN32)
  add_definitions(-D__WINDOWS_MM__)
  set(RTMIDI_DEPENDENCIES winmm)
endif()

# Add RtMidi dependencies on Linux
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  add_definitions(-D__LINUX_ALSA__)
  find_package(ALSA)
  set(RTMIDI_DEPENDENCIES ${ALSA_LIBRARY} pthread)
endif()

# Add RtMidi dependencies on Mac
if(APPLE)
  add_definitions(-D__MACOSX_CORE__)
  find_library(COREMIDI_LIBRARY CoreMIDI)
  find_library(COREAUDIO_LIBRARY CoreAudio)
  find_library(COREFOUNDATION_LIBRARY CoreFoundation)
  set(RTMIDI_DEPENDENCIES ${COREMIDI_LIBRARY} ${COREAUDIO_LIBRARY} ${COREFOUNDATION_LIBRARY})
endif(APPLE)

target_include_directories(FeAudio PUBLIC ../FeLib/Include ../Main/Include)
target_link_libraries(FeAudio ${RTMIDI_DEPENDENCIES})

if(NOT EMSCRIPTEN)
  find_package(SDL2 REQUIRED)
endif()

target_include_directories(FeAudio SYSTEM PUBLIC ${SDL2_mixer_INCLUDE_DIR})
target_link_libraries(FeAudio ${SDL2_mixer_LIBRARY})
