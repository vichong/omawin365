# Packaging

Local Arch packaging recipes. There is no published binary package yet.

## Two packages

- **`omawin365-freerdp`** ([`packaging/freerdp`](../packaging/freerdp)): FreeRDP 3.32.2 built with SSO MIB off, the camera client off and the WebAuthn channel on (upstream leaves it off), plus three patches. Windows opens several WebAuthn channels at once and 3.32.2 refuses all but one. `0001` and `0002` are upstream's fix ([FreeRDP#13781](https://github.com/FreeRDP/FreeRDP/pull/13781), merged 2026-10-19, not yet released), which lets the WebAuthn channel hold several connections. `0003` is ours: `0001` finds its per-plugin data by searching for a marker that starts with `*`, and a `*` byte in the plugin's own data made the search loop forever; `0003` always moves past it. Before `3.32.2-3`, security keys did not work inside the Cloud PC; `3.32.2-3` used our own patch, which `3.32.2-4` replaces with upstream's fix. It installs privately under `/usr/lib/omawin365/freerdp` and doesn't replace or conflict with Arch's `freerdp`. `check-contract.py` verifies the build configuration against `accepted-features.txt`.
- **`omawin365`** ([`packaging/app`](../packaging/app)): the app at `/usr/lib/omawin365/omawin365`, a `/usr/bin/omawin365` wrapper that puts the private FreeRDP first on the app's own `PATH`, the desktop entry, icon and licences.

The private FreeRDP package is temporary. The app currently accepts exactly FreeRDP 3.32.2 with SSO MIB off. 3.32.2 is the first release with the wildcard certificate fix ([FreeRDP#13653](https://github.com/FreeRDP/FreeRDP/pull/13653)). Stock 3.32.2 is in Arch and, as of 2026-10-19, both of Omarchy's mirrors, but it lacks the WebAuthn fix ([FreeRDP#13741](https://github.com/FreeRDP/FreeRDP/issues/13741)). That fix was merged on 2026-10-19 but isn't released yet; the provider carries it. Once Omarchy's mirrors carry a `freerdp` with that fix and the hang fix (patch `0003`), the package dependency moves to stock `freerdp`.

**Omarchy package.** Submitting to Omarchy's package repository waits for stock FreeRDP. FreeRDP 3.32.2 (released 2026-10-12) contains the wildcard-certificate fix ([FreeRDP#13653](https://github.com/FreeRDP/FreeRDP/pull/13653)) and [FreeRDP#13665](https://github.com/FreeRDP/FreeRDP/pull/13665), which fixes a regression on FreeRDP's development branch that could block WebAuthn channels. Arch's `freerdp` package has 3.32.2, but stock 3.32.2 also has the WebAuthn regression. Once Omarchy's mirrors carry a `freerdp` with that fixed upstream (including the hang fix in patch `0003`), the app drops `omawin365-freerdp` and ships as a single package.

## Building

```sh
# Provider (needs cmake); the subshell keeps you in the checkout:
(cd packaging/freerdp && makepkg)

# App, from a committed checkout; the output directory must be new and outside the checkout:
python3 packaging/app/prepare-snapshot.py "$PWD" /tmp/omawin365-pkg
(cd /tmp/omawin365-pkg && makepkg)
```

`prepare-snapshot.py` archives the committed `HEAD` (never working-tree edits) and writes a checksummed `PKGBUILD`. The `VERSION` file is the release version: at its `v<VERSION>` tag you get that release (`0.1.0-rc.2` → `pkgver=0.1.0rc2`); otherwise `pkgver=<release>.local<commit>`. `omawin365 --version` and About show the same version. A plain `make` from a git checkout shows `VERSION` at the release tag and `VERSION+git.<commit>` (plus `.dirty`) elsewhere.

More detail is in [`packaging/README.md`](../packaging/README.md).
