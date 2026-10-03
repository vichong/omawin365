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

Corrected packaging prototypes passed independent AI static follow-up and bounded producer builds/staging. On 2026-10-03, separate normal declared-dependency devtools clean working-root builds and independent bounded **static archive acceptance passed** at public app source `02fbb8af242de5f8d4e9cdc47bf634d1ac51f189`. Four local main/debug archives were produced (app 240,620 bytes; provider 2,159,809 bytes), with source/recipe provenance, canonical numeric/named root ownership, safe archive/MTREE/attribution, ELF/loader-path/debug pairing and app hardening checked. Lineage clones a previously completed base, not pristine bootstrap or reproducibility proof. Dependencies resolved normally for these builds, not full dynamic/runtime/license closure.

App check() validates desktop metadata **only**, not a fresh full-suite/sanitizer run inside chroot; earlier ordinary and sanitizer totals/provenance above remain separate. Static namcap excludes loader-executing `unusedsodepends`; optional-dependency/Maintainer/cosmetic debug-directory findings are nonblocking and detached-debug symlink diagnostics resolve with paired base archives. Not lint-clean or full default-namcap PASS. Neutral provider compiler-receipt build paths remain, no zero-build-path claim. No local binary upload/release, GUI/client runtime/current live or independent human/legal acceptance follows from this static gate; later one-host metadata acceptance is separate below. See [packaging details](PACKAGING.md).

## One-host installation and isolated metadata — 2026-10-03

An exact two-main-package local experimental transaction completed; independent installed-state acceptance matched all 325 payload identities with accepted artifacts and unchanged scoped stock FreeRDP/dependencies/tools/hooks. Normal package-manager database/log/cache and disclosed mutex/mtime/icon/MIME-cache effects occurred. Both packages remain installed, no rollback ran; removal lifecycle is untested.

Four isolated nonroot provider version/buildconfig and app/provider loader-list receipts completed successfully, confirming 3.32.1 (n/a), unambiguous SSO MIB OFF and private library bindings. Reviewed hash-bound namespace code/receipts support intended isolation, not separate live kernel namespace attestation. Provider print paths initialize/free settings/context but exit before client start/connection; loader listing does not invoke app main. No public-wrapper/app/GUI/browser/login/client/addin/device/hardware functionality is demonstrated. Tested artifact source remains `02fbb8af242de5f8d4e9cdc47bf634d1ac51f189`; this docs-only update is not a new code/full-suite/sanitizer campaign.

## Before recommended installation or submission

Finish dynamic/plugin dependency and license closure, functional deployed app/provider discovery/addin checks and separately approved removal-lifecycle validation. Coordinate current tenant/hardware/compositor validation; seek practical independent human feedback and user release/badge approval. Experimental source sharing is not Omarchy endorsement, package submission or supported installation. No paid/formal audit requirement is asserted here.

This concise summary is not publicly reproducible proof of every historical experiment. Source curation runs inventory, hygiene, link and syntax checks; the separately executed ordinary portability and packaging producer checks above are distinct evidence.
