# Security review brief

Thank you for reviewing OMAWIN365. This page tells you what the app does, where the risk is, what has already been checked, and what we'd most like a human to look at. It should take about 10 minutes to read; the review itself is up to you.

## What it is

A native Qt 6 desktop app (about 6,000 lines of C++ plus one small JavaScript file) that connects Linux users to a Windows 365 Cloud PC. It does three security-sensitive things:

1. **Sign-in.** It starts a private Chromium window, drives Microsoft sign-in over the Chrome DevTools Protocol (anonymous pipes, not a TCP port), and captures the OAuth callback.
2. **Connection files.** It downloads or imports `.rdpw` connection files, validates them against a strict allowlist, and stores private copies.
3. **Remote desktop.** It runs FreeRDP 3.32.1 (one small patch) in a private terminal, answers its sign-in prompts, and relays security-key PIN prompts from a native dialog.

It was written with AI coding agents. It has had extensive AI review but **no human security review yet**. That's why you're here.

## Build and test

On Arch/Omarchy with Qt 6.11, Chromium and XWayland:

```sh
make -j4 && make test      # six suites, about 4 minutes; needs an X11/XWayland DISPLAY
```

Running against a real Cloud PC also needs the private FreeRDP provider: see `packaging/freerdp` and [README](../README.md#build). You don't need a Windows 365 account to review the code or run the tests.

## Where to look first

| Area | Files | Why it matters |
| --- | --- | --- |
| Sign-in browser control | `src/browserauth.cpp`, `src/portal.js`, `src/oauthcontract.cpp` | Handles OAuth state/PKCE and callbacks, drives a hostile-content browser, accepts one download. |
| FreeRDP process and prompts | `src/session.cpp`, `src/promptparser.cpp` | Parses untrusted terminal output, sends PINs and sign-in replies, manages process lifetime and signals. |
| Connection-file validation | `src/rdpprofile.cpp`, `src/profilestore.cpp` | The only barrier between an attacker-supplied `.rdpw` and FreeRDP's settings. |
| FreeRDP patch | `packaging/freerdp/0003-accept-wildcard-dns-san.patch` | Changes certificate name extraction; merged upstream as [FreeRDP#13653](https://github.com/FreeRDP/FreeRDP/pull/13653). |

The enforced rules are summarized in [BOUNDARIES.md](BOUNDARIES.md).

## Questions we most want answered

1. Can a malicious `.rdpw` file, or a malicious page inside the sign-in window, get FreeRDP to connect somewhere unintended, enable local device or file redirection, or leak a token?
2. Is the OAuth flow sound: state and PKCE binding, callback capture, and replay or staleness across sign-in rounds?
3. Can untrusted FreeRDP output make the app send a PIN or sign-in reply to the wrong prompt, or send it twice?
4. Does the certificate handling ever accept a certificate it shouldn't? The app never answers trust prompts; FreeRDP verifies against the system CA store.
5. Do sign-in cookies, tokens or PINs end up anywhere outside the private, memory-backed stores?

## Already checked

- Independent AI reviews (Claude Code CLI and Codex), a written threat model, and a findings ledger with a verdict and evidence for every item: summary in [VERIFICATION.md](VERIFICATION.md).
- Seven confirmed findings fixed with failing-then-passing tests, including an IP-literal bypass of the hostname rule and a foreign-frame download substitution.
- All suites pass under ASan and UBSan with leak detection; clang static analyzer clean; bounded fuzzing of the prompt parser and file import.
- An `strace` of a live session: file, process and network activity. It led to keeping Chromium crash data private and blocking Chromium's Google background services.

## Known and accepted risks

- **Same-user attackers are out of scope.** A process running as the same user can already read the 0600 stores or swap files between validation and use.
- **Chromium and FreeRDP are trusted dependencies.** Two Chromium background requests to Google remain on purpose (`accounts.google.com`, `www.google.com`), so Google-federated sign-in keeps working.
- **No cleanup guarantee after a crash or kill.** Leftovers stay in the user's memory-backed runtime directory until logout; swap is not covered.
- **The app icon is a redrawn Windows 365 mark**, used under the trademark notice in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## Reporting

Open a GitHub issue for anything non-sensitive. For anything exploitable, use GitHub's private vulnerability reporting ([SECURITY.md](../SECURITY.md)). Severity estimates and suggested fixes are welcome but not required.
