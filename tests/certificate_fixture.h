#pragma once
#include <QByteArray>
#include <QString>

// Invented metadata only. The loopback oracle overrides the host/port with its
// own listener; none of these values identify an external service.
inline QString certificateStoppedMessage()
{
    return QStringLiteral("FreeRDP requested certificate trust that OMAWIN365 cannot safely grant. The connection is being stopped; no certificate exception was granted. Contact your administrator to review certificate trust and server identity.");
}
inline QByteArray fixtureFingerprint(char digit = 'a')
{
    QByteArray result;
    for (int i = 0; i < 32; ++i) {
        if (i) result += ':';
        result += QByteArray(2, digit);
    }
    return result;
}
inline QByteArray fixtureCertificate(bool changed = false, const QByteArray& role = "Server",
    const QByteArray& host = "fixture.example", quint16 port = 443)
{
    const QByteArray identity = host + ':' + QByteArray::number(port) + " (RDP-" + role + ')';
    QByteArray result = changed ? "!!!Certificate for " + identity + " has changed!!!\n\nNew Certificate details:\n"
        : "Certificate details for " + identity + ":\n";
    result += "\tCommon Name: fixture.example\n\tSubject:     /CN=fixture.example\n\tIssuer:      /CN=Fixture CA\n";
    result += "\tValid from:  fixture-date\n\tValid to:    fixture-date\n\tThumbprint:  " + fixtureFingerprint() + '\n';
    if (changed) {
        result += "\nOld Certificate details:\n\tSubject:     /CN=old.example\n\tIssuer:      /CN=Old CA\n";
        result += "\tThumbprint:  " + fixtureFingerprint('b') + "\n\n";
        result += "The above X.509 certificate does not match the certificate used for previous connections.\n"
            "This may indicate that the certificate has been tampered with.\n"
            "Please contact the administrator of the RDP server and clarify.\n";
    } else {
        result += "The above X.509 certificate could not be verified, possibly because you do not have\n"
            "the CA certificate in your certificate store, or the certificate has expired.\n"
            "Please look at the OpenSSL documentation on how to add a private CA to the store.\n";
    }
    return result + "Do you trust the above certificate? (Y/T/N) ";
}
