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

  # Music/ and Sound/ are ~28MB against Graphics/ and Script/'s ~3.5MB, and
  # neither belongs in ivan.data. Neither is needed there either, now that both
  # are played by the page and fetched over HTTP: effects by tools/web/sfx.js
  # (§9.7), music by tools/web/music.js (§9.8). Preloading would put 28MB back
  # on the first load to populate a MEMFS nothing reads.
  #
  # For Music/ it would be worse than useless. What the page fetches is the
  # rendered OGG stems and stems.json beside them; the .mid are the *source*
  # for those, they are never read on this target, and nothing here can play
  # them -- the MIDI parser and playback engine are not even compiled (see
  # audio/CMakeLists.txt). Preloading Music/ buys 1.5MB of files with no
  # reader.
  #
  # Kept as an escape hatch for serving the page from somewhere that cannot
  # serve the directories beside it.
  option(WASM_PRELOAD_AUDIO "Preload Music/ and Sound/ into ivan.data (~28MB; neither is needed, see §9.7 and §9.8)" OFF)

  # Function names are not in here: WASM_BROWSER keeps them unconditionally,
  # because a report from a player's crash is worthless without them and cannot
  # be re-taken to order (§9.6). What is left is the part with a runtime cost.
  #
  #   ASSERTIONS=2           runtime checks with messages, rather than a trap.
  #   STACK_OVERFLOW_CHECK=2 the 8MB stack is generous but the level generator is
  #                          recursive, and an overflow otherwise looks exactly
  #                          like the out-of-bounds access it is mistaken for.
  # Defaults ON for a browser build and OFF for node. The node host runs the
  # differential corpora, where the measurement is the point and a runtime cost
  # is a tax on every run; the browser host is played by a human who would
  # rather be told what went wrong than have it inferred afterwards from a trap.
  if(WASM_BROWSER)
    set(IVAN_WASM_DEBUG_DEFAULT ON)
  else()
    set(IVAN_WASM_DEBUG_DEFAULT OFF)
  endif()
  option(WASM_DEBUG "Turn on runtime assertions and stack checking in the WASM build" ${IVAN_WASM_DEBUG_DEFAULT})

  # Separate because it is the expensive one: SAFE_HEAP instruments every load
  # and store, which is what turns "trapped somewhere" into "this access, this
  # address", at a slowdown that makes the game unpleasant to steer. Turn it on
  # once a recording reproduces the crash, not while hunting for one.
  option(WASM_SAFE_HEAP "Bounds-check every load and store (slow; use with a recording)" OFF)

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

    # The one file under Sound/ that is not audio. §9.7 moved *playback* to the
    # page but deliberately left the pattern table in C++, and initSound reads
    # it with fopen, so SoundEffects.cfg has to exist in MEMFS even though none
    # of the wavs beside it do. Miss this and there is no failure to see: the
    # open fails, initSound settles on SoundState = -1 (the ABORT next to it is
    # commented out), and playSound returns before reaching the bridge -- a
    # game that is silent for a reason nothing prints. 10KB.
    if(NOT WASM_PRELOAD_AUDIO)
      set(IVAN_WASM_LINK_FLAGS
          "${IVAN_WASM_LINK_FLAGS} --preload-file ${CMAKE_CURRENT_SOURCE_DIR}/Sound/SoundEffects.cfg@/Sound/SoundEffects.cfg")
    endif()

    # tools/web/harness-pre.js turns the query string into argv and collects a
    # crash report. It needs FS on Module to read MEMFS, which is not exported
    # by default. See tools/web/README.md.
    set(IVAN_WASM_LINK_FLAGS
        "${IVAN_WASM_LINK_FLAGS} -sEXPORTED_RUNTIME_METHODS=FS,callMain")

    # A crash report names the build it came from, because a stack trace is only
    # meaningful against the binary that produced it -- symbolizing last week's
    # report against this week's wasm gives confident, wrong answers. Git
    # describes the source; a dirty tree says so, since that is exactly the
    # build whose report nobody else can reproduce.
    find_package(Git QUIET)

    if(GIT_FOUND)
      execute_process(COMMAND ${GIT_EXECUTABLE} describe --always --dirty --tags
                      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                      OUTPUT_VARIABLE IVAN_BUILD_ID
                      OUTPUT_STRIP_TRAILING_WHITESPACE
                      ERROR_QUIET)
    endif()

    if(NOT IVAN_BUILD_ID)
      set(IVAN_BUILD_ID "unknown")
    endif()

    # Where crash reports are POSTed. Empty means they are only kept locally,
    # in localStorage and on the console. Overridable per-session with
    # ?crashlog=<url> so a report can be redirected without a rebuild.
    set(WASM_CRASH_ENDPOINT "" CACHE STRING
        "URL to POST browser crash reports to (empty: keep them local only)")

    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/tools/web/harness-pre.js.in
                   ${CMAKE_CURRENT_BINARY_DIR}/harness-pre.js @ONLY)

    set(IVAN_WASM_LINK_FLAGS
        "${IVAN_WASM_LINK_FLAGS} --pre-js ${CMAKE_CURRENT_BINARY_DIR}/harness-pre.js")

    # Sound effects, played by the page rather than by SDL_mixer (§9.7). No
    # configure_file: nothing in it is substituted, and what would have been
    # build-time settings are query-string options so they can be changed
    # without a rebuild, exactly as ?crashlog= is.
    set(IVAN_WASM_LINK_FLAGS
        "${IVAN_WASM_LINK_FLAGS} --pre-js ${CMAKE_CURRENT_SOURCE_DIR}/tools/web/sfx.js")

    # Music, likewise played by the page (§9.8). After sfx.js, and that order
    # matters: music.js borrows the AudioContext sfx.js owns rather than
    # opening a second one, so the object it looks for has to exist by the time
    # it runs. Both are plain --pre-js with nothing substituted, and their
    # options are query-string rather than build-time for the same reason
    # ?crashlog= is.
    set(IVAN_WASM_LINK_FLAGS
        "${IVAN_WASM_LINK_FLAGS} --pre-js ${CMAKE_CURRENT_SOURCE_DIR}/tools/web/music.js")

    # Function names are kept in *every* browser build, not just a debug one.
    # --profiling-funcs preserves the name section and changes no codegen, so it
    # costs about 800KB and nothing else, and without it a trap from a player's
    # session prints byte offsets that nothing on earth can resolve (§9.6). The
    # asymmetry is the point: the cost is paid once at build time, the loss is
    # paid on a crash that cannot be reproduced to order.
    set(IVAN_WASM_LINK_FLAGS "${IVAN_WASM_LINK_FLAGS} --profiling-funcs")
  endif()

  if(WASM_DEBUG)
    set(IVAN_WASM_LINK_FLAGS
        "${IVAN_WASM_LINK_FLAGS} -sASSERTIONS=2 -sSTACK_OVERFLOW_CHECK=2")
  endif()

  if(WASM_SAFE_HEAP)
    set(IVAN_WASM_LINK_FLAGS "${IVAN_WASM_LINK_FLAGS} -sSAFE_HEAP=1")
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
