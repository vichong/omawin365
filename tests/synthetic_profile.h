#pragma once

#include <QByteArray>

// Invented connection metadata, never copied from a portal export or real profile.
inline const QByteArray supportedProfile =
    "full address:s:cloudpc.example.test\r\n"
    "gatewayhostname:s:gateway.example.test\r\n"
    "armpath:s:/subscriptions/11111111-2222-3333-4444-555555555555/resourcegroups/fixture_group/providers/Microsoft.DesktopVirtualization/hostpools/fixture-pool\r\n"
    "loadbalanceinfo:s:mth://localhost/aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee/01234567-89ab-cdef-0123-456789abcdef\r\n"
    "remoteapplicationprogram:s:||99999999-8888-7777-6666-555555555555\r\n";

// Narrow EU public-example family, assembled from approved rules with invented IDs.
inline const QByteArray euOptionalSettings =
    "wvd endpoint pool:s:22222222-3333-4444-5555-666666666666\r\n"
    "workspace id:s:33333333-4444-5555-6666-777777777777\r\n"
    "geo:s:EU\r\n"
    "alternate full address:s:CLOUDPC.example.test\r\n"
    "diagnosticserviceurl:s:https://rdweb-g-eu-r1.wvd.microsoft.com/api/arm/DiagnosticEvents/v1\r\n"
    "hubdiscoverygeourl:s:https://rdweb-g-eu-r1.wvd.microsoft.com/api/arm/hubdiscovery?resourceId=44444444-5555-6666-7777-888888888888\r\n"
    "remotedesktopname:s:Cloud PC Enterprise 2vCPU/8GB/128GB\r\n"
    "activityhint:s:ms-wvd-ep:55555555-6666-7777-8888-999999999999?ScaleUnitPath={\"Geo\"%3a\"EU\"%2c\"Ring\"%3a3%2c\"Region\"%3a\"westeurope\"%2c\"ScaleUnit\"%3a123}\r\n"
    "gatewayusagemethod:i:1\r\n"
    "gatewayprofileusagemethod:i:1\r\n"
    "authentication level:i:1\r\n"
    "gatewaybrokeringtype:i:1\r\n"
    "promptcredentialonce:i:1\r\n"
    "redirectclipboard:i:1\r\n"
    "redirectprinters:i:1\r\n"
    "redirectsmartcards:i:1\r\n"
    "dynamic resolution:i:1\r\n"
    "audiocapturemode:i:1\r\n"
    "redirectcomports:i:1\r\n"
    "singlemoninwindowedmode:i:1\r\n"
    "redirectlocation:i:1\r\n"
    "targetisaadjoined:i:1\r\n"
    "gatewaycredentialssource:i:0\r\n"
    "remoteapplicationmode:i:0\r\n"
    "audiomode:i:0\r\n"
    "enablerdsaadauth:i:0\r\n"
    "clientrejectinjectedinput:i:0\r\n"
    "rdgiskdcproxy:i:0\r\n"
    "camerastoredirect:s:*\r\n"
    "devicestoredirect:s:*\r\n"
    "drivestoredirect:s:*\r\n"
    "usbdevicestoredirect:s:*\r\n";

inline const QByteArray fullerProfile = supportedProfile + euOptionalSettings +
    "aadtenantid:s:abcdefab-1234-5678-9abc-def012345678\r\nresourceprovider:s:arm\r\nredirectwebauthn:i:1\r\n";
