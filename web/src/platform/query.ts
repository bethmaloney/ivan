/*
 * The query string, in one place.
 *
 * Every one of the page's modules is switched by the query string rather than
 * at build time -- ?sfx=off, ?musicbase=, ?saves=off, ?wipesaves, ?record=off,
 * ?crashlog= -- because they have to be changeable without a rebuild of an 8MB
 * wasm binary. web/README.md lists them all.
 *
 * Read at call time rather than cached at module load. The string cannot change
 * without a reload, so the only thing caching would buy is a few microseconds
 * at boot, and it would cost the tests the ability to set up a second world.
 */

interface MaybeLocated {
  location?: { search?: string } | undefined;
}

function Params(): URLSearchParams {
  return new URLSearchParams((globalThis as MaybeLocated).location?.search ?? '');
}

/* `?name=off` is the off switch across the whole page, and absence means on:
   every one of these features is on by default and opted out of. */

export function Enabled(Name: string): boolean {
  return Params().get(Name) !== 'off';
}

/* A value, or null when the option is absent -- ?musicbase=<url>, ?crashlog=<url>. */

export function Setting(Name: string): string | null {
  return Params().get(Name);
}

/* Present with no value at all, as ?wipesaves is. `?wipesaves=` counts, because
   a trailing = is a typo rather than a retraction. */

export function Present(Name: string): boolean {
  return Params().has(Name);
}

/* Every option in the URL, in the order it gave them, which is the order
   harness::ParseArgs sees once argv is built out of them. The one caller that
   needs the whole string rather than a named option -- and the reason this is
   here rather than a second URLSearchParams in harness/argv.ts. */

export function All(): [string, string][] {
  return Array.from(Params());
}
