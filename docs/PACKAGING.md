# Experimental local packaging

**Source-only prototypes, not supported installation/release or a recommendation for work-account use.** The [packaging tree](../packaging/README.md) has completed independent AI static review and bounded producer validation. No full makepkg lifecycle, clean-chroot, namcap, deployed-prefix install/removal, current live authentication or independent human acceptance is claimed.

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

The app template explicitly passes supplied CFLAGS/CXXFLAGS/LDFLAGS through qmake. Producer fresh builds/staging with explicit representative Arch flags verified compile/link receipts and staged PIE/full RELRO/nonexecutable stack plus bounded stack/FORTIFY evidence. Only staged copies were stripped. This is not universal hardening or runtime/stripped-app validation. App check validates desktop metadata; provider check validates static invariants. Neither substitutes for the six ordinary suites or stock client behavior.

Fresh isolated provider/app function-level build/check/package and staging evidence is retained separately. Relocated metadata-only version/buildconfig probes validate those paths, not deployed addin lookup. Missing tools or drift are blockers, not permission to fetch/install/disable features. Do not use `makepkg -s`, `-i`, sudo or database refresh as source-publication preparation.

Before recommending install/submission: complete full makepkg/clean-chroot/package/dependency/license checks, approved install/removal/deployed provenance, current tenant/desktop/input/resize/hardware checks and practical human/user release/badge approval. See [verification limits](VERIFICATION.md).
