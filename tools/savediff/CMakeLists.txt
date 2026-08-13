set(SAVEDIFF_VERSION 1.0)

# Deliberately no target_link_libraries: savediff is the differential oracle and
# must build and run when the game build is broken.
add_executable(savediff EXCLUDE_FROM_ALL Source/savediff.cpp)
target_include_directories(savediff PUBLIC ../../FeLib/Include)
