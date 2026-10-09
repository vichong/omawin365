# What has been verified

Status as of October 2026. "Verified" means observed or tested as described, not a security certification.

## Automated tests

`make test` runs six suites (profiles, prompt parser, window, two session suites, browser automation): about 4,100 QtTest checks and 215 browser-automation assertions, all passing. They use synthetic connection files, a fake FreeRDP and synthetic Chromium; no Microsoft account, real profile or security key is involved. All six also pass under AddressSanitizer and UndefinedBehaviorSanitizer with leak detection.

## Static and dynamic analysis

- Clang static analyzer (`scan-build`: core, unix, security, C++ and dead-code checkers) over every source file: no findings. clang-tidy findings are reviewed and dispositioned.
- Bounded fuzzing of the prompt parser and connection-file import (`tools/security/`).
- An `strace` of a live sign-in and connection (file, process and network calls only). It found Chromium keeping crash data in the user's real config directory and still contacting Google background services. Both were fixed.

## Security review so far

A threat model and per-area reviews produced a ledger of 23 findings plus four deferred lifecycle items. Each was reproduced and fixed with a failing-then-passing test, closed with evidence, or accepted with a stated reason. Fixed issues include:

- an IP-literal bypass of the hostname-only rule (`2130706433`, `0x7f.0.0.1`),
- a foreign frame substituting the portal's connection-file download,
- portal automation running in a replacement page,
- signals that could reach a recycled process ID,
- a stale-write race between two app instances,
- a FIFO that could hang the UI.

A release-gate audit of the ten design invariants found two more problems: a debug-only raw FreeRDP log reachable in release builds, and a browser process-group signalling gap. Both are fixed. All of these were AI-assisted reviews (Claude Code and Codex), with fixes re-reviewed until no findings remained. **No independent human review has happened yet.** That's the main open item; see the [review brief](SECURITY-REVIEW-BRIEF.md).

## Live checks

On the maintainer's Windows 365 Enterprise tenant, with a hardware security key:

- automatic connection-file download from the portal, sign-in, desktop,
- in-session PIN and touch,
- disconnect and reconnect,
- one wrong PIN followed by the correct one,
- cancelling during sign-in, and unplugging the key during sign-in.

Not yet checked: other tenants, Frontline and other editions, multi-monitor.
