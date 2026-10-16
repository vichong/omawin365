# Development tests

```sh
make -j4
make test
```

Run from the source checkout. Seven suites exercise profiles, retained Session, public Session, Window, prompt parsing, BrowserAuth and the sign-in cache. QtTest totals include setup/cleanup; BrowserAuth reports assertions separately.

Additional prerequisites:

- Existing `/usr/bin/python3` with isolated `-I` mode for the synthetic Session child; Python is not an app runtime dependency.
- Accessible **existing X11/XWayland DISPLAY** for the public process suite. It creates no desktop window. Do not create/stop/alter someone else's display to satisfy tests without approval.
- Writable existing tmpfs fixture parent (such as the usual system temporary-memory locations), and a writable recognized disk-backed fixture parent (checkout, standard disk-backed temporary storage or the isolated desktop’s private home). Browser storage tests require both, and fail rather than silently skip missing prerequisites. No mounts are created.
- `libsecret` development headers and pkg-config for the sign-in cache.
- Qt Testlib, Widgets/Network, X11 development libraries, Linux PTY/process support and the Qt offscreen/SVG plugins. Window tests force offscreen mode and isolate their environment.

The synthetic child path is recorded by qmake; regenerate after moving the checkout. Test binaries alone are not standalone artifacts. Use fresh private build/evidence directories for sanitizer work; instrumented external library coverage is not implied.

Individual targets: `make test-session`, `make test-session-public`, `make test-window`. Other suites are built by `make test`. Process fixtures use a fake FreeRDP executable and synthetic profiles, not Microsoft authentication or hardware keys. The maintained composed BrowserAuth tests use explicit CDP page adoption/inert process/command-sink scaffolding rather than real Chromium dispatch.

Optional parser/profile fuzz harness sources are under `tools/security/`; see their [scope](../tools/security/README.md).

See [recorded verification](VERIFICATION.md) and [contributor rules](../CONTRIBUTING.md).

The sign-in cache suite uses an in-memory stand-in for the keyring, and the BrowserAuth cache checks point D-Bus at a missing address, so tests never touch your real keyring. They cover which cookies are kept, the three Remember sign-in choices, Forget racing a save or load, keyring failures and cancellation. Run just the BrowserAuth cache checks with `.build-tests/browser/browserauth_test --cache-only`. None of this shows that a tenant allows persistent sign-in; that needs a live test, and real cookies or tokens must never go into fixtures or logs.
