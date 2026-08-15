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

  # Emitting .html rather than .js is what makes emcc generate the page and the
  # canvas with it: Emscripten's SDL2 port looks up "#canvas" at video init and
  # has nothing to bind to otherwise. Set as a target property rather than
  # through CMAKE_EXECUTABLE_SUFFIX so igor, mihail and savediff keep building
  # as plain programs.
  if(WASM_BROWSER)
    set_target_properties(ivan PROPERTIES SUFFIX ".html")
  endif()
endif()

if(UNIX)
    install(TARGETS ivan DESTINATION "${CMAKE_INSTALL_BINDIR}")
else()
    install(TARGETS ivan DESTINATION "ivan")
endif()
