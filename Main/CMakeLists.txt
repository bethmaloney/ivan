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

add_executable(ivan ${IVAN_SOURCES})
target_include_directories(ivan PUBLIC Include ../Felib/Include ../audio ../fantasyname ../FastNoise ${SDL2_mixer_INCLUDE_DIR})
target_link_libraries(ivan FeLib FeAudio xbrzscale fantasyname fastnoise portmath ${SDL2_mixer_LIBRARY})

if(EMSCRIPTEN)
  # Assembled in the top-level CMakeLists so the knobs live in one place.
  # LINK_FLAGS rather than target_link_options because the floor is CMake 3.10
  # and target_link_options arrived in 3.13.
  set_target_properties(ivan PROPERTIES LINK_FLAGS "${IVAN_WASM_LINK_FLAGS}")

  # --shell-file is a link input, but it appears only inside LINK_FLAGS as far
  # as CMake can see, so nothing reruns the link when it changes. The failure is
  # silent and expensive: the build says it is up to date and the page keeps the
  # previous copy, so an edit appears to have no effect and gets debugged as a
  # code problem. It is the last one -- saves.js was the other, and it is
  # web/src/saves now, where a change costs a bundle rather than a relink.
  if(WASM_BROWSER)
    set_target_properties(ivan PROPERTIES
                          LINK_DEPENDS "${CMAKE_SOURCE_DIR}/tools/web/shell.html")
  endif()

  # Emitting .html rather than .js is what makes emcc generate the page and the
  # canvas with it: Emscripten's SDL2 port looks up "#canvas" at video init and
  # has nothing to bind to otherwise. Set as a target property rather than
  # through CMAKE_EXECUTABLE_SUFFIX so igor and savediff keep building
  # as plain programs.
  if(WASM_BROWSER)
    set_target_properties(ivan PROPERTIES SUFFIX ".html")

    # The page's own JavaScript, bundled beside ivan.html where the shell's
    # <script src="ivan-page.js"> resolves it. Not a link input: it is an
    # ordinary script the page loads, which is what lets an edit to web/src cost
    # a rebuild measured in milliseconds instead of a relink.
    #
    # Always run rather than dependency-tracked. esbuild takes single-digit
    # milliseconds, and the failure it avoids is the one LINK_DEPENDS above
    # exists for: a stale bundle is silent, and the build reports success while
    # the page keeps serving the previous copy.
    #
    # No `cmake -E env` wrapper: build.mjs takes IVAN_BUILD_ID and
    # IVAN_CRASH_ENDPOINT from the environment, and the environment reaches a
    # custom command already. Forwarding CMake's own copies is what used to happen
    # and it was worse than nothing -- `cmake -E env "IVAN_CRASH_ENDPOINT="` with
    # the cache variable at its empty default *overwrote* an ambient value, so the
    # override build.mjs documents could not be used through a CMake build at all.
    # Measured on cmake 3.28.3, and it was silent because the value had never been
    # set anywhere.
    add_custom_target(ivan-page ALL
                      COMMAND ${IVAN_NODE} build.mjs --outdir $<TARGET_FILE_DIR:ivan>
                      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/web
                      COMMENT "Bundling web/ beside the page")

    add_dependencies(ivan ivan-page)

    # Sound/ and Music/ are the data directories the page fetches over HTTP
    # instead of reading out of ivan.data -- effects per docs/port-log.md §9.7,
    # music per §9.8 -- so both have to be reachable at a URL relative to
    # ivan.html. Symlinked
    # rather than copied: together they are ~28MB, they are read-only, and a
    # copy would go stale the moment a wav or a stem changed. This is what keeps
    # the documented `emrun --no_browser build-web/Main` serving a complete game
    # with no extra argument.
    #
    # Music/ carries the .mid too, which the page never asks for. Linking the
    # directory rather than picking files out of it keeps this rule simple and
    # costs nothing: a symlink does not copy, and an unrequested file is not
    # served.
    foreach(AudioDir Sound Music)
      add_custom_command(TARGET ivan POST_BUILD
                         COMMAND ${CMAKE_COMMAND} -E remove -f
                                 $<TARGET_FILE_DIR:ivan>/${AudioDir}
                         COMMAND ${CMAKE_COMMAND} -E create_symlink
                                 ${CMAKE_SOURCE_DIR}/${AudioDir}
                                 $<TARGET_FILE_DIR:ivan>/${AudioDir}
                         COMMENT "Linking ${AudioDir}/ beside the page for browser audio")
    endforeach()

    # The shell asks for /fonts/fonts.css and /icon.png at absolute paths,
    # because on the deployed site the page sits at /play/ and shares both with
    # the landing page at / rather than carrying a second copy. Linking the
    # site root's assets in beside the page is what makes a local
    # tools/web/serve.py -- whose root is this directory -- resolve the same
    # URLs the deployed site does. Without it the page still plays; it just
    # draws in the fallback serif, which is the kind of difference between dev
    # and production that gets noticed late.
    foreach(SiteAsset fonts icon.png)
      add_custom_command(TARGET ivan POST_BUILD
                         COMMAND ${CMAKE_COMMAND} -E remove -f
                                 $<TARGET_FILE_DIR:ivan>/${SiteAsset}
                         COMMAND ${CMAKE_COMMAND} -E create_symlink
                                 ${CMAKE_SOURCE_DIR}/tools/web/site/${SiteAsset}
                                 $<TARGET_FILE_DIR:ivan>/${SiteAsset}
                         COMMENT "Linking ${SiteAsset} beside the page")
    endforeach()
  endif()
endif()

if(UNIX)
    install(TARGETS ivan DESTINATION "${CMAKE_INSTALL_BINDIR}")
else()
    install(TARGETS ivan DESTINATION "ivan")
endif()
