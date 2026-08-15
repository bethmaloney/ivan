# tools/web — collecting and diagnosing a browser crash

The goal is that an ordinary session, played with no foresight and no special
flags, produces enough on a crash to find the bug. That means two things have to
be true *before* the crash, because neither can be arranged afterwards:

- the binary has to carry function names, or the trap prints byte offsets that
  nothing can resolve;
- the session has to have been recording, or there is no way to reproduce it.

Both are on by default in a `WASM_BROWSER` build. Nothing below needs enabling.

## What you get when it crashes

The console prints a report and it is also saved to `localStorage`, so it
survives the reload or the closed tab that usually follows:

```
ivan: unhandled rejection
RuntimeError: memory access out of bounds
    at character::Move (ivan.wasm:0x...)
    ...

--- crash report ---
build   v059-55-gf7d61e1
seed    1755312345
keys    403
stored  ivanHarness.reports() / ivanHarness.save()

Reproduce it natively:
  ivanHarness.saveRecording()   then
  ./ivan --replay session.rec
```

From the console:

```js
ivanHarness.reports()        // every stored report, newest first
ivanHarness.save()           // newest one as a .json file
ivanHarness.saveRecording()  // just the .rec, ready for --replay
ivanHarness.text()           // the live recording, crash or no crash
ivanHarness.report('note')   // file one for something that did not crash
ivanHarness.clear()          // forget them
```

**The recording is the valuable part.** A stack says where it stopped; the
recording says how to get back there. It carries the seed in its header, so

```bash
./ivan --replay session.rec
```

replays the session on the native build, where gdb, valgrind and ASan all apply.
That works because `harness::RecordKey` flushes every key as it writes it
(`FeLib/Source/harness.cpp:545`) — a trap leaves the recording complete but for
its `# end keys=` trailer, so the keys that led to the crash survive it.

Recording changes nothing about how the game plays. The seed it pins is the same
`time(0)` the game would have used anyway (`Main/Source/main.cpp:154`); it just
writes it down. Opt out with `?record=off` if you ever need to.

## Why a stripped trap is not a lead

```
Uncaught (in promise) RuntimeError: memory access out of bounds
    at ivan.wasm:0xed4dd
```

Those are byte offsets. A build without `--profiling-funcs` has no name section
and no DWARF, so **nothing** maps them back to a function — not `emsymbolizer`,
which needs DWARF; not the browser; and not a later rebuild, because the offsets
belong to the exact binary that produced them and even that binary no longer
knows its own function names. The information was never emitted.

This is why names are unconditional for browser builds rather than an option:
the cost is ~800KB paid once at build time, and the loss is a report from a
crash nobody can reproduce to order. `--profiling-funcs` changes no codegen.

Match the report's `build` field against the binary before trusting a trace.
Symbolizing one build's offsets against another's gives confident, wrong
answers, which is worse than no answer.

## Turning the screws further

`-DWASM_SAFE_HEAP=ON` bounds-checks every load and store, naming the offending
access and address outright. It is slow enough to make the game hard to steer by
hand, so use it with a recording that already reproduces the crash rather than
while hunting for one:

```bash
emcmake cmake -S . -B build-web-safe -DCMAKE_BUILD_TYPE=Release -DWIZARD=ON \
  -DPORTABLE_BUILD=ON -DWASM_BROWSER=ON -DWASM_SAFE_HEAP=ON
```

`-DWASM_DEBUG=OFF` drops `ASSERTIONS=2` and `STACK_OVERFLOW_CHECK=2` if you want
the faster build back. It defaults ON for browser builds and OFF for node, where
the differential corpora would pay the cost on every run.

## If it will not reproduce natively

That is a finding, not a dead end. wasm32 traps on accesses x86-64 absorbs: a
read past a buffer is still inside a mapped page natively and returns garbage,
where in wasm it is either outside linear memory or caught by `SAFE_HEAP`. Same
for a null dereference, and for a `long` that was 8 bytes when a save was written
and is 4 here (HARNESS.md §6.8, §7.9).

So a WASM-only crash usually means a real latent defect that native testing
structurally cannot see — the class HARNESS.md §6.6 and §9.4 were about. Reach
for `SAFE_HEAP` plus the recording, and compare a native `--trace` of the same
recording against the WASM one to find the last frame they agreed on.

## Sending reports somewhere

Reports are local-only by default: `localStorage` and the console, with
`ivanHarness.save()` to get one out by hand.

The POST hook is wired but inert. Set an endpoint at build time, or per-session
without a rebuild:

```bash
emcmake cmake ... -DWASM_CRASH_ENDPOINT=https://example.com/ivan-crash
```

```
ivan.html?crashlog=https://example.com/ivan-crash
```

It sends the whole report as JSON, `keepalive` so it outlives the page — which a
crash is usually followed by. `keepalive` caps a body at 64KB, so a long
recording is dropped from the POST and kept locally rather than losing the
report; the key count says so. Nothing is sent when no endpoint is set.
