# Experimental local packaging

**Not a release, supported installation, AUR/OPR submission or publication approval.** These are auditable local prototypes. No clean-chroot/package-manager install/removal or current live acceptance is claimed. Do not use `makepkg -s`, `-i`, sudo or database refresh as part of preparation.

## Layout and migration

- `omawin365-freerdp` installs a complete private stock FreeRDP prefix at `/usr/lib/omawin365/freerdp`. Executables, libraries, headers, CMake/pkg-config metadata and any installed addins remain private. No global FreeRDP links or stock `freerdp` provides/replaces/conflicts.
- Private `bin/xfreerdp3` links to upstream `xfreerdp`. Installed RPATHs are origin-relative; compiled plugin prefix is private. The pinned source builds the selected channels into the client library: its channel macros set static linkage directly; there is **no `BUILTIN_CHANNELS` CMake option** in this source. Device/addin startup remains unverified.
- App binary is `/usr/lib/omawin365/omawin365`, public wrapper `/usr/bin/omawin365`. The wrapper prepends only private provider bin to this process tree's PATH; no LD_LIBRARY_PATH changes. Missing/nonexecutable provider fails clearly. Existing app probes reject loader failure or incompatible metadata, without trying distro FreeRDP instead. Other child programs inherit this PATH addition and can see the private provider's extra tools. Do not invoke the private app directly.
- Existing desktop/icon integration is retained. App MIT, third-party notices and Omarchy attribution are installed. The separate provider retains upstream Apache-2.0 LICENSE plus verbatim bundled uwac/protocol copyright/permission notices; license metadata includes LGPL-2.1-or-later for server-decoration. This is bounded attribution preparation, not a full dependency/license audit or legal certification.

**Older user-local installs:** a previous `~/.local/bin/omawin365` can shadow the public wrapper, and a user-local desktop entry can override the system entry. Before an approved deployment, inspect and deliberately migrate those owned files. Do not automatically delete them. The unwrapped old app may discover another FreeRDP; exact dependency probes still reject unsupported versions. No migration/install action is authorized by these instructions.

## Provider input and invariants

Pinned upstream commit `bf217a504e54cc719880c228e82353382cd7d4fa`; archive SHA256 `4a25a61f2e81f8d8832634c6d457edc1b48fc76630cf58c06695a27afdbbecca`. The recipe uses a real pinned GitHub archive URL, not a moving branch. Reuse a verified local copy named `FreeRDP-bf217a504e54cc719880c228e82353382cd7d4fa.tar.gz` for offline preparation. Do not download or install tools to rescue this local task.

Configuration preserves X11/AAD and accepted channel capabilities, including clipboard/WebAuthn; SSO MIB OFF and camera client OFF. Relative to the original candidate: private prefix, explicit default `lib`, explicit default camera-client OFF, explicit already-ON shared libraries, and **git version discovery disabled** (`USE_VERSION_FROM_GIT_TAG=OFF`, `USE_GIT_FOR_REVISION=OFF`). The pinned upstream version module can walk parent repositories; disabling it preserves the archive's actual `3.32.1 (n/a)` rather than stamping unrelated ancestry. No stock source edits or opportunistic feature removals.

`accepted-features.txt` contains the 229 accepted WITH_/CHANNEL_ BOOL cache entries. `check-contract.py` additionally rejects shared/version invariant drift, unsupported BUILTIN_CHANNELS overrides, unexpected generated version/revision, changed JSON translation-unit/definition, or changed JSON ELF dependency. Before build it checks generated configuration; after build it also checks libwinpr NEEDED. The expected JSON consumer is **jansson**, with `libjansson.so.4`. Actual old accepted candidate and packaging artifacts both compile `json/jansson.c` with WITH_JANSSON and link jansson. A historical private evidence narrative called this JSON-C; that sentence was inaccurate, not a runtime parser transition. No JSON-C forcing or implementation switch was made.

All 229 feature entries remain identical; the git-version flags are separate intentionally changed invariants. This check is not proof of every library/codec choice or complete ABI equivalence. Ordinary package dependencies are not newly pinned dependency versions. Dependency/ABI changes require rebuild/review; PCSC and other dynamically loaded facilities mean ELF NEEDED is not a complete runtime closure.

Copy `freerdp/` and the verified archive to a fresh external private working directory. Existing CMake/compiler/make/Python/binutils/development dependencies must suffice. Recipe explicitly resolves CMake from PATH, not inherited CMAKE; validation can supply existing private CMake via command-local PATH. Use a network-disabled filesystem namespace, private HOME and bounded two-job builds. Missing requirements or changed invariants are blockers, not permission to fetch/install/disable features. `check()` is static, not full FreeRDP tests/TLS/device/client execution.

## Portable application snapshot template

There is intentionally **no runnable app PKGBUILD in this checkout** and no invented public URL/tag/release checksum. `app/PKGBUILD.in` plus `app/prepare-snapshot.py` generate a real local source recipe outside a committed checkout:

```sh
python packaging/app/prepare-snapshot.py "$PWD" "$NEW_EXTERNAL_DIRECTORY"
```

Set `NEW_EXTERNAL_DIRECTORY` to a new, nonexistent directory outside the checkout. The helper archives **committed HEAD** and reads both `packaging/app/PKGBUILD.in` and `packaging/app/omawin365-launcher` as blobs from that same commit, never from the working copy or script location. Missing committed packaging inputs fail closed. It derives the full real commit, local version identifier and input checksums, then writes PKGBUILD, archive, launcher and snapshot provenance including the template/wrapper paths, hashes and origin commit. It refuses existing or in-checkout output. No Git initialization, commit, staging, history rewrite or public source coordinates. An uncommitted curated draft is not yet an input: after separately approved new-history commit, run the same helper there. Generated outputs must not be committed into the input snapshot; no self-referential checksum.

Copy the complete `packaging/` tree into an approved curated snapshot; no private commit or machine path is hardcoded in the template/helper. Generate only after its own committed source exists. Generated LOCAL recipes are not public release recipes; public pinned coordinates/tag/version and actual public artifact checksums remain a separate decision.

The app build explicitly passes makepkg CFLAGS, CXXFLAGS and LDFLAGS to qmake release variables, then make -j2. Validation used explicit representative x86-64 Arch hardening flags without reading host makepkg configuration: compile/link receipts preserve them, staged ELF is PIE/full RELRO/nonexecutable stack, stack-protector and fortified imports are present, and a compiled object contains endbr64. Compiler flag receipts are not proof of universal hardening coverage or runtime behavior. Only staged copies are stripped; original binaries retained.

App `check()` validates desktop metadata, not the complete six-suite procedure. Run offline suites separately with their private tmpfs runtime/display prerequisites; generic clean-chroot `make test` is not implied.

## Validation boundary and remaining gates

Fresh provider recipe build/check/package and generated app build/check/package passed locally in isolation. Source/recipe checksums, shell/metadata parsing, invariant failure controls, staged paths/permissions/licenses and no build-temp RPATH checks passed. Metadata-only provider version/buildconfig passed under private HOME, hidden host stores/sockets, fresh /dev, no network/display/LD_LIBRARY_PATH. Relocated staging metadata does **not** establish addin lookup at actual deployment prefix. No real app/GUI/browser, full client/TLS/device/addin startup, current live authentication or hardware retries were run. No new offline DD/sanitizer/full-suite claim.

Complete makepkg fetch/archive/strip/debug lifecycle, clean-chroot provenance, namcap, complete license/dependency closure and approved install/removal are still outstanding. Lead independent review precedes adoption. Public source approval/coordinates, deployed provenance, coordinated tenant/desktop/input/resize/hardware acceptance and practical human/user release approval remain separate gates. Nothing here authorizes network, install, deployment, publication or submission.
