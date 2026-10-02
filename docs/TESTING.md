# Development tests

```sh
make -j4
make test
```

Run from the source checkout. Six suites exercise profiles, retained Session, public Session, Window, prompt parsing and BrowserAuth. QtTest totals include setup/cleanup; BrowserAuth reports assertions separately.

Additional prerequisites:

- Existing `/usr/bin/python3` with isolated `-I` mode for the synthetic Session child; Python is not an app runtime dependency.
- Accessible **existing X11/XWayland DISPLAY** for the public process suite. It creates no desktop window. Do not create/stop/alter someone else's display to satisfy tests without approval.
- Writable existing tmpfs fixture parent (such as the usual system temporary-memory locations), and a writable recognized disk-backed fixture parent (checkout or standard disk-backed temporary storage). Browser storage tests require both, and fail rather than silently skip missing prerequisites. No mounts are created.
- Qt Testlib, Widgets/Network, X11 development libraries, Linux PTY/process support and the Qt offscreen/SVG plugins. Window tests force offscreen mode and isolate their environment.

The synthetic child path is recorded by qmake; regenerate after moving the checkout. Test binaries alone are not standalone artifacts. Use fresh private build/evidence directories for sanitizer work; instrumented external library coverage is not implied.

Individual targets: `make test-session`, `make test-session-public`, `make test-window`. Other suites are built by `make test`. Process fixtures use a fake FreeRDP executable and synthetic profiles, not Microsoft authentication or hardware keys. The maintained composed BrowserAuth tests use explicit CDP page adoption/inert process/command-sink scaffolding rather than real Chromium dispatch.

Optional parser/profile fuzz harness sources are under `tools/security/`; see their [scope](../tools/security/README.md). Do not execute broader campaigns or capture a real session simply to reproduce the verification summary. No tests were run during snapshot curation.

See [recorded verification](VERIFICATION.md) and [contributor rules](../CONTRIBUTING.md).
