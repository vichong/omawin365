# What has been verified

Status as of November 15, 2026. "Verified" means observed or tested as described, not a security certification.

## Automated tests

`make test` runs seven suites (profiles, prompt parser, window, two session suites, browser automation and sign-in cache). They use synthetic connection files, a fake FreeRDP, synthetic Chromium and an in-memory Secret Service adapter; no Microsoft account, real profile, personal keyring or security key is involved. For v0.2.0 all seven suites pass: 4,224 QtTest checks and 244 browser-automation assertions. The original six suites passed under AddressSanitizer and UndefinedBehaviorSanitizer with leak detection; for remembered sign-in, the sign-in cache, browser cache and window suites were also run that way, with no findings. The static-analysis results predate remembered sign-in.

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

In-session security keys were rechecked on October 20 with provider `3.32.2-4` (upstream's WebAuthn fix plus our hang fix, replacing our own patch): several sign-ins inside the Cloud PC, each with the OMAWIN365 PIN dialog and a touch.

Session settings were checked live on October 20 (rc.6, Omarchy 4.0.6): remote audio playback, microphone forwarding and display scale all worked.

Provider `3.33.1-1` (plain FreeRDP 3.33.1) was checked live on November 15: sign-in and desktop, remote audio playback, and the in-session security key (OMAWIN365 PIN dialog, then touch). `xfreerdp` averaged about 3% CPU over the first eight minutes. Cancelling from the PIN dialog and unplugging the key twice during in-session sign-ins left the app responsive, with no hang or crash. Not yet rechecked with `3.33.1-1`: a wrong PIN, the microphone, clipboard, resizing, and keyboard modifiers after Hyprland shortcuts.

Arch's stock `freerdp` 3.32.2-1, without the provider, was checked live on November 15: sign-in, desktop, microphone forwarding, and one in-session security-key sign-in, which worked. No camera appeared in the Cloud PC, so the channel reset that keeps the stock build's camera client off held. 3.32.2 still lacks the WebAuthn fix, so in-session keys there may fall back to Windows' own prompt ([FreeRDP#13741](https://github.com/FreeRDP/FreeRDP/issues/13741)); the provider remains the reliable path.

Not yet checked: other tenants, Frontline and other editions, multi-monitor.

## Remembered sign-in (October 2026)

- **Automated:** the sign-in cache suite and the BrowserAuth and Window checks described in [TESTING](TESTING.md), with an in-memory keyring stand-in.
- **Smoke checks by the contributor, with invented cookies only:** a real private Secret Service saved, reloaded and deleted an item; real Chromium restored and exported a Microsoft-domain cookie with its expiry, Secure, HttpOnly and SameSite attributes intact.
- **Live, by the contributor on their tenant:** sign in, disconnect, then reconnect without quitting reused the sign-in. This was before the three-way setting; that behaviour is now **Until the app closes**, which **On this device** also includes.

- **Live, by the maintainer on their tenant (October 16):** with **On this device**, sign-in was reused after quitting and reopening the app; **Until the app closes** reused it on Reconnect and asked again after a restart; **Forget sign-in** turned remembering off and the next connection asked to sign in.

Not yet checked: Conditional Access differences, federated sign-in, and keyring providers other than the maintainer's.
