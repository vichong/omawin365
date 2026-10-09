# Packaging

Local Arch packaging recipes. There is no published binary package yet.

## Two packages

- **`omawin365-freerdp`** ([`packaging/freerdp`](../packaging/freerdp)): FreeRDP 3.32.1 built with SSO MIB off and the camera client off, plus [patch 0003](../packaging/freerdp/0003-accept-wildcard-dns-san.patch) for wildcard certificate names. It installs privately under `/usr/lib/omawin365/freerdp` and doesn't replace or conflict with Arch's `freerdp`. `check-contract.py` verifies the build configuration against `accepted-features.txt`.
- **`omawin365`** ([`packaging/app`](../packaging/app)): the app at `/usr/lib/omawin365/omawin365`, a `/usr/bin/omawin365` wrapper that puts the private FreeRDP first on the app's own `PATH`, the desktop entry, icon and licences.

The private FreeRDP package is temporary. The app currently accepts exactly FreeRDP 3.32.1 with SSO MIB off. The fix was merged upstream as [FreeRDP#13653](https://github.com/FreeRDP/FreeRDP/pull/13653) but is not yet in a FreeRDP release. Once Arch ships a fixed FreeRDP, that version check and the package dependency will move to stock `freerdp`.

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
