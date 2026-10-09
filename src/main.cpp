#include "browserauth.h"
#include "profilestore.h"
#include "session.h"
#include "window.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QDebug>
#include <QIcon>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <optional>
#include <sys/prctl.h>
#include <sys/resource.h>

#ifndef OMAWIN365_VERSION
#define OMAWIN365_VERSION "dev" // Set by omawin365.pro from the package or git commit.
#endif

namespace {
int fail(const QString& message)
{
    QMessageBox box(QMessageBox::Critical, QStringLiteral("OMAWIN365"), message, QMessageBox::Ok);
    box.setTextFormat(Qt::PlainText);
    box.exec();
    return 1;
}

// Opt-in, local correlation only. Never connect raw status/error/URL output to
// this sink. Producers supply fixed labels; request metadata is bucketed below.
struct AuthenticationDiagnostics {
    QTemporaryFile file;
    QElapsedTimer clock;
    quint64 attempt = 0;
    quint64 transport = 0;
    quint64 authorization = 0;
    quint64 pin = 0;
    bool desktop = false;

    bool open(const QString& runtime)
    {
        file.setFileTemplate(QDir(runtime).filePath(QStringLiteral("omawin365-auth-XXXXXX.log")));
        if (!file.open() || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
            return false;
        file.setAutoRemove(false);
        clock.start();
        record(QStringLiteral("diagnostics-start"));
        return file.isOpen();
    }

    void record(const QString& event)
    {
        if (!file.isOpen())
            return;
        if (event == QLatin1String("connection-start")) {
            ++attempt;
            transport = authorization = pin = 0;
            desktop = false;
        } else if (event == QLatin1String("transport-start")) {
            ++transport;
        } else if (event == QLatin1String("authorization-request")) {
            ++authorization;
        } else if (event == QLatin1String("native-pin-request")) {
            ++pin;
        } else if (event == QLatin1String("desktop-connected")) {
            desktop = true;
        }
        const QByteArray line = QStringLiteral(
            "[auth-flow] v=1 ms=%1 attempt=%2 transport=%3 authorization=%4 native_pin=%5 desktop=%6 event=%7\n")
            .arg(clock.elapsed()).arg(attempt).arg(transport).arg(authorization)
            .arg(pin).arg(desktop ? 1 : 0).arg(event).toLatin1();
        if (file.size() + line.size() > 64 * 1024) {
            file.close();
            qWarning("Authentication diagnostics stopped at the 64 KiB limit.");
        } else if (file.write(line) != line.size() || !file.flush()) {
            file.close();
            qWarning("Authentication diagnostics stopped after a write failure.");
        }
    }

    void authorizationRequest(const OAuthContract::Request& request)
    {
        if (!file.isOpen())
            return;
        const auto scopes = request.scope.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        bool gateway = false;
        bool target = false;
        for (const auto& scope : scopes) {
            gateway |= scope == QLatin1String("https://www.wvd.microsoft.com/.default");
            target |= scope.startsWith(QLatin1String("ms-device-service://termsrv.wvd.microsoft.com/name/")) &&
                      scope.endsWith(QLatin1String("/user_impersonation"));
        }
        // FreeRDP 3.32.1: settings.c gateway scope and aad.c target scope.
        // These buckets are diagnostic hints, never authentication decisions.
        record(gateway && !target ? QStringLiteral("authorization-stage-gateway") :
               target && !gateway ? QStringLiteral("authorization-stage-desktop") :
                                    QStringLiteral("authorization-stage-other"));
        // The exact seven-field profile contains neither prompt nor max_age.
        record(QStringLiteral("authorization-prompt-absent"));
        record(QStringLiteral("authorization-max-age-absent"));
    }
};
}

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("omawin365"));
    QCoreApplication::setOrganizationName(QStringLiteral("omawin365"));
    QCoreApplication::setApplicationVersion(QStringLiteral(OMAWIN365_VERSION));
    QApplication::setApplicationDisplayName(QStringLiteral("OMAWIN365"));
    QApplication::setDesktopFileName(QStringLiteral("omawin365"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/omawin365.svg")));

    // Auth codes and PINs pass through process memory. Disable crash dumps before
    // any browser/FreeRDP child is started; this does not promise zeroization.
    const rlimit noCore {0, 0};
    if (setrlimit(RLIMIT_CORE, &noCore) != 0 || prctl(PR_SET_DUMPABLE, 0) != 0)
        return fail(QStringLiteral("Could not disable process dumps. OMAWIN365 will not start a session without this protection."));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Windows 365 Cloud PCs for Omarchy. Tested with Windows 365 Enterprise."));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption importOption(QStringLiteral("import"),
        QStringLiteral("Import a trusted .rdp or .rdpw connection file. Does not auto-connect."), QStringLiteral("path"));
    parser.addOption(importOption);
    QCommandLineOption diagnosticsOption(QStringLiteral("diagnose-auth"),
        QStringLiteral("Record secret-free authentication stage/restart counts in a private runtime log."));
    parser.addOption(diagnosticsOption);
    parser.process(app);
    if (!parser.positionalArguments().isEmpty())
        return fail(QStringLiteral("Unexpected positional argument. Use --import PATH to import a connection file, or --help for usage."));
    if (parser.isSet(importOption) && parser.value(importOption).isEmpty())
        return fail(QStringLiteral("--import requires a connection-file path."));

    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty() || !QDir(runtime).exists())
        return fail(QStringLiteral("A private runtime directory is unavailable. Start OMAWIN365 from your desktop session."));
    QLockFile lock(QDir(runtime).filePath(QStringLiteral("omawin365.lock")));
    if (!lock.tryLock(0)) {
        if (lock.error() == QLockFile::LockFailedError)
            return fail(QStringLiteral("OMAWIN365 is already running. Use its existing window to manage your Cloud PC."));
        return fail(QStringLiteral("Could not lock the private runtime directory. OMAWIN365 will not open a second profile store or session."));
    }

    std::optional<AuthenticationDiagnostics> diagnostics;
    if (parser.isSet(diagnosticsOption)) {
        diagnostics.emplace();
        if (!diagnostics->open(runtime))
            return fail(QStringLiteral("Could not create the private authentication diagnostic log."));
        qInfo().noquote() << QStringLiteral("Authentication diagnostics:") << diagnostics->file.fileName();
    }
    ProfileStore profiles;
    Session session;
    BrowserAuth browser;
    if (parser.isSet(diagnosticsOption)) {
        // Connect before Window so each request is counted before browser work.
        QObject::connect(&session, &Session::diagnosticEvent, &app,
                         [&diagnostics](const QString& event) { diagnostics->record(event); });
        QObject::connect(&session, &Session::authRequested, &app,
                         [&diagnostics](const OAuthContract::Request& request) { diagnostics->authorizationRequest(request); });
        QObject::connect(&session, &Session::ended, &app,
                         [&diagnostics] { diagnostics->record(QStringLiteral("connection-end")); });
        QObject::connect(&browser, &BrowserAuth::diagnosticEvent, &app,
                         [&diagnostics](const QString& event) { diagnostics->record(event); });
        session.setDiagnosticsEnabled(true);
        browser.setDiagnosticsEnabled(true);
    }
    Window window(&session, &browser, &profiles);
    window.show();
    if (parser.isSet(importOption)) {
        const QString path = parser.value(importOption);
        QTimer::singleShot(0, &window, [&window, path] { window.importProfile(path); });
    }
    return app.exec();
}
