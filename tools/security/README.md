# Optional synthetic fuzz tooling

These harnesses exercise production PromptParser and ProfileStore import APIs with synthetic data. The runner builds fresh ASan/UBSan/libFuzzer fixtures and records bounded budgets, seeds and source hashes; it does not launch the app, Chromium or FreeRDP.

Review the runner before use and choose a fresh private evidence directory. Never replace synthetic corpus inputs with real credentials/profiles. External Qt/system libraries are not instrumented, and an uneventful bounded run is not security certification. Any failure needs reproduction and attribution before a production fix.

No fuzz campaign ran during public snapshot preparation. See [development tests](../../docs/TESTING.md).
