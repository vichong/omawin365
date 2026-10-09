# Contributing

Thanks for helping. Reviews, bug reports and focused fixes are all welcome.

## Reviews and findings

Start with the [security review brief](docs/SECURITY-REVIEW-BRIEF.md). Report anything exploitable privately ([SECURITY.md](SECURITY.md)); everything else can be a GitHub issue.

**AI-assisted reviews are welcome, with one rule: verify before you file.** Point to the exact code, explain the attacker's precondition, and confirm the behaviour yourself, ideally with a failing test or a synthetic reproduction. Please say which tool you used. Unverified AI output costs maintainers far more time to triage than it saves.

## Code changes

- Keep changes focused. Preserve the core rules: explicit Connect only, strict connection-file validation, private memory-backed sign-in storage, state/PKCE checks, and never answering a certificate prompt.
- For a bug, add a test that fails before your fix and passes after.
- Run `make test` (see [tests](docs/TESTING.md)).

## Never share

Real connection files, passwords, PINs, tokens, callback URLs, browser state or tenant identifiers, in issues, fixtures or logs. Use invented values. Don't weaken verification, change system trust or spend someone's security-key retries to reproduce a report without agreement.
