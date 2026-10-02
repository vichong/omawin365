# Third-party notices

OMAWIN365's own code, docs and artwork generator (`src/`, `tests/`, `tools/`, `docs/`) are MIT, © 2026 Vic Hong; see [`LICENSE`](LICENSE). Everything below belongs to someone else and keeps its own terms.

## Included in this repository

| What | Where | Author | Terms |
|---|---|---|---|
| Omarchy logo letterforms: the `o`, `m` and `a` pixel glyphs, reused for the matching `w i n 3 6 5` | `tools/logo.py`, `assets/logo.svg`, `assets/logo.txt`, `docs/banner.*` | From Omarchy's `logo.svg`, © David Heinemeier Hansson, <https://github.com/omacom/omarchy> | MIT (`licenses/omarchy-MIT.txt`) |
| Windows 365 mark: a pixel redrawing of its three stacked tiles in Microsoft's colours. Our "win 365" label, in the wordmark's glyphs, replaces the white 2×2 Windows squares | `tools/logo.py`, `assets/omawin365.svg`, `assets/logo.*`, `docs/banner.*` | Traced from [Windows365-logo.svg](https://commons.wikimedia.org/wiki/File:Windows365-logo.svg) on Wikimedia Commons, which credits Microsoft | Commons lists it as public domain (too simple for copyright) but **trademarked**. The design remains Microsoft's; see *Trademarks* below |

## Used at run time, not included

OMAWIN365 ships none of these. They come from your distribution's packages and keep their own licences and support policies.

| Component | How OMAWIN365 uses it | Licence (Arch package) |
|---|---|---|
| [Qt 6](https://www.qt.io/) (`qt6-base`, `qt6-wayland`) | Dynamically linked UI and networking library | LGPL-3.0-only (also GPL / commercial); you can swap in your own Qt build |
| [FreeRDP](https://www.freerdp.com/) (`freerdp`) | `xfreerdp3` runs as a separate process for the remote desktop and WebAuthn redirection | Apache-2.0 |
| [Chromium](https://www.chromium.org/) (`chromium`) | Runs as a separate process, in a private profile, for Microsoft sign-in and the connection-file download | BSD-3-Clause (plus bundled third-party terms) |
| libX11 (`libx11`) | Dynamically linked | MIT AND X11 |
| glibc `libutil` (`glibc`) | Dynamically linked | LGPL-2.1-or-later |
| [JetBrains Mono](https://www.jetbrains.com/lp/mono/) | Font named in `docs/banner.svg`; used to render `docs/banner.png`, not bundled | SIL OFL 1.1 |

## Trademarks and affiliation

Microsoft, Windows, Windows 365, Microsoft Entra and the Windows 365 logo are trademarks of the Microsoft group of companies. Omarchy is a project of David Heinemeier Hansson and contributors. The names, and the redrawn Windows 365 mark, are used only to identify what OMAWIN365 connects to and runs on.

OMAWIN365 is an independent community project. It is not affiliated with, endorsed by or supported by Microsoft or the Omarchy project. It is not Microsoft's Windows App and contains no Microsoft code. It talks to Microsoft's public sign-in and Windows 365 services as any client does. For Microsoft service or Cloud PC problems, ask your administrator. For problems with this app, open an issue here, not with Microsoft or Omarchy.
