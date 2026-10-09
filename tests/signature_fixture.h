#pragma once

#include <QByteArray>

// Historical-shape synthetic bytes, deliberately NOT CMS or a cryptographic
// signature. Kept as a regression corpus: production treats both fields as text
// and never decodes or checks this header/base64 representation.
inline QByteArray syntheticSignature(const QByteArray& payload = "fixture-private-sentinel")
{
    QByteArray blob = QByteArray::fromHex("0100010001000000");
    const quint32 size = quint32(payload.size());
    for (int shift = 0; shift < 32; shift += 8)
        blob.append(char(size >> shift));
    return (blob + payload).toBase64();
}

inline QByteArray signaturePair(const QByteArray& scope = "Full Address",
                                const QByteArray& signature = syntheticSignature())
{
    return "signature:s:" + signature + "\r\nsignscope:s:" + scope + "\r\n";
}

// Historical removed-policy examples, NOT the current allowed scope vocabulary.
// Keys let tests demonstrate that missing mapped settings no longer constrain
// opaque text. This table is only an invented/historical regression corpus.
struct HistoricalScopeExample { const char* label; const char* key; };
inline constexpr HistoricalScopeExample historicalScopeExamples[] = {
    {"Full Address", "full address"},
    {"Alternate Full Address", "alternate full address"},
    {"GatewayHostname", "gatewayhostname"},
    {"GatewayUsageMethod", "gatewayusagemethod"},
    {"GatewayProfileUsageMethod", "gatewayprofileusagemethod"},
    {"GatewayCredentialsSource", "gatewaycredentialssource"},
    {"PromptCredentialOnce", "promptcredentialonce"},
    {"RemoteApplicationProgram", "remoteapplicationprogram"},
    {"RemoteApplicationMode", "remoteapplicationmode"},
    {"Authentication Level", "authentication level"},
    {"AudioMode", "audiomode"},
    {"RedirectPrinters", "redirectprinters"},
    {"RedirectCOMPorts", "redirectcomports"},
    {"RedirectSmartCards", "redirectsmartcards"},
    {"RedirectClipboard", "redirectclipboard"},
    {"DevicesToRedirect", "devicestoredirect"},
    {"DrivesToRedirect", "drivestoredirect"},
    {"LoadBalanceInfo", "loadbalanceinfo"},
    {"RDGIsKDCProxy", "rdgiskdcproxy"},
};

inline QByteArray allSyntheticScopes(bool reverse = false)
{
    QByteArray value;
    for (int n = 0; n < 19; ++n) {
        if (n) value += ',';
        value += historicalScopeExamples[reverse ? 18 - n : n].label;
    }
    return value;
}
