file(GLOB PORTMATH_SOURCES src/*.c)

add_library(portmath ${PORTMATH_SOURCES})

target_include_directories(portmath PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
                                    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Both of these are load-bearing, and both are set on the target rather than
# with add_definitions(). HARNESS.md §7.7 is what a directory-scoped flag costs:
# -DGCC in a sibling directory gave one struct two layouts in one binary.
#
#   -ffp-contract=off  stops GCC fusing a*b+c into an FMA. The fused form is
#                      more accurate, which is the problem -- WASM has no FMA,
#                      so a contracted native build would not match a WASM one
#                      even compiled from this same source.
#   -fno-builtin       stops GCC recognising these as the libm functions they
#                      are and constant-folding them with its own MPFR-based
#                      arithmetic at compile time, which need not agree with
#                      what the code computes at run time.
#
# The force-include supplies musl's `hidden` and `weak_alias` without editing
# any vendored file -- see portmath/musl_shim.h.
target_compile_options(portmath PRIVATE
  -ffp-contract=off
  -fno-builtin
  -include ${CMAKE_CURRENT_SOURCE_DIR}/musl_shim.h)
