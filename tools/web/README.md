# tools/web — collecting and diagnosing a browser crash

A wasm trap in a release build tells you nothing:

```
Uncaught (in promise) RuntimeError: memory access out of bounds
    at ivan.wasm:0xed4dd
    at ivan.wasm:0x5286d
```

Those are byte offsets into the binary. A release build carries no name section
and no DWARF, so **nothing can map them back to a function** — not `emsymbolizer`,
not the browser, not a later rebuild, because the offsets belong to the exact
binary that produced them and even that one no longer knows its own function
names. A trace like the one above is not a lead. Treat it as a prompt to rebuild
and reproduce, and throw it away.

Two things fix that, and they are independent: make the *next* trap legible, and
make the crash *reproducible* so it can be moved to a real debugger.

## 1. Make the trap legible

```bash
emcmake cmake -S . -B build-web-dbg -DCMAKE_BUILD_TYPE=Release -DWIZARD=ON \
  -DPORTABLE_BUILD=ON -DWASM_BROWSER=ON -DWASM_DEBUG=ON
cmake --build build-web-dbg -j$(nproc)
```

`WASM_DEBUG` keeps the name section (`--profiling-funcs`, no codegen change) and
turns on `ASSERTIONS=2` and `STACK_OVERFLOW_CHECK=2`. Stack frames become
`character::Move` rather than `0xed4dd`, and an assertion failure or a blown
stack reports itself as what it is instead of as a generic trap.

Add `-DWASM_SAFE_HEAP=ON` to bounds-check every load and store. That names the
offending access and address outright, at a slowdown that makes the game hard to
steer by hand — so turn it on once you have a recording that reproduces the
crash, not while hunting for one.

## 2. Make the crash reproducible

The point is not to debug in the browser. It is to get the crash onto the
**native** build, where there is gdb, valgrind and ASan, and where the harness
already replays a session deterministically (HARNESS.md §4).

`harness-pre.js` is linked into every `WASM_BROWSER` build and does two things.
It turns the query string into argv:

```
http://localhost:8111/ivan.html?record=/session.rec&seed=999
```

and it hands the recording back when the game crashes — printing it to the
console and offering it as a download. Manually, at any point:

```js
ivanHarness.download('/session.rec')   // save it
ivanHarness.text('/session.rec')       // or just look at it
```

**A crash does not lose the recording.** `harness::RecordKey` flushes every key
as it writes it (`FeLib/Source/harness.cpp:545`), so a trap leaves the file
complete but for its `# end keys=` trailer — the keys that led to the crash are
all there. The seed rides along in the header, so the file is self-contained:

```bash
./ivan --replay session.rec --trace crash.jsonl
```

Pin `?seed=` anyway when you can. A recording made without one carries whatever
`time(0)` returned, which replays fine but makes two sessions incomparable.

## 3. Where to look once it reproduces

If it reproduces natively, it is an ordinary bug and the native toolchain is
better at it than anything here:

```bash
valgrind --track-origins=yes ./ivan --replay session.rec
g++ -fsanitize=address,undefined ...   # or a purpose-built binary
```

**If it does not reproduce natively, that is a finding, not a dead end.** wasm32
traps on accesses x86-64 absorbs silently: a read a few bytes past a buffer is
still inside a mapped page natively and returns garbage, where in wasm it is
either out of linear memory or caught by `SAFE_HEAP`. The same is true of a null
dereference, and of a `long` that was 8 bytes when the save was written and is 4
here (HARNESS.md §6.8, §7.9).

So a WASM-only crash usually means a real latent defect that native testing
structurally cannot see — which is the same class of bug §6.6 and §9.4 were
about, and the reason the second host is worth having. Reach for `SAFE_HEAP`
plus the recording, and compare against a native `--trace` of the same recording
to find the last frame the two agreed on.
