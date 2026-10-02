#pragma once

#include "../src/session.h"
#include "synthetic_profile.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <optional>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <cerrno>

// Read-only probe used to report the existing-display prerequisite explicitly.
bool sessionFixtureDisplayAvailable();

// These observations are made by the external adapter, not Session private state.
class SessionFixture
{
    QTemporaryDir sandbox_;
    QList<QPair<QByteArray, std::optional<QByteArray>>> environment_;
    int sequence_ = 0;
public:
    QStringList lifecycle; // Outlives Session, including any teardown signal delivery.
    QStringList notifications; // Combined diagnostic/status/error/end ordering.
    Session session;
    QSignalSpy statuses{&session, &Session::statusChanged};
    QSignalSpy errors{&session, &Session::error};
    QSignalSpy ended{&session, &Session::ended};
    QSignalSpy pins{&session, &Session::pinRequested};
    void submitAuthUrl(const QUrl& url) {
        const quint64 generation = auth.isEmpty() ? 0 : auth.last().at(0).value<OAuthContract::Request>().generation;
        session.submitAuthResult({url, generation});
    }
    QSignalSpy auth{&session, &Session::authRequested};
    QSignalSpy diagnostics{&session, &Session::diagnosticEvent};

    SessionFixture()
    {
        QObject::connect(&session, &Session::statusChanged, &session,
            [this](const QString& phase) {
                lifecycle.append(QStringLiteral("status:") + phase);
                notifications.append(QStringLiteral("status:") + phase);
            });
        QObject::connect(&session, &Session::error, &session,
            [this] { lifecycle.append(QStringLiteral("error")); notifications.append(QStringLiteral("error")); });
        QObject::connect(&session, &Session::ended, &session,
            [this] { lifecycle.append(QStringLiteral("ended")); notifications.append(QStringLiteral("ended")); });
        QObject::connect(&session, &Session::diagnosticEvent, &session,
            [this](const QString& event) { notifications.append(QStringLiteral("diagnostic:") + event); });
    }
    ~SessionFixture()
    {
        for (const auto& [name, value] : environment_) {
            if (value)
                qputenv(name.constData(), *value);
            else
                qunsetenv(name.constData());
        }
        // Session is destroyed before its sandbox; production destructor owns cleanup.
    }
    void environment(const QByteArray& name, const QByteArray& value)
    {
        environment_.append({name, qEnvironmentVariableIsSet(name.constData())
            ? std::optional<QByteArray>(qgetenv(name.constData())) : std::nullopt});
        qputenv(name.constData(), value);
    }
    QString path(const QString& name) const { return sandbox_.filePath(name); }
    QString profile() const { return path(QStringLiteral("synthetic.rdp")); }
    bool write(const QString& name, const QByteArray& bytes)
    {
        QFile file(path(name));
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
    }
    bool init(const QJsonObject& settings = {})
    {
        if (!sandbox_.isValid() || !QFile::copy(QStringLiteral(SESSION_FIXTURE_PATH), path(QStringLiteral("xfreerdp3"))) ||
            !QFile::setPermissions(path(QStringLiteral("xfreerdp3")), QFileDevice::ReadOwner |
                QFileDevice::WriteOwner | QFileDevice::ExeOwner) ||
            !write(QStringLiteral("settings.json"), QJsonDocument(settings).toJson(QJsonDocument::Compact)) ||
            !write(QStringLiteral("commands.jsonl"), {}) ||
            !write(QStringLiteral("synthetic.rdp"), supportedProfile))
            return false;
        environment("PATH", QFile::encodeName(sandbox_.path()));
        session.setDiagnosticsEnabled(true);
        return true;
    }
    QList<QJsonObject> events(const QString& kind) const
    {
        QFile file(path(QStringLiteral("events.jsonl")));
        QList<QJsonObject> result;
        if (!file.open(QIODevice::ReadOnly))
            return result;
        while (!file.atEnd()) {
            const QByteArray line = file.readLine();
            if (!line.endsWith('\n'))
                break;
            const auto item = QJsonDocument::fromJson(line).object();
            if (item.value(QStringLiteral("kind")).toString() == kind)
                result.append(item);
        }
        return result;
    }
    QByteArray input() const
    {
        QByteArray result;
        for (const auto& item : events(QStringLiteral("input")))
            result += QByteArray::fromBase64(item.value(QStringLiteral("bytes")).toString().toLatin1());
        return result;
    }
    int output(const QByteArray& bytes, bool stderr = false)
    {
        const int sequence = ++sequence_;
        const QJsonObject command{{QStringLiteral("bytes"), QString::fromLatin1(bytes.toBase64())},
            {QStringLiteral("stderr"), stderr}, {QStringLiteral("sequence"), sequence}};
        const QByteArray line = QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n';
        return appendCommand(line) ? sequence : -1;
    }
    int outputAndExit(const QByteArray& bytes, int code = 1)
    {
        const int sequence = ++sequence_;
        return appendCommand(QJsonDocument(QJsonObject{
            {QStringLiteral("bytes"), QString::fromLatin1(bytes.toBase64())},
            {QStringLiteral("sequence"), sequence}, {QStringLiteral("exit_after_output"), code}})
            .toJson(QJsonDocument::Compact) + '\n') ? sequence : -1;
    }
    bool resumeInput()
    {
        return appendCommand("{\"resume_input\":true}\n");
    }
    bool delivered(int sequence) const
    {
        for (const auto& item : events(QStringLiteral("output"))) {
            if (item.value(QStringLiteral("sequence")).toInt() == sequence)
                return true;
        }
        return false;
    }
    bool appendCommand(const QByteArray& bytes)
    {
        QFile file(path(QStringLiteral("commands.jsonl")));
        return file.open(QIODevice::WriteOnly | QIODevice::Append) && file.write(bytes) == bytes.size();
    }
    bool exit(int code)
    {
        return appendCommand(QJsonDocument(QJsonObject{{QStringLiteral("exit"), code}}).toJson(QJsonDocument::Compact) + '\n');
    }
};

inline QByteArray fixtureAuthorization(const QByteArray& extra = "&state=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
{
    return "Browse to: https://login.microsoftonline.com/common/oauth2/v2.0/authorize?"
        "client_id=a85cf173-4192-42f8-81fa-777a763e6e2c&response_type=code&scope=https%3A%2F%2Fwww.wvd.microsoft.com%2F.default+openid+profile+offline_access&code_challenge=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb&code_challenge_method=S256&redirect_uri=https%3A%2F%2Flogin.microsoftonline.com%2Fcommon%2Foauth2%2Fnativeclient" + extra +
        "\nPaste redirect URL here: ";
}

inline QUrl fixtureCallback(const QString& query = QStringLiteral("code=fixture-code&state=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"))
{
    return QUrl(QStringLiteral("https://login.microsoftonline.com/common/oauth2/nativeclient?") + query);
}

inline bool fixtureHasPhase(const QSignalSpy& statuses, const QString& phase)
{
    for (const auto& status : statuses) {
        if (status.at(0).toString() == phase)
            return true;
    }
    return false;
}
