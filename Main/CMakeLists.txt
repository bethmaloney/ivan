include(FindPkgConfig)

if(NOT EMSCRIPTEN)
  find_package(SDL2 REQUIRED)
endif()

file(GLOB IVAN_SOURCES Include/*.h Source/*.cpp)
# The following files are #included from other .cpp files and so shouldn't be
# compiled directly. Setting the HEADER_FILE_ONLY property to TRUE prevents it.
set_source_files_properties(
    Source/action.cpp   Source/actions.cpp   Source/area.cpp     Source/bodypart.cpp
    Source/char.cpp     Source/cont.cpp      Source/database.cpp Source/entity.cpp
    Source/fluid.cpp    Source/gear.cpp      Source/god.cpp      Source/gods.cpp
    Source/human.cpp    Source/item.cpp      Source/level.cpp    Source/lsquare.cpp
    Source/lterra.cpp   Source/lterras.cpp   Source/materia.cpp  Source/materias.cpp
    Source/miscitem.cpp Source/nonhuman.cpp  Source/pool.cpp     Source/proto.cpp
    Source/rain.cpp     Source/room.cpp      Source/rooms.cpp    Source/slot.cpp
    Source/smoke.cpp    Source/square.cpp    Source/stack.cpp    Source/team.cpp
    Source/terra.cpp    Source/trap.cpp      Source/traps.cpp    Source/worldmap.cpp
    Source/wsquare.cpp  Source/wterra.cpp    Source/wterras.cpp  Source/hiteffect.cpp
    Source/cmdcraft.cpp Source/cmdcraftfilters.cpp
    Source/cmdswapweap.cpp
    PROPERTIES HEADER_FILE_ONLY TRUE)

add_executable(ivan ${IVAN_SOURCES} Resource/Ivan.rc)
target_include_directories(ivan PUBLIC Include ../Felib/Include ../audio ../fantasyname ../FastNoise ${SDL2_mixer_INCLUDE_DIR})
target_link_libraries(ivan FeLib FeAudio xbrzscale fantasyname fastnoise portmath ${SDL2_mixer_LIBRARY})

if(EMSCRIPTEN)
  # Assembled in the top-level CMakeLists so the knobs live in one place.
  # LINK_FLAGS rather than target_link_options because the floor is CMake 3.10
  # and target_link_options arrived in 3.13.
  set_target_properties(ivan PROPERTIES LINK_FLAGS "${IVAN_WASM_LINK_FLAGS}")

  # --pre-js files are link inputs, but they appear only inside LINK_FLAGS as
  # far as CMake can see, so nothing reruns the link when one of them changes.
  # The failure is silent and expensive: the build says it is up to date and
  # the page keeps the previous copy of the JS, so an edit appears to have no
  # effect and gets debugged as a code problem.
  if(WASM_BROWSER)
    set_target_properties(ivan PROPERTIES
                          LINK_DEPENDS "${CMAKE_BINARY_DIR}/harness-pre.js;${CMAKE_SOURCE_DIR}/tools/web/sfx.js")
  endif()

  # Emitting .html rather than .js is what makes emcc generate the page and the
  # canvas with it: Emscripten's SDL2 port looks up "#canvas" at video init and
  # has nothing to bind to otherwise. Set as a target property rather than
  # through CMAKE_EXECUTABLE_SUFFIX so igor, mihail and savediff keep building
  # as plain programs.
  if(WASM_BROWSER)
    set_target_properties(ivan PROPERTIES SUFFIX ".html")

    # Sound/ is the one data directory the page fetches over HTTP instead of
    # reading out of ivan.data (§9.7), so it has to be reachable at a URL
    # relative to ivan.html. Symlinked rather than copied: it is 26MB, it is
    # read-only, and a copy would go stale the moment a wav changed. This is
    # what keeps the documented `emrun --no_browser build-web/Main` serving a
    # complete game with no extra argument.
    add_custom_command(TARGET ivan POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E remove -f
                               $<TARGET_FILE_DIR:ivan>/Sound
                       COMMAND ${CMAKE_COMMAND} -E create_symlink
                               ${CMAKE_SOURCE_DIR}/Sound
                               $<TARGET_FILE_DIR:ivan>/Sound
                       COMMENT "Linking Sound/ beside the page for §9.7 audio")
  endif()
endif()

if(UNIX)
    install(TARGETS ivan DESTINATION "${CMAKE_INSTALL_BINDIR}")
else()
    install(TARGETS ivan DESTINATION "ivan")
endif()
