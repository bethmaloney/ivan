/*
 * The query string, as argv (HARNESS.md §4, §9.6).
 *
 * The node host gets its harness arguments from the command line. A browser has
 * no command line, so this is it: ?seed=999&headless becomes
 * ["--seed", "999", "--headless"] on Module.arguments, which is where
 * Emscripten's runtime looks for argv before it calls main().
 *
 * RECORDING IS ON BY DEFAULT, and that is the whole point. A crash you have to
 * have predicted is no use: by the time you know you want a recording, the
 * session that would have produced one is gone. Recording costs nothing that
 * matters -- harness::RecordKey appends a line per keystroke -- and it changes
 * nothing about how the game plays, because the seed it pins is the same
 * time(0) main.cpp would have used anyway (main.cpp:154). Opt out with
 * ?record=off.
 */

import * as Query from '../platform/query.ts';

/* Where the recording goes when ?record names no path of its own. In MEMFS, not
   IDBFS: it is read back out by report.ts within the session and has no reason
   to outlive the tab, and putting it under /ivan would push a megabyte of
   keystrokes through saves.js's debounce on every key. */

export const DefaultRecordPath = '/session.rec';

/* Answered by the page and meaningless to ParseArgs, so they are the two the
   argv below leaves out. Every other option is forwarded verbatim: ParseArgs is
   an if/else-if chain with no else (harness.cpp:357), so ?sfx=off arriving as
   "--sfx off" is ignored rather than rejected, and the page keeps one list of
   options rather than two that have to agree. */

const PageOnly = ['record', 'crashlog'];

/* Null under ?record=off, which is also what tells report.ts there is no
   recording to put in a report. */

export function RecordPath(): string | null {
  if(!Query.Enabled('record'))
    return null;

  /* `?record=` with nothing after it is a request for a recording, not a
     request for one called "". */

  return Query.Setting('record') || DefaultRecordPath;
}

/* A bare key becomes a flag with no value, so ?headless is --headless while
   ?seed=999 is --seed 999. */

export function Argv(): string[] {
  const Args: string[] = [];

  for(const [Key, Value] of Query.All()) {
    if(PageOnly.includes(Key))
      continue;

    Args.push('--' + Key);

    if(Value !== '')
      Args.push(Value);
  }

  const Path = RecordPath();

  if(Path)
    Args.push('--record', Path);

  return Args;
}
