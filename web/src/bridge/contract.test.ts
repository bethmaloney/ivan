/*
 * The C++ -> page bridge contract. Run with: npm test
 *
 * Parses the EM_JS blocks out of the game's own sources and checks them against
 * contract.ts beside it. Parsed rather than trusted, for the same reason
 * dist.py parses SoundEffects.cfg instead of globbing Sound/: a declaration
 * checked against itself would agree with itself and prove nothing.
 *
 * The first run of this found two bridges -- IvanMusicPlaying and
 * IvanMusicVolume -- that tools/web/README.md's bridge table does not mention.
 */

import { test } from 'node:test';
import { deepStrictEqual } from 'node:assert';
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { dirname, join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { Bridges, SourceDirs } from './contract.ts';

/* Found by walking up to the marker rather than by counting `..`, because this
   file moved once already and a wrong count does not fail here -- Sources()
   returns nothing for a directory that is not there, and a test that parses no
   C++ at all would pass every comparison against an empty set. The vacuity
   check below is the backstop; this is not needing it. */

function RepoRoot(): string {
  let At = dirname(fileURLToPath(import.meta.url));

  for(;;) {
    if(statSync(join(At, 'CMakeLists.txt'), { throwIfNoEntry: false })?.isFile())
      return At;

    const Up = dirname(At);

    if(Up === At)
      throw new Error('no CMakeLists.txt above ' + import.meta.url);

    At = Up;
  }
}

const Repo = RepoRoot();

function Sources(Dir: string): string[] {
  const Full = join(Repo, Dir);

  if(!statSync(Full, { throwIfNoEntry: false })?.isDirectory())
    return [];

  return readdirSync(Full, { recursive: true, encoding: 'utf8' })
    .filter((Name) => /\.(cpp|h)$/.test(Name))
    .map((Name) => join(Full, Name))
    .filter((Path) => statSync(Path).isFile());
}

interface Call {
  object: string;
  method: string;
  source: string;
}

/* An EM_JS body is brace-balanced JavaScript, so the block is taken by counting
   braces from the macro's opening one rather than by a regex that would stop at
   the first `}` inside an if. */

function Block(Text: string, From: number): string {
  const Open = Text.indexOf('{', From);

  if(Open === -1)
    return '';

  let Depth = 0;

  for(let At = Open; At < Text.length; At++) {
    if(Text[At] === '{')
      Depth++;
    else if(Text[At] === '}' && --Depth === 0)
      return Text.slice(Open, At + 1);
  }

  return Text.slice(Open);
}

function CallsIn(Path: string): Call[] {
  const Text = readFileSync(Path, 'utf8');
  const Source = relative(Repo, Path).replaceAll('\\', '/');
  const Found: Call[] = [];

  for(const Macro of Text.matchAll(/\bEM_JS\s*\(/g)) {
    const Body = Block(Text, Macro.index);

    /* Every bridge in the tree opens the same way: take the global, bail if the
       page has not defined it, then call one method on it. The local is read
       out of that assignment rather than assumed, so a block that names it
       something else is still parsed correctly. */
    const Bind = /(?:var|const|let)\s+(\w+)\s*=\s*globalThis\.(\w+)/.exec(Body);

    if(!Bind)
      continue;

    const [, Local, Object] = Bind as unknown as [string, string, string];

    for(const Use of Body.matchAll(new RegExp('\\b' + Local + '\\.(\\w+)\\s*\\(', 'g')))
      Found.push({ object: Object, method: Use[1] as string, source: Source });
  }

  return Found;
}

const Calls = SourceDirs.flatMap((Dir) => Sources(Dir).flatMap(CallsIn));

function Sorted(Names: Iterable<string>): string[] {
  return [...new Set(Names)].sort();
}

test('the C++ makes at least one bridge call, so the parse is not vacuous', () => {
  deepStrictEqual(Calls.length > 0, true);
});

test('every global the C++ reaches for is declared', () => {
  deepStrictEqual(Sorted(Calls.map((C) => C.object)), Sorted(Object.keys(Bridges)));
});

test('and every method on it, in both directions', () => {
  for(const [Name, Bridge] of Object.entries(Bridges)) {
    const Called = Sorted(Calls.filter((C) => C.object === Name).map((C) => C.method));

    deepStrictEqual(Called, Sorted(Bridge.methods),
                    Name + ': ' + Bridge.source + ' and contract.ts disagree');
  }
});

test('the declared source file is where the calls actually are', () => {
  for(const [Name, Bridge] of Object.entries(Bridges)) {
    const Where = Sorted(Calls.filter((C) => C.object === Name).map((C) => C.source));

    deepStrictEqual(Where, [Bridge.source]);
  }
});
