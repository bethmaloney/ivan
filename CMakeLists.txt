cmake_minimum_required(VERSION 3.10...4.0)
project(ivan CXX C)

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

set(VERSION_MAJOR 0)
set(VERSION_MINOR 59)
set(PROJECT_VERSION "${VERSION_MAJOR}.${VERSION_MINOR}")
#set(VERSION_PATCH 0)
#set(PROJECT_VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")

set(CMAKE_CXX_FLAGS "-std=c++11 ${CMAKE_CXX_FLAGS}")
add_definitions(-DIVAN_VERSION="${PROJECT_VERSION}" -DUSE_SDL)

option(BUILD_MAC_APP "Build standalone application for MacOS" OFF)
option(USE_HOME_FOR_STATE_DIR "Statedir will be /.ivan/ in current user's homedir" OFF)
option(WIZARD "Enable Wizard Mode" OFF)
option(PORTABLE_BUILD "Keep game and user data in the directory IVAN is launched from" OFF)

if(UNIX)
  add_definitions(-DUNIX)
  if(NOT PORTABLE_BUILD)
  include(GNUInstallDirs)

  if(BUILD_MAC_APP)
    install(DIRECTORY Graphics Script Music Sound DESTINATION "ivan")
    install(FILES COPYING INSTALL LICENSING NEWS MANUAL README.md DESTINATION "docs")
    add_definitions(-DMAC_APP)
  else()
    install(DIRECTORY Graphics Script Music Sound DESTINATION "${CMAKE_INSTALL_DATADIR}/ivan")
    add_definitions(-DDATADIR="${CMAKE_INSTALL_FULL_DATADIR}")

    message("\n*NOTICE*\n"
            "    Highscore files and Bone files are now stored at \"\$HOME/.ivan\".\n"
            "    Your HOME directory is \"$ENV{HOME}\".\n")
    if(IS_DIRECTORY "${CMAKE_INSTALL_FULL_LOCALSTATEDIR}/ivan")
      message(WARNING
              "If you have game data in \"${CMAKE_INSTALL_FULL_LOCALSTATEDIR}/ivan\","
              " please move them to \"$ENV{HOME}/.ivan\".")
    endif()
  endif()
  endif()

elseif(WIN32)
  install(DIRECTORY Graphics Script Music Sound DESTINATION "ivan")
  install(FILES COPYING INSTALL LICENSING NEWS MANUAL README.md DESTINATION "ivan")
  add_definitions(-DWIN32)

  if(MINGW)
    set(CMAKE_EXE_LINKER_FLAGS -mwindows)
  endif(MINGW)

  # This is bad. To remove below defines, FeLib should switch to std::chrono and serialize time since common epoch.
  # 32 bit TIME_T has year 2038 problem. Moreover, serialized value is not portable for time_t in general.
  # Switching to std::chrono should not be hard, but it's not a topic for this pull request.
  add_definitions(-D_USE_32BIT_TIME_T)
endif(UNIX)

# In visual studio 2017, add an option to hide the console window (useful where debugging is not intended - i.e. releases)
if(MSVC AND MSVC_HIDE_CONSOLE_WINDOW)
  # Hide the console window in visual studio projects
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
endif()

if(WIZARD)
  add_definitions(-DWIZARD)
endif()

if(PORTABLE_BUILD)
  add_definitions(-DPORTABLE_BUILD)
endif()

if(EMSCRIPTEN)
  # Declared in here rather than beside the other options so a native cmake -LH
  # does not offer knobs that do nothing on its target.
  #
  # WASM_BROWSER picks the host, and the two hosts are genuinely different builds
  # rather than one build with a flag: node gets the real filesystem and no page,
  # a browser gets MEMFS, the data directories baked into a .data file, and an
  # HTML shell holding the canvas SDL2 binds to. Default OFF because the thing
  # that runs unattended is the differential harness, and that one is node.
  option(WASM_BROWSER "Build for a browser: MEMFS, preloaded data and an HTML shell, rather than node" OFF)

  if(WASM_BROWSER)
    set(IVAN_WASM_NODERAWFS_DEFAULT OFF)
  else()
    set(IVAN_WASM_NODERAWFS_DEFAULT ON)
  endif()
  option(WASM_NODERAWFS "Use the real filesystem instead of MEMFS (node only, not browsers)" ${IVAN_WASM_NODERAWFS_DEFAULT})
  option(WASM_ASYNCIFY "Unwind the stack around blocking input so interactive play works" ON)

  # Music/ and Sound/ are ~28MB against Graphics/ and Script/'s ~3.5MB, and the
  # game runs without either: a missing Sound/SoundEffects.cfg leaves initSound
  # at SoundState = -1 (sfx.cpp:131 -- the ABORT beside it is commented out) and
  # the wavs load lazily per effect, while audio::LoadMIDIFile only pushes a
  # filename onto a vector (audio.cpp:449) and RtMidi is a dummy on this target
  # regardless (§9.3). So the audio data is opt-in: preloading it blocks the
  # first frame on the whole download for something that may not make a sound.
  option(WASM_PRELOAD_AUDIO "Preload Music/ and Sound/ too (adds ~28MB to the browser first load)" OFF)

  if(WASM_BROWSER AND WASM_NODERAWFS)
    message(FATAL_ERROR
            "WASM_BROWSER needs WASM_NODERAWFS=OFF -- NODERAWFS binds to node's fs "
            "module and there is no such thing in a browser.")
  endif()

  # SDL2, SDL2_mixer and libpng come from Emscripten's own ports rather than
  # from the system, so the find_package(SDL2/PNG REQUIRED) calls in FeLib,
  # Main and audio are skipped and their result variables are simply left
  # empty. Nothing else in those files has to change: they interpolate the
  # variables into target_link_libraries, and an empty variable expands to
  # nothing.
  #
  # The port flags are set globally, and that is deliberate. -sUSE_SDL=2 is a
  # *compile* flag as well as a link flag -- it is what puts the port's headers
  # on the include path -- so a target that misses it cannot find SDL.h, and a
  # target that gets a different set from its neighbours is the exact shape of
  # bug HARNESS.md §7.7 records: -DGCC in one sibling directory gave graphicid
  # two layouts inside one binary. Uniform beats scoped here, which is why this
  # is add_compile_options at the top level and not add_definitions per
  # directory.
  #
  # -fexceptions sits in the same list for the same reason, and it is not
  # optional. Emscripten compiles with exception *throwing* disabled by default:
  # __cxa_throw lowers to abort(), so the first throw kills the process with a
  # bare "Aborted(undefined)" and no clue as to where. IVAN throws as ordinary
  # control flow rather than only on error -- areachangerequest every time the
  # player enters or leaves a level (game.cpp:6429), quitrequest on quit
  # (game.cpp:4750), and genericException out of the prototype database
  # (database.cpp:966, caught in dungeon.cpp) -- so without this the WASM build
  # dies drawing the main menu and never reaches a dungeon at all. The compile
  # flag is the load-bearing half: it is what emits the landing pads, and a
  # translation unit built without it cannot catch what another one throws.
  #
  # -fexceptions (the JS-based lowering) rather than -fwasm-exceptions, because
  # ASYNCIFY below does not support native Wasm EH. Revisit the pair together if
  # asyncify is ever replaced by JSPI.
  set(IVAN_WASM_GLOBAL_FLAGS -sUSE_SDL=2 -sUSE_SDL_MIXER=2 -sUSE_LIBPNG=1 -fexceptions)
  add_compile_options(${IVAN_WASM_GLOBAL_FLAGS})
  string(REPLACE ";" " " IVAN_WASM_GLOBAL_FLAGS_LINK "${IVAN_WASM_GLOBAL_FLAGS}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${IVAN_WASM_GLOBAL_FLAGS_LINK}")

  # Runtime shape of the ivan binary. Applied to the executable in
  # Main/CMakeLists.txt; assembled here so every knob is in one place.
  #
  #   INITIAL_MEMORY   128MB. The 200-turn auto-play corpus peaks at 55.6MB RSS
  #                    natively (/usr/bin/time -v), and wasm32 halves every
  #                    pointer, so this is roughly 2x measured headroom rather
  #                    than a guess. ALLOW_MEMORY_GROWTH covers a corpus that
  #                    goes deeper.
  #   STACK_SIZE       8MB, to match the Linux default this code was written
  #                    against. Emscripten's default is 64KB, which the level
  #                    generator would find quickly.
  #   EXIT_RUNTIME     the harness writes its trace, PNG and text log on the way
  #                    out, and audio::Init registers an atexit handler. Without
  #                    this the runtime is kept alive when main returns and none
  #                    of that flushes.
  set(IVAN_WASM_LINK_FLAGS
      "-sINITIAL_MEMORY=134217728 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8388608 -sEXIT_RUNTIME=1")

  # NODERAWFS swaps MEMFS for the real filesystem. That is what the differential
  # harness needs -- it reads a recording and Graphics/Script/Music/Sound from
  # disk and writes a trace back, and under MEMFS those writes go nowhere. It is
  # node-only, so a browser build wants this OFF plus --preload-file for the data
  # directories and IDBFS for saves (§9 step 4).
  if(WASM_NODERAWFS)
    set(IVAN_WASM_LINK_FLAGS "${IVAN_WASM_LINK_FLAGS} -sNODERAWFS=1")
  endif()

  # Asyncify unwinds the stack around the blocking SDL_WaitEvent inside
  # globalwindowhandler::GetKey (§9 step 3). Note a *replay* never reaches it --
  # the harness returns the recorded key first (§4) -- so seam-1 differential
  # runs do not need this and are faster without it. Interactive play does.
  if(WASM_ASYNCIFY)
    set(IVAN_WASM_LINK_FLAGS "${IVAN_WASM_LINK_FLAGS} -sASYNCIFY=1")
  endif()

  # A browser build has no filesystem to read Graphics/ and Script/ from, so they
  # are baked into ivan.data and mounted at the MEMFS root. The mapping target is
  # absolute because PORTABLE_BUILD makes both game::GetDataDir() and
  # GetUserDataDir() return "./" (game.cpp:5426, save.cpp:814) while Emscripten
  # starts the module with the working directory at "/", so the "./Graphics/..."
  # every call site builds resolves to exactly the /Graphics below.
  #
  # Saves, screenshots and ivan.cfg land in MEMFS and do not survive a reload.
  # Making them persist is IDBFS, §9 step 5, and it is not done.
  if(WASM_BROWSER)
    set(IVAN_WASM_PRELOAD Graphics Script)
    if(WASM_PRELOAD_AUDIO)
      list(APPEND IVAN_WASM_PRELOAD Music Sound)
    endif()
    foreach(PreloadDir ${IVAN_WASM_PRELOAD})
      set(IVAN_WASM_LINK_FLAGS
          "${IVAN_WASM_LINK_FLAGS} --preload-file ${CMAKE_CURRENT_SOURCE_DIR}/${PreloadDir}@/${PreloadDir}")
    endforeach()
  endif()
endif()

find_path(EXECINFO_INCLUDE_DIR NAMES execinfo.h)
if(NOT EXECINFO_INCLUDE_DIR MATCHES "-NOTFOUND$")
  add_definitions(-DBACKTRACE)
endif()

add_subdirectory(FeLib)
add_subdirectory(audio)
add_subdirectory(Main)
add_subdirectory(igor)
add_subdirectory(mihail)
add_subdirectory(tools/savediff)
add_subdirectory(xbrzscale)
add_subdirectory(fantasyname)
add_subdirectory(FastNoise)
add_subdirectory(portmath)
