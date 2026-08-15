# portmath — one libm for every platform

Game logic calls `portmath::Sin` rather than `sin`, so that a native build and a
WASM build compute the same numbers.

```
portmath.h          what game code includes
musl_shim.h         ours: supplies musl's `hidden` and `weak_alias`
src/                vendored musl, byte-for-byte plus the renames vendor.sh applies
vendor.sh           regenerates src/ from upstream at a pinned commit
check-callers.py    fails if game code calls the host libm directly
COPYRIGHT           musl's licence (MIT)
```

## Why this is needed

`sin`, `cos`, `atan`, `log10` and `pow` are not correctly rounded by any
standard, so every libm is entitled to its own answer in the last ulp. IVAN
feeds those answers through `int()` and `(short)` truncation, where one ulp
stops being a rounding detail and becomes a different tile.

Measured rather than assumed. Every libm call the game makes was recorded under
`LD_PRELOAD` across both committed corpora, then replayed through musl and
compared against glibc bit-for-bit:

| function | calls | differ | max |
|---|---|---|---|
| `sincos` | 18,368 | 454 | 1 ulp |
| `log10` | 161 | 19 | 1 ulp |
| `log` | 8,967 | 0 | — |

So the divergence is real, it is small, and it is confined to the last ulp.

## Why musl

Emscripten's libm *is* musl. Vendoring musl means the WASM build agrees with
the vendored copy by construction rather than by luck, and native is the side
that moves.

Vendoring anything at all is the point: the host libm cannot be pinned, so the
only way both platforms run the same algorithm is to carry it ourselves.

## What actually changed when this landed: nothing

Both corpora produced byte-identical traces, text logs, screenshots, `.wm`
files and level files before and after the switch — verified against saves kept
from the previous binary, not just against the goldens.

That is worth reading carefully, because the obvious explanation is wrong. It
is **not** that world generation is insensitive to the last ulp. Per call site:

| site | function | calls | differ |
|---|---|---|---|
| `bitmap::DrawPolygon` | `sincos` | 3,600 | 216 |
| `worldmap::Generate` (Poisson sampler) | `sincos` | 3,400 | 132 |
| `femath::NormalDistributedRand` | `sincos` | 1,764 | 106 |
| `character::GetAdjustedStaminaCost` | `log10` | 73 | 18 |
| `character::CheckForBlockWithArm` | `log10` | 1 | 1 |
| `worldmap::PeriodicSimplexNoiseAltitude` | `sincos` | 9,604 | **0** |
| every `log` site | `log` | 8,967 | **0** |

World generation — the site HARNESS.md §6.3 singles out — is where the two
libms agree *exactly*. Its arguments are `x/XSize * 2π` for integer `x`, and
both implementations round those identically. The disagreements are everywhere
else, including 106 in `NormalDistributedRand`, which decides which way
monsters wander, and 19 in combat and stamina arithmetic.

So 473 calls returned a different number, in live game logic, and no observable
state moved. Those differences landed where nothing truncated across a boundary
— which is luck, and holds only for these two corpora and this seed. It is the
argument *for* pinning, not against: the next corpus has no reason to be as
lucky, and without portmath the failure would first appear as an unexplained
frame-hash divergence in a WASM build.

## The `sincos` trap

The symbol to grep for is not `sin` or `cos`. GCC rewrites `sin(x)` and `cos(x)`
on the same argument into a single `sincos(x)` call, and glibc's `sincos` is not
obliged to agree with its own `sin` and `cos`. So whether two builds match can
depend on whether the compiler chose to fuse — a portability hazard with no
visible call site.

This is why the first inventory taken here reported *no* `sin`/`cos` calls at
all from a binary making 18,368 of them. `portmath::SinCos` names the fused
form so the choice is explicit and identical everywhere.

## Updating

```bash
sh portmath/vendor.sh     # then git diff
```

A clean diff means `src/` is upstream at `MUSL_COMMIT` plus exactly two
documented transformations: the public entry points are renamed to `pm_*`, and
the two kernels that collide with glibc's `__MATHCALL_VEC` aliases (`__sin`,
`__cos`) become `pm_k_sin`/`pm_k_cos`. `libm.h` additionally has the
declarations for functions this subset does not vendor removed, two of which
(`__tan`, `__signgam`) conflict with glibc outright.

After updating, re-verify: `tools/corpora/verify-corpora.sh`. A change there is
a real behaviour change and needs the goldens regenerated deliberately, not
`--update`'d away.

## Two build flags that are load-bearing

Both are set on the target in `CMakeLists.txt`, never with `add_definitions()`
— HARNESS.md §7.7 records what a directory-scoped flag costs here.

- **`-ffp-contract=off`** stops GCC fusing `a*b+c` into an FMA. The fused form
  is *more* accurate, which is the problem: WASM has no FMA, so a contracted
  native build would not match a WASM one even from this same source.
- **`-fno-builtin`** stops GCC recognising these as the libm functions they are
  and constant-folding them at compile time with its own arithmetic.

## What is deliberately still on the host

`sqrt` (IEEE-754 mandates correct rounding, and WASM has a native `f64.sqrt`),
`fmod` (a remainder — computed, not approximated), and `floor`/`ceil`/`fabs`/
`abs`/`round`/`trunc`. `check-callers.py` allows exactly these.
