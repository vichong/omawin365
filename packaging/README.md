# Packaging recipes

Local Arch packaging for OMAWIN365. See [docs/PACKAGING.md](../docs/PACKAGING.md) for the overview and build commands.

## `freerdp/`: private FreeRDP provider

- **Source:** FreeRDP commit `65bcca1913a28cbdd7eb913599f692bd2b707991` (3.33.1), from a pinned GitHub archive with a fixed SHA-256. It already keeps wildcard certificate names such as `*.wvd.microsoft.com` ([FreeRDP#13653](https://github.com/FreeRDP/FreeRDP/pull/13653)).
- **Patches:** none. The WebAuthn fix ([FreeRDP#13781](https://github.com/FreeRDP/FreeRDP/pull/13781)) and its hang fix ([FreeRDP#13815](https://github.com/FreeRDP/FreeRDP/pull/13815)) are in 3.33.0 and later, so the former patches `0001`-`0003` were dropped. See [docs/PACKAGING.md](../docs/PACKAGING.md#two-packages).
- **Configuration** (`configure.sh`): SSO MIB off, WebAuthn channel on (upstream default is off; needs `libfido2` and `libcbor`), camera client off, X11 and Azure AD sign-in on, private prefix `/usr/lib/omawin365/freerdp`, and git version detection off so the build reports `3.33.1 (n/a)`.
- **Checks:** `check-contract.py` fails the build if the CMake cache drifts from the 235 feature flags in `accepted-features.txt` or from the version and shared-library settings.
- **Install layout:** a complete private FreeRDP under `/usr/lib/omawin365/freerdp`, with `bin/xfreerdp3` linking to `xfreerdp`. It doesn't provide, replace or conflict with Arch's `freerdp`, and adds no global links. Upstream's Apache-2.0 licence and the bundled uwac notices are installed.

## `app/`: application

- `PKGBUILD.in` is a template; `prepare-snapshot.py` turns it into a real recipe:

  ```sh
  python3 packaging/app/prepare-snapshot.py "$PWD" /new/empty/dir
  ```

  It archives the **committed** `HEAD` (never working-tree edits), reads the template and wrapper from that same commit, and writes `PKGBUILD`, the source archive, the wrapper and `snapshot.json` with their checksums. The output directory must be new and outside the checkout.
- The version comes from the `VERSION` file. At its `v<VERSION>` tag the package is that release (`0.1.0-rc.2` gives `pkgver=0.1.0rc2`). Any other commit gives `pkgver=<release>.local<commit>`. The build passes the version to qmake as `OMAWIN365_VERSION`, which `omawin365 --version` and About show.
- The build passes makepkg's `CFLAGS`, `CXXFLAGS` and `LDFLAGS` to qmake. `check()` validates the desktop entry only; run `make test` from a checkout for the full suites.
- **Install layout:** `/usr/lib/omawin365/omawin365`, plus the `/usr/bin/omawin365` wrapper (`omawin365-launcher`). The wrapper puts the private FreeRDP first on the app's own `PATH`, without `LD_LIBRARY_PATH`, and refuses to start if the provider is missing. Also installed: the desktop entry, the icon, the MIT licence, third-party notices and the Omarchy wordmark attribution.

An older user-local install (`~/.local/bin/omawin365` or a user desktop entry) can shadow the packaged one; remove it when switching.
