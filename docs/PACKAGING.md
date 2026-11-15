# Packaging

Local Arch packaging recipes. There is no published binary package yet.

## Two packages

- **`omawin365-freerdp`** ([`packaging/freerdp`](../packaging/freerdp)): plain FreeRDP 3.33.1, with no patches, built with SSO MIB off, the camera client off and the WebAuthn channel on (upstream leaves it off), for security keys inside the Cloud PC. 3.32.2 refused all but one of the WebAuthn channels Windows opens at once ([FreeRDP#13741](https://github.com/FreeRDP/FreeRDP/issues/13741)); `3.32.2-4` carried upstream's fix ([FreeRDP#13781](https://github.com/FreeRDP/FreeRDP/pull/13781)) plus our fix for a hang in it, which upstream fixed as [FreeRDP#13815](https://github.com/FreeRDP/FreeRDP/pull/13815). Both are in FreeRDP 3.33.0 and later. It installs privately under `/usr/lib/omawin365/freerdp` and doesn't replace or conflict with Arch's `freerdp`. `check-contract.py` verifies the build configuration against `accepted-features.txt`.
- **`omawin365`** ([`packaging/app`](../packaging/app)): the app at `/usr/lib/omawin365/omawin365`, a `/usr/bin/omawin365` wrapper that puts the private FreeRDP first on the app's own `PATH`, the desktop entry, icon and licences.

The private FreeRDP package is temporary. The app currently accepts exactly FreeRDP 3.33.1 with SSO MIB off. 3.33.0 was the first release with the WebAuthn fix; 3.33.1 fixes regressions in 3.33.0. Stock `freerdp` 3.33.1 is in Arch, but as of 2026-11-15 both of Omarchy's mirrors still serve 3.32.2, which lacks the WebAuthn fix. Once Omarchy's mirrors carry 3.33.1 or later and a live in-session security-key test passes with it, the package dependency moves to stock `freerdp`.

**Omarchy package.** Submitting to Omarchy's package repository waits for the same thing: once Omarchy's mirrors carry `freerdp` 3.33.1 or later and a live in-session security-key test passes with it, the app drops `omawin365-freerdp` and ships as a single package.

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
