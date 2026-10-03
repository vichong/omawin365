# Experimental local packaging

**Source-only prototypes, not supported installation/release or a recommendation for work-account use.** The [packaging tree](../packaging/README.md) has completed independent AI static review and bounded producer validation. Normal declared-dependency clean working-root builds and bounded static acceptance are complete below; full dynamic/license closure, host install/removal, deployed-prefix validation, current live authentication and independent human acceptance remain open.

## Layout and provider contract

`omawin365-freerdp` installs stock pinned FreeRDP 3.32.1 under `/usr/lib/omawin365/freerdp`, retaining its executable/libraries/addins and notices without replacing/conflicting with distro FreeRDP. An app wrapper at `/usr/bin/omawin365` selects the private provider through process-local PATH; the app binary is private under `/usr/lib/omawin365`. No LD_LIBRARY_PATH/global loader modification or dependency fallback is introduced. Older user-local launchers/desktop entries can shadow the wrapper; deliberate migration belongs to an approved deployment, not automatic deletion.

Provider invariants include SSO MIB OFF, camera client OFF, shared libraries ON, disabled Git version/revision discovery and all 229 accepted WITH_/CHANNEL_ flags. The actual JSON implementation is **jansson**, with generated definition/translation-unit and ELF dependency checks; no JSON implementation switch is introduced. Feature self-reports are not provenance, complete ABI/codec equality or proof of device/addin behavior. The provider retains upstream Apache, uwac/protocol notices and accurate LGPL/MIT/license metadata; complete dependency/license review remains open.

## Generate an honest local application recipe

There is no runnable app PKGBUILD or invented release URL/tag/checksum. The [template/helper](../packaging/app/prepare-snapshot.py) requires a checkout with a **committed HEAD containing both the template and wrapper**:

```sh
# After the source snapshot is committed; choose a new directory outside it.
python3 packaging/app/prepare-snapshot.py "$PWD" "$NEW_EXTERNAL_DIRECTORY"
```

Set `NEW_EXTERNAL_DIRECTORY` to a nonexistent external directory. The helper selects real committed HEAD, reads template/wrapper blobs from that same commit (not working-copy edits or script location), archives that source, and derives the local version, input checksums and provenance. Missing committed inputs, existing output or output inside the checkout fail. Keep generated PKGBUILD/archive/provenance outside the input source; do not commit them into it. A local snapshot recipe is not a pinned public release recipe.

The app template explicitly passes supplied CFLAGS/CXXFLAGS/LDFLAGS through qmake. Producer fresh builds/staging with explicit representative Arch flags verified compile/link receipts and staged PIE/full RELRO/nonexecutable stack plus bounded stack/FORTIFY evidence. The later actual makepkg lifecycle also completed strip/debug splitting. This is not universal hardening or runtime/stripped-app validation. App check validates desktop metadata; provider check validates static invariants. Neither substitutes for the six ordinary suites or stock client behavior.

Fresh isolated provider/app function-level build/check/package and staging evidence is retained separately. Relocated metadata-only version/buildconfig probes validate those paths, not deployed addin lookup. Missing tools or drift are blockers, not permission to fetch/install/disable features. Do not use `makepkg -s`, `-i`, sudo or database refresh as source-publication preparation.

## Clean working-root archives — 2026-10-03

Normal declared-dependency devtools builds and independent bounded **static package acceptance passed** for app source `02fbb8af242de5f8d4e9cdc47bf634d1ac51f189` and the pinned provider. Working roots clone a previously completed base: not pristine bootstrap, root-integrity audit or reproducible-build proof. Normal dependency resolution (no `--nodeps`) and full source verification/build/check/fakeroot/strip/debug/metadata/archive lifecycles completed. This establishes declared dependencies for these builds, not every plugin/dynamic runtime facility or future ABI compatibility.

Four local archives remain unpublished: app **240,620 bytes**, provider **2,159,809 bytes**, plus separate app/provider debug archives. Independent static checks verified source/recipe provenance, canonical numeric and named root ownership, safe private layout/MTREE, wrapper/assets/attribution, provider relative RPATHs/jansson, app hardening and ten matching ELF/debug build-ID/CRC pairs. No host product installation or product execution is claimed. Provider 229 invariants, SSO OFF and camera OFF passed. App check() is **desktop metadata only**, not a fresh 1,830 QtTest +205 BrowserAuth run inside chroot. This docs-only follow-up does not claim new code validation.

Static namcap excludes `unusedsodepends` because it invokes product loader execution through `ldd -r -u`. Findings are nonblocking for this bounded gate, **not lint-clean/full-default-namcap PASS**: provider six dependency warnings and one missing Maintainer tag; debug 152 cosmetic empty directories and nine detached symlink errors resolved with matching base archive. App eight dependency warnings and one similarly pair-validated debug false positive. Intended optional/plugin/subprocess dependencies remain; no pruning or exhaustive debug-source coverage claim.

Provider compiler-prefix-map reporting retains neutral `/build/omawin365-freerdp/src` paths, not private host paths/RPATH/addin lookups. Not a zero-build-path-bytes claim; upstream reporting was not masked. Earlier offline `--nodeps` archive results are distinct historical evidence, not these clean declared-dependency builds.

Before recommending install/submission: complete dynamic/plugin dependency and license checks, approved host install/removal/deployed-prefix/addin provenance, current tenant/desktop/input/resize/hardware checks and practical human/user release/badge approval. No supported installation, security certification or binary release is implied. See [verification limits](VERIFICATION.md).
