#!/usr/bin/env python3
"""Create deterministic, synthetic libFuzzer seeds in a fresh private run."""
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
prompt = root / "corpus-promptparser"
profiles = root / "corpus-profiles"
prompt.mkdir(mode=0o700)
profiles.mkdir(mode=0o700)


def save(directory, name, data):
    with (directory / name).open("xb") as out:
        out.write(data)


authorization = (
    b"https://login.microsoftonline.com/fixture-tenant/oauth2/v2.0/authorize?"
    b"client_id=fixture&redirect_uri=https%3A%2F%2Flogin.microsoftonline.com%2Fcommon%2Foauth2%2Fnativeclient"
)
fp = b":".join([b"aa"] * 32)
fields = (
    b"\tCommon Name: fixture.example\n\tSubject:     /CN=fixture.example\n"
    b"\tIssuer:      /CN=Fixture CA\n\tValid from:  fixture-date\n"
    b"\tValid to:    fixture-date\n\tThumbprint:  " + fp + b"\n"
)
normal = (
    b"Certificate details for fixture.example:443 (RDP-Gateway):\n" + fields
    + b"The above X.509 certificate could not be verified, possibly because you do not have\n"
    b"the CA certificate in your certificate store, or the certificate has expired.\n"
    b"Please look at the OpenSSL documentation on how to add a private CA to the store.\n"
    b"Do you trust the above certificate? (Y/T/N) "
)
changed = (
    b"!!!Certificate for fixture.example:443 (RDP-Redirect) has changed!!!\n\n"
    b"New Certificate details:\n" + fields
    + b"\nOld Certificate details:\n\tSubject:     /CN=old.example\n"
    b"\tIssuer:      /CN=Old CA\n\tThumbprint:  " + b":".join([b"bb"] * 20)
    + b"\n\n\tA matching entry with legacy SHA1 was found in local known_hosts2 store.\n"
    b"\tIf you just upgraded from a FreeRDP version before 2.0 this is expected.\n"
    b"\tThe hashing algorithm has been upgraded from SHA1 to SHA256.\n"
    b"\tAll manually accepted certificates must be reconfirmed!\n\n"
    b"The above X.509 certificate does not match the certificate used for previous connections.\n"
    b"This may indicate that the certificate has been tampered with.\n"
    b"Please contact the administrator of the RDP server and clarify.\n"
    b"Do you trust the above certificate? (Y/T/N) "
)
prompt_seeds = {
    "empty": b"",
    "pin": b"FIDO2 PIN: ",
    "pin-tail": b"FIDO2 PIN: Password: \x1b\rX\nFIDO2 PIN: ",
    "authorization": b"Browse to: " + authorization + b"\r\nPaste redirect URL here: ",
    "authorization-overlap": b"Browse to: " + authorization + b"\nFIDO2 PIN: ",
    "authorization-duplicate-state": b"Browse to: " + authorization + b"&state=one&state=two\nPaste redirect URL here: ",
    "callback": b"https://login.microsoftonline.com/common/oauth2/nativeclient?code=fixture&state=fixture",
    "callback-control": b"https://login.microsoftonline.com/common/oauth2/nativeclient?code=fixture%0A",
    "normal-certificate": normal,
    "changed-certificate": changed,
    "duplicate-certificate": normal.replace(b"\tIssuer:", b"\tThumbprint:  " + fp + b"\n\tIssuer:"),
    "unsupported": b"Password: \nGatewayPassword: \nUnknown challenge: \n",
    "diagnostic-precedence": b"[ERROR] ERRCONNECT_CONNECT_TRANSPORT_FAILED ERRCONNECT_AUTHENTICATION_FAILED fixture-private\r\n",
    "diagnostic-security": b"ERRCONNECT_SECURITY_NEGO_CONNECT_FAILED\n",
    "controls": b"\x00\x1b\x7f\nFIDO2\r PIN: \n",
    "malformed-utf8": b"\xff\xc0\x80\xed\xa0\x80\n",
}
for boundary in (32767, 32768, 32769, 65500):
    prompt_seeds[f"line-{boundary}"] = b"x" * boundary + b"\nFIDO2 PIN: "
for index, (name, data) in enumerate(prompt_seeds.items()):
    # Fixed schedule/reset controls; mutations vary both, separately from payload.
    save(prompt, name, bytes([index, 17, index * 7, 0]) + data)

valid = (
    b"full address:s:cloudpc.example.test\r\n"
    b"gatewayhostname:s:gateway.example.test\r\n"
    b"armpath:s:/subscriptions/11111111-2222-3333-4444-555555555555/resourcegroups/fixture_group/providers/Microsoft.DesktopVirtualization/hostpools/fixture-pool\r\n"
    b"loadbalanceinfo:s:mth://localhost/aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee/01234567-89ab-cdef-0123-456789abcdef\r\n"
    b"remoteapplicationprogram:s:||99999999-8888-7777-6666-555555555555\r\n"
    b"redirectwebauthn:i:1\r\n"
)
profile_seeds = {
    "empty": b"",
    "valid-utf8": valid,
    "valid-utf8-bom": b"\xef\xbb\xbf" + valid,
    "valid-utf16le": b"\xff\xfe" + valid.decode().encode("utf-16le"),
    "valid-utf16be": b"\xfe\xff" + valid.decode().encode("utf-16be"),
    "unicode": valid + "username:s:fixture-\U0001f4bb\n".encode(),
    "bad-number": valid + b"screen mode id:i:two\n",
    "types": valid + b"fixture:b:00ff\nfixture:i:-1\nfixture:s:a:b:c\n",
    "nul": valid + b"username:s:a\x00b\n",
    "invalid-utf8": valid + b"name:s:\xff\xff\n",
    "truncated-utf8": valid + b"name:s:\xe2\x82",
    "truncated-utf16": b"\xff\xfe" + valid.decode().encode("utf-16le") + b"a",
    "utf16-surrogate": b"\xff\xfe" + valid.decode().encode("utf-16le") + b"\x00\xd8",
    "wrapper": b'{"url":"rdp://fixture.example.test"}',
    "missing-arm": b"full address:s:fixture.example.test\ngatewayhostname:s:fixture.example.test\n",
    "case-duplicate": valid + b"FULL ADDRESS:s:other.example.test\nfull address:i:1\n",
    "cli-option": valid + b"/cert:ignore\n",
    "bare-cr": valid + b"fixture:s:a\rb\n",
    "exact-limit": valid + b"fixture:s:" + b"x" * (1048576 - len(valid) - 10),
    "over-limit": b"x" * 1048577,
}
for name, data in profile_seeds.items():
    save(profiles, name, b"\x00" + data)
for mode in (1, 2, 3):
    save(profiles, f"structured-mode-{mode}", bytes([mode]) + b"fixture:i:-1\nusername:s:synthetic\n")

# libFuzzer token hints, not a substitute implementation or grammar oracle.
for target, tokens in {
    "promptparser": [b"FIDO2 PIN: ", b"Browse to: ", b"Paste redirect URL here: ",
                     b"ERRCONNECT_AUTHENTICATION_FAILED", b"ERRCONNECT_CONNECT_TRANSPORT_FAILED",
                     b"ERRCONNECT_SECURITY_NEGO_CONNECT_FAILED", b"\tThumbprint:  ", b"\r\n"],
    "profiles": [b"full address:s:", b"gatewayhostname:s:", b"armpath:s:", b":i:", b":s:", b":b:",
                 b"\xff\xfe", b"\xfe\xff", b"\xef\xbb\xbf", b"\r\n"],
}.items():
    dictionary = "\n".join('"' + "".join(f"\\x{byte:02x}" for byte in token) + '"' for token in tokens)
    save(root, f"{target}.dict", (dictionary + "\n").encode())
