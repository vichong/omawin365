# Verification summary

**Accepted bounded offline code checkpoint:** fail-closed certificate implementation and tightened regressions passed independent ordinary and ASan/UBSan verification: **1,830 QtTest passes +205 BrowserAuth assertions per mode**, zero failures/skips/sanitizer diagnostics. Leak detection was enabled without suppressions. A fresh baseline app/all six suites was built; after oracle tightening the two affected Session suites were rebuilt, unchanged app/other binaries were hash-verified and all six suites rerun. Ten production objects/seven binaries were instrumented; Qt/system libraries and interpreted fixtures were not. The verifier built, but did not launch, the application.

Official AI test-review follow-up closed five test-oracle Low findings. These source/review results are not independent human security review, universal race/lifetime assurance or release certification. Generic reentrancy/process/input-order limits remain; no arbitrary signal-receiver execution-time guarantee is claimed.

Separate completed offline evidence includes:

- A private stock FreeRDP 3.32.1/SSO-OFF candidate with camera client OFF, source/configuration provenance and selected parser/redirection-consumer checks. It is not a supported deployment package.
- A real Chromium → production typed Session → real **synthetic** PTY bridge: two exact replies, generation/replay rejection, expected-failure guard sensitivity and sampled process cleanup. Explicit headless, blank-page adoption, null-display and receipt-counter seams bypass full app/portal startup. No stock client TLS, devices or real accounts ran in that bridge; sampled cleanup is not exhaustive descendant coverage.
- Synthetic profile, parser, UI and storage regressions. Storage checks and blank-browser smoke do not establish every credential API, outside-tree write, future Chromium version, swap behavior or abnormal-exit cleanup.

Historical builds reached Windows 365 Enterprise desktops and user-confirmed local security-key PIN/touch and selected recovery scenarios. Those observations **do not validate the current strict authentication/import/certificate changes**, the new packaging route, every tenant or Frontline. Current live desktop/authentication, resize/input/PIN-focus and relevant recovery acceptance remain pending. Wrong-PIN behavior remains unverified; no retry allowance should be spent automatically. The historical missing PIN dots/caret over a live desktop remains unexplained.

## Fresh-root ordinary portability and packaging preparation

A separate fresh-root ordinary application/all-six-suite build from this curated source passed **1,830 QtTest passes +205 BrowserAuth assertions**, zero failures/skips. All 60 unchanged source/test/build/resource/tooling inputs matched before and after; generated resource/dependency/fixture paths resolved within the source checkout without a dependency on another checkout. The app was not launched. This is same-host source portability, not a clean-chroot or another-distribution result; no fresh sanitizer run is claimed.

Corrected packaging prototypes passed independent AI static follow-up and bounded producer fresh provider/app builds/staging, configuration/JSON invariants, hardening receipts and helper controls. No full makepkg lifecycle, real package tarball, deployment, live authentication or independent human/legal certification follows. See [packaging details](PACKAGING.md).

## Before recommended installation or submission

Finish supported dependency provisioning, full package/clean-chroot checks, deployed artifact/provenance verification and separately approved install/removal checks. Coordinate current tenant/hardware/compositor validation; seek practical independent human feedback and user release/badge approval. Experimental source sharing is not Omarchy endorsement, package submission or supported installation. No paid/formal audit requirement is asserted here.

This concise summary is not publicly reproducible proof of every historical experiment. Source curation runs inventory, hygiene, link and syntax checks; the separately executed ordinary portability and packaging producer checks above are distinct evidence.
