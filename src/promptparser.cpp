#include "promptparser.h"
#include "oauthcontract.h"

#include <QHostAddress>
#include <QRegularExpression>

namespace {
bool safeHost(const QString& host)
{
    QHostAddress address;
    if (address.setAddress(host))
        return true;
    static const QRegularExpression dns(QStringLiteral(
        "^(?=.{1,253}$)[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?"
        "(?:\\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*\\.?$"));
    return dns.match(host).hasMatch();
}

PromptParser::Event event(PromptParser::Kind kind)
{
    PromptParser::Event result;
    result.kind = kind;
    return result;
}
}

bool PromptParser::validAuthorization(const QUrl& url)
{
    return OAuthContract::request(url).has_value();
}

bool PromptParser::validCallback(const QUrl& url)
{
    return OAuthContract::callback(url);
}

void PromptParser::clearCertificate()
{
    certificateHost_.clear();
    certificatePort_ = 0;
    fingerprint_.clear();
    certificateChanged_ = false;
    certificateStage_ = CertificateStage::Idle;
}

void PromptParser::reset()
{
    line_.fill('\0');
    line_.clear();
    authorization_ = QUrl();
    clearCertificate();
    discarding_ = false;
}

QList<PromptParser::Event> PromptParser::feed(const QByteArray& bytes)
{
    QList<Event> events;
    for (char c : bytes) {
        if (!discarding_ &&
            ((line_.endsWith('\r') && c != '\n') ||
             (static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\r' && c != '\n') ||
             c == 0x7f)) {
            reset();
            discarding_ = true;
            events.append(event(Kind::InvalidPrompt));
        }
        if (c == '\n') {
            if (line_.endsWith('\r'))
                line_.chop(1);
            if (!discarding_) {
                if (mode_ == Mode::DiagnosticsOnly)
                    diagnosticComplete(events);
                else
                    lineComplete(events);
            }
            line_.fill('\0');
            line_.clear();
            discarding_ = false;
            continue;
        }
        if (discarding_)
            continue;
        if (line_.size() >= maximumLine) {
            // Never recognize a suffix of a truncated line as a fresh prompt.
            reset();
            discarding_ = true;
            events.append(event(Kind::InvalidPrompt));
            continue;
        }
        line_.append(c);
        if (mode_ == Mode::Interactive && c == ' ')
            promptComplete(events);
    }
    return events;
}

void PromptParser::promptComplete(QList<Event>& events)
{
    if (line_ == "FIDO2 PIN: ") {
        events.append(event(Kind::Pin));
    } else if (line_ == "Paste redirect URL here: ") {
        auto result = event(authorization_.isEmpty() ? Kind::InvalidPrompt : Kind::Authorization);
        result.url = authorization_;
        result.authorization = OAuthContract::request(authorization_);
        authorization_ = QUrl();
        events.append(result);
    } else if (line_ == "Do you trust the above certificate? (Y/T/N) ") {
        auto result = event(certificateStage_ == CertificateStage::Ready && !certificateHost_.isEmpty() &&
                            certificatePort_ && fingerprint_.size() == 32
                                ? Kind::Certificate : Kind::InvalidPrompt);
        result.host = certificateHost_;
        result.port = certificatePort_;
        result.fingerprint = fingerprint_;
        result.changed = certificateChanged_;
        events.append(result);
        clearCertificate();
    } else if (line_ == "Password: " || line_ == "Username: " || line_ == "Domain: " ||
               line_ == "GatewayPassword: " || line_ == "GatewayUsername: " ||
               line_ == "GatewayDomain: " || line_ == "Smartcard-Pin: " ||
               line_ == "Gateway Password: ") {
        events.append(event(Kind::UnsupportedPrompt));
    } else {
        if (certificateStage_ != CertificateStage::Idle)
            return; // Only the correctly ordered certificate block may reach Ready.
        // Runs after every space, so "? " first appears at the end; a rescan would be quadratic.
        if ((line_.endsWith(": ") || line_.endsWith("(Y/N) ") || line_.endsWith("? ")) &&
            !line_.startsWith("Browse to: ") && !line_.startsWith("Certificate details for ") &&
            !line_.startsWith("!!!Certificate for ")) {
            events.append(event(Kind::UnsupportedPrompt));
        } else {
            return;
        }
    }
    if (!authorization_.isEmpty()) {
        events.back().kind = Kind::InvalidPrompt;
        authorization_ = QUrl();
    }
    if (certificateStage_ != CertificateStage::Idle) {
        events.back().kind = Kind::InvalidPrompt;
        clearCertificate();
    }
    line_.fill('\0');
    line_.clear();
    discarding_ = true;
}

void PromptParser::lineComplete(QList<Event>& events)
{
    if (certificateStage_ != CertificateStage::Idle && line_.startsWith("Browse to: ")) {
        clearCertificate();
        events.append(event(Kind::InvalidPrompt));
        return;
    }
    if (!authorization_.isEmpty() && !line_.isEmpty() && !line_.startsWith("Browse to: ")) {
        authorization_ = QUrl();
        events.append(event(Kind::InvalidPrompt));
        return;
    }
    if (line_.startsWith("Browse to: ")) {
        const QByteArray bytes = line_.mid(11);
        if (authorization_.isEmpty() && !bytes.contains(' ') && !bytes.contains('\t') &&
            !bytes.contains('\0')) {
            const QUrl url = QUrl::fromEncoded(bytes, QUrl::StrictMode);
            if (validAuthorization(url)) {
                authorization_ = url;
                return;
            }
        }
        authorization_ = QUrl();
        events.append(event(Kind::InvalidPrompt));
        return;
    }

    const bool normal = line_.startsWith("Certificate details for ");
    const bool changed = line_.startsWith("!!!Certificate for ");
    if (normal || changed) {
        if (certificateStage_ != CertificateStage::Idle) {
            clearCertificate();
            events.append(event(Kind::InvalidPrompt));
            return;
        }
        clearCertificate();
        static const QRegularExpression header(QStringLiteral(
            "^(?:Certificate details for |!!!Certificate for )([A-Za-z0-9.:\\[\\]-]+):"
            "([0-9]{1,5}) \\(RDP-(?:Server|Gateway|Redirect)\\)(?::| has changed!!!)$"));
        const auto match = header.match(QString::fromLatin1(line_));
        if (!match.hasMatch()) {
            events.append(event(Kind::InvalidPrompt));
            return;
        }
        QString host = match.captured(1);
        if (host.startsWith('[') && host.endsWith(']'))
            host = host.mid(1, host.size() - 2);
        const uint port = match.captured(2).toUInt();
        if (!safeHost(host) || !port || port > 65535) {
            events.append(event(Kind::InvalidPrompt));
            return;
        }
        certificateHost_ = host;
        certificatePort_ = quint16(port);
        certificateChanged_ = changed;
        certificateStage_ = changed ? CertificateStage::ChangedBlank : CertificateStage::CommonName;
        return;
    }
    if (certificateStage_ != CertificateStage::Idle) {
        using Stage = CertificateStage;
        const auto transition = [this](const QByteArray& expected, Stage next) {
            if (line_ != expected)
                return false;
            certificateStage_ = next;
            return true;
        };
        const auto field = [this](const QByteArray& prefix, Stage next) {
            if (!line_.startsWith(prefix))
                return false;
            for (qsizetype i = prefix.size(); i < line_.size(); ++i) {
                const auto c = static_cast<unsigned char>(line_[i]);
                if (c < 0x20 || c == 0x7f)
                    return false;
            }
            certificateStage_ = next;
            return true;
        };
        const auto fingerprint = [this](bool old, Stage next) {
            if (!line_.startsWith("\tThumbprint:  "))
                return false;
            const QByteArray text = line_.mid(14);
            static const QRegularExpression sha256(QStringLiteral("^(?:[0-9A-Fa-f]{2}:){31}[0-9A-Fa-f]{2}$"));
            static const QRegularExpression sha1(QStringLiteral("^(?:[0-9A-Fa-f]{2}:){19}[0-9A-Fa-f]{2}$"));
            if (!sha256.match(QString::fromLatin1(text)).hasMatch() &&
                !(old && sha1.match(QString::fromLatin1(text)).hasMatch()))
                return false;
            if (!old) {
                QByteArray hex = text;
                hex.replace(":", "");
                fingerprint_ = QByteArray::fromHex(hex);
            }
            certificateStage_ = next;
            return true;
        };
        bool valid = false;
        switch (certificateStage_) {
        case Stage::ChangedBlank: valid = transition("", Stage::NewDetails); break;
        case Stage::NewDetails: valid = transition("New Certificate details:", Stage::CommonName); break;
        case Stage::CommonName: valid = field("\tCommon Name: ", Stage::Subject); break;
        case Stage::Subject: valid = field("\tSubject:     ", Stage::Issuer); break;
        case Stage::Issuer: valid = field("\tIssuer:      ", Stage::ValidityOrFingerprint); break;
        case Stage::ValidityOrFingerprint:
            valid = field("\tValid from:  ", Stage::ValidTo) ||
                    fingerprint(false, certificateChanged_ ? Stage::NewBlank : Stage::NormalProse);
            break;
        case Stage::ValidTo: valid = field("\tValid to:    ", Stage::Fingerprint); break;
        case Stage::Fingerprint:
            valid = fingerprint(false, certificateChanged_ ? Stage::NewBlank : Stage::NormalProse); break;
        case Stage::NewBlank: valid = transition("", Stage::OldDetails); break;
        case Stage::OldDetails: valid = transition("Old Certificate details:", Stage::OldSubject); break;
        case Stage::OldSubject: valid = field("\tSubject:     ", Stage::OldIssuer); break;
        case Stage::OldIssuer: valid = field("\tIssuer:      ", Stage::OldValidityOrFingerprint); break;
        case Stage::OldValidityOrFingerprint:
            valid = field("\tValid from:  ", Stage::OldValidTo) || fingerprint(true, Stage::OldBlank); break;
        case Stage::OldValidTo: valid = field("\tValid to:    ", Stage::OldFingerprint); break;
        case Stage::OldFingerprint: valid = fingerprint(true, Stage::OldBlank); break;
        case Stage::OldBlank: valid = transition("", Stage::LegacyOrChangedProse); break;
        case Stage::LegacyOrChangedProse:
            valid = transition("\tA matching entry with legacy SHA1 was found in local known_hosts2 store.", Stage::LegacyUpgrade) ||
                    transition("The above X.509 certificate does not match the certificate used for previous connections.", Stage::ChangedTampered);
            break;
        case Stage::LegacyUpgrade:
            valid = transition("\tIf you just upgraded from a FreeRDP version before 2.0 this is expected.", Stage::LegacyHash); break;
        case Stage::LegacyHash:
            valid = transition("\tThe hashing algorithm has been upgraded from SHA1 to SHA256.", Stage::LegacyConfirm); break;
        case Stage::LegacyConfirm:
            valid = transition("\tAll manually accepted certificates must be reconfirmed!", Stage::LegacyBlank); break;
        case Stage::LegacyBlank: valid = transition("", Stage::LegacyOrChangedProse); break;
        case Stage::ChangedTampered:
            valid = transition("This may indicate that the certificate has been tampered with.", Stage::ChangedAdministrator); break;
        case Stage::ChangedAdministrator:
            valid = transition("Please contact the administrator of the RDP server and clarify.", Stage::Ready); break;
        case Stage::NormalProse:
            valid = transition("The above X.509 certificate could not be verified, possibly because you do not have", Stage::NormalCa); break;
        case Stage::NormalCa:
            valid = transition("the CA certificate in your certificate store, or the certificate has expired.", Stage::NormalDocumentation); break;
        case Stage::NormalDocumentation:
            valid = transition("Please look at the OpenSSL documentation on how to add a private CA to the store.", Stage::Ready); break;
        case Stage::Ready:
        case Stage::Idle:
            break;
        }
        if (!valid) {
            clearCertificate();
            events.append(event(Kind::InvalidPrompt));
        }
        return;
    }
    if (line_.startsWith("\tThumbprint:") || line_.startsWith("\tCommon Name:") ||
        line_.startsWith("\tSubject:") || line_.startsWith("\tIssuer:") ||
        line_.endsWith(":") || line_.endsWith("?")) {
        events.append(event(Kind::InvalidPrompt));
        return;
    }
    diagnosticComplete(events);
}

void PromptParser::diagnosticComplete(QList<Event>& events)
{
    // Fixed diagnostic vocabulary only. Tokens, URLs and arbitrary log text are discarded.
    if (line_.contains("ERRCONNECT_AUTHENTICATION_FAILED")) {
        auto result = event(Kind::Diagnostic);
        result.detail = QStringLiteral("FreeRDP reported authentication failure.");
        events.append(result);
    } else if (line_.contains("ERRCONNECT_CONNECT_TRANSPORT_FAILED")) {
        auto result = event(Kind::Diagnostic);
        result.detail = QStringLiteral("FreeRDP reported a transport connection failure.");
        events.append(result);
    } else if (line_.contains("ERRCONNECT_SECURITY_NEGO_CONNECT_FAILED")) {
        auto result = event(Kind::Diagnostic);
        result.detail = QStringLiteral("FreeRDP reported a security negotiation failure.");
        events.append(result);
    }
}
