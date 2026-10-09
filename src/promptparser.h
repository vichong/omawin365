#pragma once

#include "oauthcontract.h"
#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>

// Upstream FreeRDP 3.32.1 client/common/client.c prompts. Raw output never leaves
// this parser; only bounded, validated protocol events do.
class PromptParser
{
public:
    enum class Mode { Interactive, DiagnosticsOnly };
    enum class Kind { Authorization, Pin, Certificate, UnsupportedPrompt, InvalidPrompt, Diagnostic };
    struct Event {
        Kind kind;
        QUrl url;
        std::optional<OAuthContract::Request> authorization;
        QString host;
        quint16 port = 0;
        QByteArray fingerprint;
        bool changed = false;
        QString detail;
    };

    explicit PromptParser(Mode mode = Mode::Interactive) : mode_(mode) {}
    QList<Event> feed(const QByteArray& bytes);
    void reset();
    static bool validAuthorization(const QUrl& url);
    static bool validCallback(const QUrl& url);
    static constexpr qsizetype maximumLine = 32768;

private:
    void lineComplete(QList<Event>& events);
    void diagnosticComplete(QList<Event>& events);
    void promptComplete(QList<Event>& events);
    void clearCertificate();
    Mode mode_;
    QByteArray line_;
    QUrl authorization_;
    QString certificateHost_;
    quint16 certificatePort_ = 0;
    QByteArray fingerprint_;
    bool certificateChanged_ = false;
    enum class CertificateStage {
        Idle, ChangedBlank, NewDetails, CommonName, Subject, Issuer,
        ValidityOrFingerprint, ValidTo, Fingerprint, NewBlank, OldDetails,
        OldSubject, OldIssuer, OldValidityOrFingerprint, OldValidTo,
        OldFingerprint, OldBlank, LegacyOrChangedProse, LegacyUpgrade,
        LegacyHash, LegacyConfirm, LegacyBlank,
        NormalProse, NormalCa, NormalDocumentation,
        ChangedTampered, ChangedAdministrator, Ready
    };
    CertificateStage certificateStage_ = CertificateStage::Idle;
    bool discarding_ = false;
};
