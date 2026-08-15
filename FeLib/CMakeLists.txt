include(FindPkgConfig)

file(GLOB FELIB_SOURCES Include/*.h Source/*.cpp)

option(IGNORE_EXTRA_WHITESPACES "Strip stray whitespace from linked library names" OFF)

# This used to be cmake_policy(SET CMP0004 OLD), which tolerated leading and
# trailing whitespace in library names. CMake 4 removed that OLD behaviour
# outright -- setting it is a hard error -- so the whitespace has to be stripped
# rather than tolerated. Stripping is what CMP0004 NEW wants anyway and works on
# every CMake version. The whitespace comes from pkg-config: several ${X_LDFLAGS}
# results below are assigned straight into ${X_LIBRARIES}.
function(ivan_strip_library_whitespace VarName)
  set(Stripped "")
  foreach(Lib IN LISTS ${VarName})
    string(STRIP "${Lib}" Lib)
    if(NOT Lib STREQUAL "")
      list(APPEND Stripped "${Lib}")
    endif()
  endforeach()
  set(${VarName} "${Stripped}" PARENT_SCOPE)
endfunction()

add_library(FeLib ${FELIB_SOURCES})

# Under Emscripten both of these come from -sUSE_SDL=2 / -sUSE_LIBPNG=1, set
# globally in the top-level CMakeLists. Searching the host for them would find
# the native libraries and link them into a wasm32 target.
if(NOT EMSCRIPTEN)
  find_package(SDL2 REQUIRED)

  # Try finding libpng using pkg-config first, because find_package(PNG) finds a
  # mismatching header + library combination for me.
  include(FindPkgConfig)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(PNG libpng)
    set(PNG_LIBRARIES ${PNG_LDFLAGS})
  endif()
  if(NOT PNG_FOUND)
    find_package(PNG REQUIRED)
    add_definitions(${PNG_DEFINITIONS})
  endif()
endif()

if(MINGW)
  # Not very pretty solution. This finds SDL2.dll from SDL2.lib path, so that it can be installed where ivan.exe will end up.
  message("SDL2_LIBRARY ${SDL2_LIBRARY}")
  list(GET SDL2_LIBRARY 2 sdl_lib_dir)
  message("sdl_lib_dir ${sdl_lib_dir}")
  get_filename_component(sdl_lib_dir "${sdl_lib_dir}" DIRECTORY)
  message("sdl_lib_dir ${sdl_lib_dir}")
  get_filename_component(deps_path "${sdl_lib_dir}" DIRECTORY)

  if((NOT BUILD_SHARED_LIBS) AND BUILD_STATIC_LIBS)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
    set(SDL2_LIBRARIES ${SDL2_LIBRARY} ${SDL2_LDFLAGS})
  else()
    cmake_policy(SET CMP0087 NEW)
    set(IVAN_DEPENDENCY_PATHS "${deps_path}/bin/")
    message("IVAN_DEPENDENCY_PATHS ${IVAN_DEPENDENCY_PATHS}")

    # Transfer the value of ${IVAN_DEPENDENCY_PATHS} into the install script
    install(CODE "set(IVAN_DEPENDENCY_PATHS \"${IVAN_DEPENDENCY_PATHS}\")")

    string(REPLACE "/" "[\\\\/]" IVAN_DEPENDENCY_PATHS_FILTER ${IVAN_DEPENDENCY_PATHS})
    string(REPLACE ":" "\\\\:" IVAN_DEPENDENCY_PATHS_FILTER ${IVAN_DEPENDENCY_PATHS_FILTER})
    # message("IVAN_DEPENDENCY_PATHS_FILTER ${IVAN_DEPENDENCY_PATHS_FILTER}")

    LIST(APPEND post_include_regex ".*${IVAN_DEPENDENCY_PATHS_FILTER}*") # gnu dlls
    # message("post_include_regex ${post_include_regex}")
    # Transfer the value of ${post_include_regex} into the install script
    install(CODE "set(post_include_regex \"${post_include_regex}\")")

    install(CODE [[
      file(GET_RUNTIME_DEPENDENCIES
        LIBRARIES ${PNG_LIBRARIES} ${SDL2_LIBRARY} ${SDL2_mixer_LIBRARY}
        EXECUTABLES $<TARGET_FILE:ivan>
        RESOLVED_DEPENDENCIES_VAR _r_deps
        UNRESOLVED_DEPENDENCIES_VAR _u_deps
        DIRECTORIES ${IVAN_DEPENDENCY_PATHS}
        POST_EXCLUDE_REGEXES "^.*\\.dll$" # First exclude all dlls
        POST_INCLUDE_REGEXES ${post_include_regex} # Thereafter include only the desired dlls
      )
      foreach(_file ${_r_deps})
        file(INSTALL
          DESTINATION "${CMAKE_INSTALL_PREFIX}/ivan"
          TYPE SHARED_LIBRARY
          FOLLOW_SYMLINK_CHAIN
          FILES "${_file}"
        )
      endforeach()
      list(LENGTH _u_deps _u_length)
      if("${_u_length}" GREATER 0)
        message(WARNING "Unresolved dependencies detected!")
        # message("Unresolved dependencies include: ${_u_deps}")
      endif()
    ]])
  endif() # if STATIC
endif() # if MINGW

if(MSVC AND _VCPKG_INSTALLED_DIR)
  # Gum solution for manually copying dynamic link libraries from the vcpkg bin directory
  install(FILES "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/libpng16.dll" DESTINATION "ivan")
  install(FILES "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/zlib1.dll" DESTINATION "ivan")
  install(FILES "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/SDL2.dll" DESTINATION "ivan")
  install(FILES "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/SDL2_mixer.dll" DESTINATION "ivan")
endif()

target_include_directories(FeLib PUBLIC Include)
target_include_directories(FeLib SYSTEM PUBLIC ${SDL2_INCLUDE_DIR} PRIVATE ${PNG_INCLUDE_DIRS} ${SDL2_mixer_INCLUDE_DIR})
target_include_directories(FeLib PUBLIC ../xbrzscale ../xbrzscale/xbrz)
target_include_directories(FeLib PUBLIC ../portmath)

if(IGNORE_EXTRA_WHITESPACES)
  ivan_strip_library_whitespace(PNG_LIBRARIES)
  ivan_strip_library_whitespace(SDL2_LIBRARY)
  ivan_strip_library_whitespace(SDL2_LIBRARIES)
  ivan_strip_library_whitespace(SDL2_mixer_LIBRARY)
endif()

if((NOT BUILD_SHARED_LIBS) AND BUILD_STATIC_LIBS)
  target_link_libraries(FeLib ${PNG_LIBRARIES} ${SDL2_LIBRARIES} portmath ${SDL2_mixer_LIBRARY} -static-libgcc -static-libstdc++)
else()
  target_link_libraries(FeLib ${PNG_LIBRARIES} ${SDL2_LIBRARY} portmath ${SDL2_mixer_LIBRARY})
endif()

target_compile_options(FeLib PUBLIC ${SDL2_CFLAGS} ${SDL2_CFLAGS_OTHER})

