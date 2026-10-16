# What has been verified

Status as of October 2026. "Verified" means observed or tested as described, not a security certification.

## Automated tests

`make test` runs seven suites (profiles, prompt parser, window, two session suites, browser automation and sign-in cache). They use synthetic connection files, a fake FreeRDP, synthetic Chromium and an in-memory Secret Service adapter; no Microsoft account, real profile, personal keyring or security key is involved. The original six-suite baseline also passed under AddressSanitizer and UndefinedBehaviorSanitizer with leak detection. Those sanitizer and static-analysis results predate the remembered-sign-in change; its checks are recorded below.

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

## Remembered sign-in (October 2026)

- **Automated:** the sign-in cache suite and the BrowserAuth and Window checks described in [TESTING](TESTING.md), with an in-memory keyring stand-in.
- **Smoke checks by the contributor, with invented cookies only:** a real private Secret Service saved, reloaded and deleted an item; real Chromium restored and exported a Microsoft-domain cookie with its expiry, Secure, HttpOnly and SameSite attributes intact.
- **Live, by the contributor on their tenant:** sign in, disconnect, then reconnect without quitting reused the sign-in. This was before the three-way setting; that behaviour is now **Until the app closes**, which **On this device** also includes.

- **Live, by the maintainer on their tenant (October 16):** with **On this device**, sign-in was reused after quitting and reopening the app; **Until the app closes** reused it on Reconnect and asked again after a restart; **Forget sign-in** turned remembering off and the next connection asked to sign in.

Not yet checked: Conditional Access differences, federated sign-in, and keyring providers other than the maintainer's.
