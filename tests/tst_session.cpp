// Retained non-certificate Session regressions. Certificate denial is exercised
// through the public parser/PTY suite; no independent TLS approval fixture remains.
#include "certificate_fixture.h"
#include <QSignalSpy>
#include <QScopeGuard>
#include <QtTest>
#include "../src/session.cpp"

struct SessionTestAccess {
    using Awaiting = Session::State::Awaiting;
    using VersionCheck = Session::State::VersionCheck;
    static auto& state(Session& session) { return *session.state_; }
    static void reap(Session& session) { session.checkChild(); }
    static void reject(Session& session) { session.rejectCertificate(certificateStoppedMessage()); }
    static void flush(Session& session) { session.flushInput(); }
    static bool ownChild(Session& session)
    {
        int ready[2];
        if (pipe2(ready, O_CLOEXEC) < 0)
            return false;
        const pid_t child = fork();
        if (child == 0) {
            close(ready[0]);
            // Model an exec'd transport, not the parent's QtTest crash handler.
            ::signal(SIGTERM, SIG_DFL);
            if (setsid() < 0)
                _exit(1);
            const char byte = 'R';
            if (write(ready[1], &byte, 1) != 1)
                _exit(1);
            close(ready[1]);
            for (;;) pause();
        }
        close(ready[1]);
        char byte = 0;
        ssize_t count;
        do { count = read(ready[0], &byte, 1); } while (count < 0 && errno == EINTR);
        close(ready[0]);
        if (child <= 0)
            return false;
        session.state_->child = child;
        ++session.state_->generation;
        return count == 1 && byte == 'R';
    }
};

class SessionTest : public QObject {
    Q_OBJECT
private slots:
    void authorizationTimeoutInvalidatesTransaction()
    {
        Session session;
        QVERIFY(SessionTestAccess::ownChild(session));
        auto& state = SessionTestAccess::state(session);
        state.awaiting = SessionTestAccess::Awaiting::Authorization;
        state.authTransaction = OAuthContract::Request{QUrl(), QString(64, 'a'), QString(43, 'b'), state.generation};
        const quint64 generation = state.generation;
        QSignalSpy errors(&session, &Session::error);
        state.authTimer.start(1); // Production callback with a test-shortened deadline.
        QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 3000);
        QVERIFY(state.stopping);
        QVERIFY(!state.authTransaction);
        QVERIFY(state.generation > generation);
        QVERIFY(!state.authTimer.isActive());
        SessionTestAccess::reap(session);
    }

    void certificateRejectionClearsPartialInput_data()
    {
        QTest::addColumn<bool>("authorization");
        QTest::addColumn<bool>("reject");
        QTest::newRow("partial-pin-denial") << false << true;
        QTest::newRow("partial-auth-denial") << true << true;
        QTest::newRow("partial-pin-sensitivity") << false << false;
        QTest::newRow("partial-auth-sensitivity") << true << false;
    }
    void certificateRejectionClearsPartialInput()
    {
        QFETCH(bool, authorization);
        QFETCH(bool, reject);
        int descriptors[2];
        QCOMPARE(::pipe2(descriptors, O_NONBLOCK | O_CLOEXEC), 0);
        const auto closeReader = qScopeGuard([&] { ::close(descriptors[0]); });
        Session session;
        session.setDiagnosticsEnabled(true);
        QVERIFY(SessionTestAccess::ownChild(session));
        auto& state = SessionTestAccess::state(session);
        state.master = descriptors[1]; // Owned synthetic nonblocking delivery channel.
        state.writer = new QSocketNotifier(state.master, QSocketNotifier::Write, &session);
        state.config = std::make_unique<QTemporaryDir>();
        QVERIFY(state.config->isValid());
        const QString config = state.config->path();
        const QByteArray reply = authorization ? QByteArray("https://fixture.invalid/?code=synthetic\n") : QByteArray("synthetic-pin\n");
        const QByteArray prefix = reply.left(5);
        QCOMPARE(::write(state.master, prefix.constData(), prefix.size()), ssize_t(prefix.size()));
        // Model the exact retained suffix after a successful partial flush: the
        // delivered prefix has already been wiped, while written tracks its length.
        // Public PTY pressure rows separately establish real queue/resume behavior.
        state.input = reply;
        ::explicit_bzero(state.input.data(), prefix.size());
        state.written = prefix.size();
        state.authTransaction = OAuthContract::Request{QUrl(), QString(64, 'a'), QString(43, 'b'), state.generation};
        state.awaiting = authorization ? SessionTestAccess::Awaiting::Authorization : SessionTestAccess::Awaiting::Pin;
        state.authTimer.start(10000);
        state.touchTimer.start(10000);
        state.windowTimer.start(10000);
        const quint64 generation = state.generation;
        QSignalSpy errors(&session, &Session::error);
        QSignalSpy ended(&session, &Session::ended);
        struct RejectionSnapshot {
            QString notification;
            QByteArray input;
            qsizetype written;
            bool writerDisabled, authorityInvalid, timersStopped, terminal, configRetained, active;
        };
        QList<RejectionSnapshot> snapshots;
        auto snapshot = [&](const QString& notification) {
            snapshots.append({notification, state.input, state.written, !state.writer->isEnabled(),
                !state.authTransaction && state.generation > generation && state.awaiting == SessionTestAccess::Awaiting::Nothing,
                !state.authTimer.isActive() && !state.touchTimer.isActive() && !state.windowTimer.isActive(),
                state.failed && state.stopping && state.terminalNotifications, QFileInfo::exists(config), session.active()});
        };
        connect(&session, &Session::diagnosticEvent, &session, [&](const QString& event) {
            if (event == "connection-error" || event == "transport-stop") snapshot("diagnostic:" + event);
        });
        connect(&session, &Session::statusChanged, &session, [&](const QString& phase) {
            if (phase == "error") snapshot("status:error");
        });
        connect(&session, &Session::error, &session, [&] { snapshot("error"); });
        if (reject) SessionTestAccess::reject(session);
        else SessionTestAccess::flush(session); // Counterfactual must deliver the complete retained suffix.
        char buffer[4096];
        const ssize_t count = ::read(descriptors[0], buffer, sizeof(buffer));
        QVERIFY(count > 0);
        QCOMPARE(QByteArray(buffer, count), reject ? prefix : reply);
        ::explicit_bzero(buffer, sizeof(buffer));
        if (reject) {
            QStringList observed;
            for (const auto& sample : snapshots) observed.append(sample.notification);
            QCOMPARE(observed, (QStringList{"diagnostic:connection-error", "diagnostic:transport-stop", "status:error", "error"}));
            for (const auto& sample : snapshots) {
                QCOMPARE(sample.input, QByteArray{});
                QCOMPARE(sample.written, qsizetype(0));
                QVERIFY2(sample.writerDisabled, qPrintable(sample.notification));
                QVERIFY2(sample.authorityInvalid, qPrintable(sample.notification));
                QVERIFY2(sample.timersStopped, qPrintable(sample.notification));
                QVERIFY2(sample.terminal, qPrintable(sample.notification));
                QVERIFY2(sample.configRetained && sample.active, qPrintable(sample.notification));
            }
            QCOMPARE(errors.count(), 1);
            QCOMPARE(errors.first().first().toString(), certificateStoppedMessage());
        } else {
            QVERIFY(snapshots.isEmpty());
            QCOMPARE(errors.count(), 0);
        }
        session.stop();
        QTRY_VERIFY_WITH_TIMEOUT((SessionTestAccess::reap(session), ended.count() == 1), 3000);
        QVERIFY(!session.active());
        QVERIFY(!QFileInfo::exists(config));
    }
    void executableLaunchFailure_data()
    {
        QTest::addColumn<bool>("cancel");
        QTest::newRow("normal-failure") << false;
        QTest::newRow("cancel-pending-failure") << true;
    }
    void executableLaunchFailure()
    {
        QFETCH(bool, cancel);
        QTemporaryDir sandbox(QDir::tempPath() + QStringLiteral("/omawin365-launch-test-XXXXXX"));
        QVERIFY(sandbox.isValid());
        const QString executable = sandbox.filePath(QStringLiteral("xfreerdp3"));
        const QString interpreter = sandbox.filePath(QStringLiteral("nonexistent-interpreter"));
        const QString profile = sandbox.filePath(QStringLiteral("nonexistent-profile.rdp"));
        QVERIFY(!QFileInfo::exists(interpreter));
        QVERIFY(!QFileInfo::exists(profile));
        QFile fixture(executable);
        QVERIFY(fixture.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        // Executable lookup succeeds, but exec fails before any script can run.
        const QByteArray shebang = "#!" + QFile::encodeName(interpreter) + '\n';
        QCOMPARE(fixture.write(shebang), qint64(shebang.size()));
        fixture.close();
        QVERIFY(fixture.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                      QFileDevice::ExeOwner));
        const bool hadPath = qEnvironmentVariableIsSet("PATH");
        const QByteArray oldPath = qgetenv("PATH");
        const auto restorePath = qScopeGuard([&] {
            if (hadPath)
                qputenv("PATH", oldPath);
            else
                qunsetenv("PATH");
        });
        QVERIFY(qputenv("PATH", QFile::encodeName(sandbox.path())));
        QCOMPARE(QStandardPaths::findExecutable(QStringLiteral("xfreerdp3")), executable);

        Session session;
        QSignalSpy ended(&session, &Session::ended);
        QSignalSpy errors(&session, &Session::error);
        const auto& state = SessionTestAccess::state(session);
        QSignalSpy launchErrors(&state.versionProcess, &QProcess::errorOccurred);
        QSignalSpy finished(&state.versionProcess, &QProcess::finished);
        QSignalSpy started(&state.versionProcess, &QProcess::started);
        QStringList processTrace, lifecycle;
        connect(&state.versionProcess, &QProcess::started, &session,
            [&] { processTrace.append(QStringLiteral("started")); });
        connect(&state.versionProcess, &QProcess::errorOccurred, &session, [&](QProcess::ProcessError value) {
            processTrace.append(QStringLiteral("error:%1").arg(int(value)));
        });
        connect(&state.versionProcess, &QProcess::finished, &session,
            [&] { processTrace.append(QStringLiteral("finished")); });
        connect(&session, &Session::statusChanged, &session,
            [&](const QString& phase) { lifecycle.append(QStringLiteral("status:") + phase); });
        connect(&session, &Session::error, &session, [&] { lifecycle.append(QStringLiteral("error")); });
        connect(&session, &Session::ended, &session, [&] {
            lifecycle.append(session.active() ? QStringLiteral("ended-active") : QStringLiteral("ended"));
        });
        session.start(profile);
        const bool failurePending = state.versionCheck == SessionTestAccess::VersionCheck::Checking;
        // Deliberately no event processing between start and synchronous cancellation.
        if (cancel)
            session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(ended.count(), 1, 3000);
        QVERIFY(!session.active());
        QCOMPARE(state.versionProcess.state(), QProcess::NotRunning);
        const bool cancelledPending = cancel && failurePending;
        const int firstErrors = cancelledPending ? 0 : 1;
        QCOMPARE(errors.count(), firstErrors);
        if (firstErrors)
            QCOMPARE(errors.at(0).at(0).toString(), QStringLiteral("Could not inspect the installed stock FreeRDP executable."));
        QCOMPARE(launchErrors.count(), 1);
        const auto launchError = launchErrors.at(0).at(0).value<QProcess::ProcessError>();
        if (cancelledPending && launchError == QProcess::Crashed) {
            // SIGKILL can win before exec reports its missing interpreter.
            // Qt then observes startup-pipe EOF followed by signal termination,
            // not an exec failure. Only this exact cancellation trace is valid.
            QCOMPARE(started.count(), 1);
            QCOMPARE(finished.count(), 1);
            QCOMPARE(finished.at(0).at(0).toInt(), int(SIGKILL));
            QCOMPARE(finished.at(0).at(1).value<QProcess::ExitStatus>(), QProcess::CrashExit);
            QCOMPARE(processTrace, (QStringList{QStringLiteral("started"),
                QStringLiteral("error:%1").arg(int(QProcess::Crashed)), QStringLiteral("finished")}));
        } else {
            // Without cancellation this remains strictly a failed exec; with
            // cancellation it means the exec failure report won the race.
            QCOMPARE(launchError, QProcess::FailedToStart);
            QCOMPARE(started.count(), 0);
            QCOMPARE(finished.count(), 0);
            QCOMPARE(processTrace, QStringList{QStringLiteral("error:%1").arg(int(QProcess::FailedToStart))});
        }
        const int firstFinished = finished.count();
        const int firstStarted = started.count();
        QStringList expectedLifecycle;
        if (failurePending)
            expectedLifecycle.append(QStringLiteral("status:connecting"));
        if (cancelledPending)
            expectedLifecycle.append(QStringLiteral("status:disconnected"));
        else
            expectedLifecycle.append({QStringLiteral("status:error"), QStringLiteral("error")});
        expectedLifecycle.append(QStringLiteral("ended"));
        QCOMPARE(lifecycle, expectedLifecycle);
        session.stop();
        QCoreApplication::processEvents();
        QCOMPARE(ended.count(), 1);
        QCOMPARE(errors.count(), firstErrors);
        QCOMPARE(launchErrors.count(), 1);
        QCOMPARE(finished.count(), firstFinished);
        QCOMPARE(started.count(), firstStarted);
        QCOMPARE(lifecycle, expectedLifecycle);

        // A terminal failed/cancelled attempt must not block a fresh launch.
        processTrace.clear();
        lifecycle.clear();
        session.start(profile);
        const bool freshFailurePending = state.versionCheck == SessionTestAccess::VersionCheck::Checking;
        QTRY_COMPARE_WITH_TIMEOUT(ended.count(), 2, 3000);
        QVERIFY(!session.active());
        QCOMPARE(errors.count(), firstErrors + 1);
        QCOMPARE(errors.last().at(0).toString(), QStringLiteral("Could not inspect the installed stock FreeRDP executable."));
        QCOMPARE(launchErrors.count(), 2);
        QCOMPARE(launchErrors.at(1).at(0).value<QProcess::ProcessError>(), QProcess::FailedToStart);
        QCOMPARE(finished.count(), firstFinished);
        QCOMPARE(started.count(), firstStarted);
        QCOMPARE(processTrace, QStringList{QStringLiteral("error:%1").arg(int(QProcess::FailedToStart))});
        expectedLifecycle.clear();
        if (freshFailurePending)
            expectedLifecycle.append(QStringLiteral("status:connecting"));
        expectedLifecycle.append({QStringLiteral("status:error"), QStringLiteral("error"), QStringLiteral("ended")});
        QCOMPARE(lifecycle, expectedLifecycle);
        session.stop();
        QCoreApplication::processEvents();
        QCOMPARE(ended.count(), 2);
        QCOMPARE(errors.count(), firstErrors + 1);
        QCOMPARE(launchErrors.count(), 2);
        QCOMPARE(finished.count(), firstFinished);
        QCOMPARE(started.count(), firstStarted);
        QCOMPARE(lifecycle, expectedLifecycle);
    }
};

QTEST_GUILESS_MAIN(SessionTest)
#include "tst_session.moc"
