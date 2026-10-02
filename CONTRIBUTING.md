# Contributing

This snapshot is experimental source for inspection, not a supported installation or a recommendation to use work credentials. Start with the [boundaries](docs/BOUNDARIES.md), [tests](docs/TESTING.md) and [verification limits](docs/VERIFICATION.md).

Keep changes focused; preserve explicit Connect, strict profile validation, private temporary storage, state/S256 correlation and fail-closed certificate handling. Add synthetic regressions for reproduced defects before changing behavior. Avoid speculative compatibility/security workarounds.

Never include real profiles, passwords, PINs, tokens, callback URLs, browser state or identifying tenant data in issues, fixtures or logs. Use invented identifiers and review diagnostic output before sharing. Do not disable verification, alter system trust, install dependencies, stop unrelated processes or spend hardware-key retries to reproduce a report without explicit agreement.

Independent human review and constructive feedback are welcome. Use synthetic, redacted examples in reports. AI reviews and synthetic tests are useful evidence, not certification.
