# Security policy

OMAWIN365 handles Microsoft sign-in, security-key PIN prompts and a remote desktop connection, so security reports are very welcome.

## Reporting a vulnerability

**Please don't open a public issue for anything exploitable.** Use GitHub's private reporting instead: **Security → Report a vulnerability** on this repository. Include:

- what an attacker needs (a malicious `.rdpw` file, a hostile page inside the sign-in window, a network position, a local account),
- what they gain,
- the file and line, and ideally a failing test or a minimal synthetic reproduction.

Never include real credentials, PINs, tokens, callback URLs, connection files or tenant identifiers. Invented values are fine.

Expect an acknowledgement within a week. This is a volunteer project with no bug bounty. Confirmed issues get a fix with a regression test, and credit if you'd like it.

## Scope

In scope: the app (`src/`), its tests and the FreeRDP patch `packaging/freerdp/0003-accept-wildcard-dns-san.patch`. Same-user attackers (another process running as you) and bugs in Chromium, FreeRDP or Qt themselves are out of scope. Please report those upstream, though we're glad to hear how they affect this app. The [review brief](docs/SECURITY-REVIEW-BRIEF.md) lists the known, accepted risks.
