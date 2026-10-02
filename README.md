# OMAWIN365

![OMAWIN365](docs/banner.png)

**Experimental source for review and contribution. No supported installation or release yet; not recommended for work-account use.** Local packaging prototypes are included for inspection, not installation advice. The banner's Enterprise testing refers to historical builds, not current live acceptance.

A native Qt launcher for Windows 365 Cloud PCs on Omarchy/Arch, using stock FreeRDP and a private Chromium sign-in window. FreeRDP opens a separate X11 desktop through XWayland; it is not embedded in the launcher. Importing a connection never connects automatically.

Not affiliated with, endorsed by or supported by Microsoft or Omarchy. Microsoft, Windows, Windows 365 and the redrawn Windows 365 mark are Microsoft trademarks used for identification. This independent community project is provided **as-is, without warranty**, under the [MIT license](LICENSE); see [third-party notices](THIRD_PARTY_NOTICES.md).

## Development build

Existing prerequisites: a Linux/Omarchy desktop, Qt 6.11 development packages (`qt6-base`, `qt6-wayland`, `qt6-svg`), a C++20 compiler, make/qmake6, libX11 and fontconfig. Chromium and XWayland are runtime dependencies. The app itself needs no CMake, Electron or Python runtime.

```sh
make -j4
# With the additional test prerequisites below:
make test
```

A development executable is produced at `.build/omawin365`. **Do not treat these commands as a supported end-user installation recipe.** Transport accepts only stock FreeRDP **3.32.1 with `WITH_SSO_MIB=OFF`**, using the modern state/S256 console-callback contract. Distro FreeRDP 3.31.1 is rejected. Version/buildconfig self-reports do not prove provenance or the required feature configuration. There is no automatic upgrade or fallback; supported provisioning/deployed validation remains pending.

Browser authentication requires an existing absolute, user-owned **0700 tmpfs** `XDG_RUNTIME_DIR`. Disk-backed storage is rejected; tmpfs may swap. Long custom runtime paths can exceed Chromium's socket limit. The Arch Chromium executable is expected at `/usr/lib/chromium/chromium`. Do not sign in with work credentials merely to test this source snapshot.

## Review and contribution

- [Architecture and boundaries](docs/BOUNDARIES.md)
- [Tests and prerequisites](docs/TESTING.md)
- [Verification summary and remaining gates](docs/VERIFICATION.md)
- [Contributing](CONTRIBUTING.md)
- [Packaging preparation status](docs/PACKAGING.md)
- [Content manifest](PUBLICATION-MANIFEST.json)

Source repository: [vichong/omawin365](https://github.com/vichong/omawin365).

Built with AI coding agents, directed by the maintainer. AI-assisted reviews are **not independent human security review or security certification**. Practical independent human feedback is still needed. Never submit credentials, PINs, callback URLs, tokens or real connection files with a report.
