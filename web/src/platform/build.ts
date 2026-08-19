/*
 * The two values esbuild substitutes into the bundle (build.mjs), read in a way
 * that also works when the source is run directly.
 *
 * `typeof` on an undeclared name is the one reference in JavaScript that does
 * not throw, and that is what makes this work in both worlds: esbuild replaces
 * the identifier with a string literal and folds the ternary away, while
 * node --test strips types and never runs esbuild, so it sees no such binding,
 * takes the fallback, and -- because an unqualified name still resolves through
 * globalThis -- lets a test set one. Without this, importing any module that
 * names a define is a ReferenceError before the first assertion.
 */

/* A crash report has to say something about its build even when git describe
   failed, so 'unknown' rather than ''. */

export function BuildId(): string {
  return typeof IVAN_BUILD_ID === 'undefined' ? 'unknown' : IVAN_BUILD_ID;
}

/* Empty means reports are kept locally and posted nowhere, which is the shipped
   default: no endpoint has ever been configured (docs/port-log.md §9.6). */

export function CrashEndpoint(): string {
  return typeof IVAN_CRASH_ENDPOINT === 'undefined' ? '' : IVAN_CRASH_ENDPOINT;
}
