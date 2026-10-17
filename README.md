# OMAWIN365

![OMAWIN365](docs/banner.png)

**Pre-release. Looking for reviewers.** OMAWIN365 is a native Qt launcher for Windows 365 Cloud PCs on Omarchy/Arch. It signs in to Microsoft in a private Chromium window and opens your Cloud PC with FreeRDP in a separate window. Importing a connection never connects automatically.

**Why it exists: your security key works *inside* the Cloud PC.** Signing in to Windows 365 with a FIDO2 security key works in supported browsers, Omarchy included. But in my testing the browser-based client couldn't redirect a local key into the remote session, so sign-ins *inside* the Cloud PC (Microsoft 365, admin portals, websites) can't use it. OMAWIN365 uses FreeRDP's WebAuthn redirection: when something in the session asks for your key, a native PIN prompt appears on your Omarchy desktop, and you touch your local key. No raw USB passthrough is involved.

It works end to end with Windows 365 Enterprise on the maintainer's tenant, including:

- downloading the connection file from the portal,
- Microsoft sign-in with a hardware security key,
- in-session security-key sign-ins (native PIN prompt, then touch),
- reconnect, cancel and wrong-PIN recovery.

Other tenants and editions are untested. It has had extensive AI-assisted review but **no independent human security review yet**. Please [help review it](#help-review-it) before trusting it with a work account.

Not affiliated with, endorsed by or supported by Microsoft or Omarchy. Microsoft, Windows, Windows 365 and the redrawn Windows 365 mark are Microsoft trademarks, used here only for identification. Provided **as-is, without warranty**, under the [MIT license](LICENSE); see [third-party notices](THIRD_PARTY_NOTICES.md).

## Build

On Omarchy/Arch with Qt 6.11 (`qt6-base`, `qt6-wayland`, `qt6-svg`), a C++20 compiler, `libx11`, `libsecret` and `fontconfig`. Chromium and XWayland are needed at run time.

```sh
make -j4
make test   # needs an X11/XWayland DISPLAY; see docs/TESTING.md
./.build/omawin365
```

The app needs **FreeRDP 3.32.2 built with `WITH_SSO_MIB=OFF`**. Stock `freerdp` 3.32.2 is in Arch and, as of 2026-10-17, Omarchy's rc mirror (the stable mirror still has 3.31.1, which the app rejects), but it refuses Windows' overlapping WebAuthn channels, so security keys don't work inside the Cloud PC. The upstream fix is in FreeRDP's backlog ([FreeRDP#13741](https://github.com/FreeRDP/FreeRDP/issues/13741)). Until it ships, the recipe in [`packaging/freerdp`](packaging/freerdp) builds a private, patched copy under `/usr/lib/omawin365/freerdp` without touching the distro package; see [packaging](docs/PACKAGING.md).

**Why 3.32.2:** Microsoft's gateways use wildcard certificates (`*.wvd.microsoft.com`). FreeRDP 3.32.1 and older, when built without uriparser, discard wildcard names, so valid gateways look like a name mismatch. OMAWIN365 never accepts a certificate prompt, so every connection stopped. FreeRDP 3.32.2 fixes this ([FreeRDP#13653](https://github.com/FreeRDP/FreeRDP/pull/13653)). Once Omarchy's mirrors carry a stock `freerdp` with the WebAuthn fix too, the package dependency will move to stock `freerdp`, and the private copy goes away.

## Session settings

Settings apply on the next Connect or Reconnect; a running session keeps the settings it started with, including the microphone. Remember sign-in can be changed only while disconnected.

- **Display scale** sets the size of Windows text and icons for every Cloud PC. **Auto** follows the focused Omarchy monitor's scale.
- **Play remote audio**, **Forward microphone** and **Share clipboard** are saved for the selected Cloud PC. Audio and microphone are off until you turn them on and use your default local devices. The clipboard carries text and images, never files. Your organization's policy can still block any of them.
- **Remember sign-in** is for every Cloud PC and is **Off** by default: nothing is saved, and the private sign-in browser forgets everything when the app closes.
  - **Until the app closes** keeps Microsoft's sign-in cookies in memory, so Reconnect doesn't ask again.
  - **On this device (keyring)** also saves Microsoft's long-lived sign-in cookies in your system keyring (Secret Service), so sign-in can survive a restart. Accept Microsoft's **Stay signed in?** prompt to get them. Anything that can read your unlocked keyring can use them until they expire.
  - Either way, Microsoft and your organization's policy decide when to ask again, including for your security key. Only cookies for `login.microsoftonline.com` are kept: never passwords, PINs or tokens.
  - **Forget sign-in** clears memory and the keyring and turns remembering off. It doesn't sign you out of Microsoft.

## Help review it

The [security review brief](docs/SECURITY-REVIEW-BRIEF.md) explains what the app does, where the risk is and the five questions we most want answered. It takes about 10 minutes to read. Human reviews and AI-assisted reviews are both welcome; please read [contributing](CONTRIBUTING.md) first. Report anything exploitable privately: see [SECURITY.md](SECURITY.md).

## More

- [Architecture and boundaries](docs/BOUNDARIES.md)
- [Tests](docs/TESTING.md)
- [What has been verified](docs/VERIFICATION.md)
- [Packaging](docs/PACKAGING.md)

Built with AI coding agents (Claude Code and Codex), directed and tested by the maintainer. Never include credentials, PINs, callback URLs, tokens or real connection files in an issue.
