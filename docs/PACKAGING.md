# Packaging

Local Arch packaging recipes, plus the recipe for Omarchy's package repository (OPR) in [`packaging/opr/omawin365`](../packaging/opr/omawin365), which builds a tagged release. v0.2.0 is the first release that is not a pre-release, and the one to be submitted to OPR.

## Two packages

- **`omawin365-freerdp`** ([`packaging/freerdp`](../packaging/freerdp)), optional: plain FreeRDP 3.33.1, with no patches, built with SSO MIB off, the camera client off and the WebAuthn channel on (upstream leaves it off), for security keys inside the Cloud PC. 3.32.2 refused all but one of the WebAuthn channels Windows opens at once ([FreeRDP#13741](https://github.com/FreeRDP/FreeRDP/issues/13741)); `3.32.2-4` carried upstream's fix ([FreeRDP#13781](https://github.com/FreeRDP/FreeRDP/pull/13781)) plus our fix for a hang in it, which upstream fixed as [FreeRDP#13815](https://github.com/FreeRDP/FreeRDP/pull/13815). Both are in FreeRDP 3.33.0 and later. It installs privately under `/usr/lib/omawin365/freerdp` and doesn't replace or conflict with Arch's `freerdp`. `check-contract.py` verifies the build configuration against `accepted-features.txt`.
- **`omawin365`** ([`packaging/app`](../packaging/app)): the app at `/usr/lib/omawin365/omawin365`, a `/usr/bin/omawin365` wrapper that puts the private FreeRDP first on the app's own `PATH` when it is installed (otherwise `/usr/bin`, for the distro's `xfreerdp3`), the desktop entry, icon and licences. It depends on `freerdp>=2:3.32.2` and lists `omawin365-freerdp` as optional.

The app accepts the distro's FreeRDP 3.32.2, or 3.33.1 and later 3.33 releases, with SSO MIB and the external sign-in helpers off ([boundaries](BOUNDARIES.md)). 3.33.0 was the first release with the WebAuthn fix; 3.33.1 fixes regressions in 3.33.0. Stock `freerdp` 3.33.1 is in Arch, but as of 2026-11-15 both of Omarchy's mirrors still serve 3.32.2, which lacks the WebAuthn fix: the app connects, but security keys inside the Cloud PC may not work. Install both packages for reliable in-session keys until Omarchy's mirrors carry 3.33.1 or later; then the provider package can be retired.

## Building

```sh
# Optional provider (needs cmake); the subshell keeps you in the checkout:
(cd packaging/freerdp && makepkg)

# App, from a committed checkout; the output directory must be new and outside the checkout:
python3 packaging/app/prepare-snapshot.py "$PWD" /tmp/omawin365-pkg
(cd /tmp/omawin365-pkg && makepkg)
```

`prepare-snapshot.py` archives the committed `HEAD` (never working-tree edits) and writes a checksummed `PKGBUILD`. The `VERSION` file is the release version: at its `v<VERSION>` tag you get that release (`0.1.0-rc.2` → `pkgver=0.1.0rc2`); otherwise `pkgver=<release>.local<commit>`. `omawin365 --version` and About show the same version. A plain `make` from a git checkout shows `VERSION` at the release tag and `VERSION+git.<commit>` (plus `.dirty`) elsewhere.

More detail is in [`packaging/README.md`](../packaging/README.md).
