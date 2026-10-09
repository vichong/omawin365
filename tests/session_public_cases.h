#pragma once
#include "session_fixture.h"
#include "certificate_fixture.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QSocketNotifier>
#include <fcntl.h>
#include "auth_composed.h"
#include "../src/oauthcontract.h"
#include "../src/profilestore.h"
#include <QStringConverter>

// Public parser/PTY cases complement retained non-certificate private regressions.
class SessionPublicTest : public QObject {
    Q_OBJECT
    QString configPath(const SessionFixture& fixture, int transport = 0) const
    {
        const auto records = fixture.events(QStringLiteral("transport"));
        if (records.size() <= transport) return {};
        const QString tune = records.at(transport).value(QStringLiteral("argv")).toArray().last().toString();
        const QString marker = QStringLiteral("FreeRDP_ConfigPath:");
        return tune.contains(marker) ? tune.mid(tune.lastIndexOf(marker) + marker.size()) : QString{};
    }
    void noCertificateException(const SessionFixture& fixture)
    {
        // Fixed argument count and explicit exception absence complement the
        // byte-exact publicRestrictedLaunchContract oracle.
        for (const auto& transport : fixture.events(QStringLiteral("transport"))) {
            const auto args = transport.value(QStringLiteral("argv")).toArray();
            QCOMPARE(args.size(), 12);
            for (const auto& arg : args) {
                QVERIFY(!arg.toString().startsWith(QStringLiteral("/cert:")));
                QVERIFY(!arg.toString().contains(QString::fromLatin1(fixtureFingerprint())));
            }
            const QString tune = args.last().toString();
            for (const char* flag : {"FreeRDP_IgnoreCertificate:FALSE", "FreeRDP_AutoAcceptCertificate:FALSE",
                "FreeRDP_AutoDenyCertificate:FALSE", "FreeRDP_ExternalCertificateManagement:FALSE",
                "FreeRDP_CertificateCallbackPreferPEM:FALSE", "FreeRDP_AuthenticationLevel:2"})
                QVERIFY(tune.contains(QString::fromLatin1(flag)));
        }
    }
    void singleCertificateDenial(SessionFixture& fixture)
    {
        QCOMPARE(fixture.errors.count(), 1);
        QCOMPARE(fixture.errors.first().first().toString(), certificateStoppedMessage());
        QCOMPARE(fixture.ended.count(), 1);
        QVERIFY(!fixture.session.active());
        QCOMPARE(fixture.input(), QByteArray{});
        QCOMPARE(fixture.pins.count(), 0);
        QCOMPARE(fixture.auth.count(), 0);
        QCOMPARE(fixture.events(QStringLiteral("version")).size(), 1);
        QCOMPARE(fixture.events(QStringLiteral("buildconfig")).size(), 1);
        QCOMPARE(fixture.events(QStringLiteral("transport")).size(), 1);
        noCertificateException(fixture);
        QCOMPARE(fixture.lifecycle, (QStringList{"status:connecting", "status:connecting", "status:error", "error", "ended"}));
        QCOMPARE(fixture.statuses.last().at(1).toString(), certificateStoppedMessage());
        QCOMPARE(fixture.notifications, (QStringList{"diagnostic:connection-start", "status:connecting",
            "diagnostic:transport-start", "status:connecting", "diagnostic:connection-error",
            "diagnostic:transport-stop", "status:error", "error", "diagnostic:transport-exit", "ended"}));
        QStringList diagnosticEvents;
        for (const auto& signal : fixture.diagnostics) diagnosticEvents.append(signal.first().toString());
        QCOMPARE(diagnosticEvents, (QStringList{"connection-start", "transport-start", "connection-error", "transport-stop", "transport-exit"}));
        QVERIFY(!fixtureHasPhase(fixture.statuses, QStringLiteral("verifying-certificate")));
        QVERIFY(!fixtureHasPhase(fixture.statuses, QStringLiteral("confirming-certificate")));
        for (const auto* spy : {&fixture.statuses, &fixture.errors, &fixture.diagnostics})
            for (const auto& signal : *spy)
                for (const auto& value : signal) {
                    QVERIFY(!value.toString().contains(QStringLiteral("fixture.example")));
                    QVERIFY(!value.toString().contains(QString::fromLatin1(fixtureFingerprint())));
                }
    }
private slots:
    void initTestCase()
    {
        QVERIFY2(QFileInfo(QStringLiteral("/usr/bin/python3")).isExecutable(),
            "Session public tests require executable /usr/bin/python3 with isolated (-I) mode.");
        QProcess python;
        python.start(QStringLiteral("/usr/bin/python3"), {QStringLiteral("-I"), QStringLiteral("-c"),
            QStringLiteral("import base64,json,os,pathlib,select,signal,termios,time; assert hasattr(os, 'set_blocking')")});
        QVERIFY2(python.waitForStarted(3000) && python.waitForFinished(3000) &&
            python.exitStatus() == QProcess::NormalExit && python.exitCode() == 0,
            "Session public tests require working /usr/bin/python3 -I and standard-library PTY/JSON support.");
        QVERIFY2(!qEnvironmentVariableIsEmpty("DISPLAY"),
            "Session public tests require an existing X11/XWayland DISPLAY; they never start a display.");
        QVERIFY2(sessionFixtureDisplayAvailable(),
            "Session public tests cannot open the existing DISPLAY read-only; check display availability/access.");
    }
    void composedAuthorization_data()
    {
        QTest::addColumn<QString>("code");
        QTest::addColumn<QByteArray>("encoded");
        QTest::newRow("literal-plus") << QStringLiteral("a+b") << QByteArray("a%20b");
        QTest::newRow("escaped-plus") << QStringLiteral("a%2Bb") << QByteArray("a%2Bb");
        QTest::newRow("space-and-delimiters") << QStringLiteral("a%20b%26%3D") << QByteArray("a%20b%26%3D");
        QTest::newRow("unicode-once") << QStringLiteral("%E2%82%AC%252B") << QByteArray("%E2%82%AC%252B");
        QTest::newRow("decoded-code-key") << QStringLiteral("fixture") << QByteArray("fixture");
        QTest::newRow("exact-4095-escaped") << (QString(3954, 'x') + "%2B") << (QByteArray(3954, 'x') + "%2B");
        QTest::newRow("exact-4095-unicode") << (QString(3948, 'x') + "%E2%82%AC") << (QByteArray(3948, 'x') + "%E2%82%AC");
        QTest::newRow("exact-4095") << QString(3957, 'x') << QByteArray(3957, 'x');
    }
    void composedAuthorization()
    {
        QFETCH(QString, code);
        QFETCH(QByteArray, encoded);
        SessionFixture fixture;
        QVERIFY(fixture.init());
        BrowserAuth browser;
        QVERIFY(BrowserAuthTestAccess::inertTransport(browser));
        QSignalSpy callbacks(&browser, &BrowserAuth::callbackReady);
        QSignalSpy failures(&browser, &BrowserAuth::failed);
        QObject::connect(&fixture.session, &Session::authRequested, &browser, &BrowserAuth::begin);
        QObject::connect(&browser, &BrowserAuth::callbackReady, &fixture.session, &Session::submitAuthResult);
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        QVERIFY(fixture.output(fixtureAuthorization()) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.auth.count(), 1, 3000);
        BrowserAuthTestAccess::adoptPage(browser);
        const auto transaction = fixture.auth.last().at(0).value<OAuthContract::Request>();
        QVERIFY(transaction.generation > 0);
        // A correctly encoded callback from an older application generation is inert.
        const QUrl url = fixtureCallback((QByteArray(QTest::currentDataTag()) == "decoded-code-key" ? "%63ode=" : "code=") + code + "&state=" + transaction.state + "&session_state=discarded");
        fixture.session.submitAuthResult({url, transaction.generation - 1});
        QCOMPARE(fixture.input(), QByteArray{});
        BrowserAuthTestAccess::callback(browser, url);
        QTRY_COMPARE_WITH_TIMEOUT(callbacks.count(), 1, 3000);
        const QByteArray expected = "https://login.microsoftonline.com/common/oauth2/nativeclient?code=" + encoded + "&state=" + transaction.state.toUtf8() + '\n';
        if (QByteArray(QTest::currentDataTag()).startsWith("exact-4095")) QCOMPARE(expected.size(), 4095);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.input(), expected, 3000);
        const auto result = callbacks.last().at(0).value<OAuthContract::Callback>();
        QCOMPARE(result.generation, transaction.generation);
        fixture.session.submitAuthResult(result);
        BrowserAuthTestAccess::callback(browser, url);
        QTest::qWait(100);
        QCOMPARE(callbacks.count(), 1);
        QCOMPARE(fixture.input(), expected);
        QCOMPARE(failures.count(), 0);
        QCOMPARE(result.url.toEncoded() + '\n', expected); // Metadata is not returned even inside the app.
        // Same transport, same synthetic state, different round: generation alone rejects the stale result.
        QVERIFY(fixture.output("\n" + fixtureAuthorization()) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.auth.count(), 2, 3000);
        const auto next = fixture.auth.last().at(0).value<OAuthContract::Request>();
        QVERIFY(next.generation > transaction.generation);
        fixture.session.submitAuthResult(result);
        QTest::qWait(100);
        QCOMPARE(fixture.input(), expected);
        BrowserAuthTestAccess::adoptPage(browser);
        BrowserAuthTestAccess::callback(browser, url);
        QTRY_COMPARE_WITH_TIMEOUT(callbacks.count(), 2, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.input(), expected + expected, 3000);
        fixture.session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        // Replacement gets a new generation; the old result must not satisfy it.
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 2, 3000);
        QVERIFY(fixture.output(fixtureAuthorization()) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.auth.count(), 3, 3000);
        fixture.session.submitAuthResult(result);
        QTest::qWait(100);
        QCOMPARE(fixture.input(), expected + expected);
        fixture.session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 2, 3000);
        browser.cancel();
    }
    void composedCallbackFailure_data()
    {
        QTest::addColumn<QString>("query");
        QTest::newRow("legacy-code-only") << QStringLiteral("code=fixture");
        QTest::newRow("encoded-duplicate") << QStringLiteral("code=one&%63ode=two");
        QTest::newRow("error") << QStringLiteral("error=access_denied");
        QTest::newRow("mixed-error") << QStringLiteral("code=one&error=denied");
        QTest::newRow("invalid-utf8") << QStringLiteral("code=%C0%AF");
        QTest::newRow("incomplete-two-byte-utf8") << QStringLiteral("code=fixture%C3");
        QTest::newRow("incomplete-three-byte-utf8") << QStringLiteral("code=fixture%E2%82");
        QTest::newRow("incomplete-four-byte-utf8") << QStringLiteral("code=fixture%F0%9F%92");
        QTest::newRow("control") << QStringLiteral("code=%C2%85");
        QTest::newRow("overflow-4096") << (QStringLiteral("code=") + QString(3958, 'x'));
    }
    void composedCallbackFailure()
    {
        QFETCH(QString, query);
        SessionFixture fixture;
        QVERIFY(fixture.init());
        BrowserAuth browser;
        QVERIFY(BrowserAuthTestAccess::inertTransport(browser));
        QSignalSpy failures(&browser, &BrowserAuth::failed);
        QSignalSpy callbacks(&browser, &BrowserAuth::callbackReady);
        QObject::connect(&fixture.session, &Session::authRequested, &browser, &BrowserAuth::begin);
        QObject::connect(&browser, &BrowserAuth::callbackReady, &fixture.session, &Session::submitAuthResult);
        QObject::connect(&browser, &BrowserAuth::failed, &fixture.session, &Session::stop);
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        QVERIFY(fixture.output(fixtureAuthorization()) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.auth.count(), 1, 3000);
        BrowserAuthTestAccess::adoptPage(browser);
        const auto transaction = fixture.auth.last().at(0).value<OAuthContract::Request>();
        if (QByteArray(QTest::currentDataTag()) != "legacy-code-only") query += "&state=" + transaction.state;
        BrowserAuthTestAccess::callback(browser, fixtureCallback(query));
        QCOMPARE(callbacks.count(), 0);
        QCOMPARE(failures.count(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QCOMPARE(fixture.input(), QByteArray{});
        browser.cancel();
    }
    void publicBuildconfigGate_data()
    {
        QTest::addColumn<QString>("config");
        QTest::addColumn<bool>("supported");
        QTest::newRow("off") << QStringLiteral("Build configuration: WITH_AAD=ON WITH_SSO_MIB=OFF\n") << true;
        QTest::newRow("missing") << QStringLiteral("Build configuration: WITH_AAD=ON\n") << false;
        QTest::newRow("on") << QStringLiteral("Build configuration: WITH_SSO_MIB=ON\n") << false;
        QTest::newRow("unknown") << QStringLiteral("Build configuration: WITH_SSO_MIB=0\n") << false;
        QTest::newRow("duplicate") << QStringLiteral("Build configuration: WITH_SSO_MIB=OFF WITH_SSO_MIB=OFF\n") << false;
        QTest::newRow("conflict") << QStringLiteral("Build configuration: WITH_SSO_MIB=OFF WITH_SSO_MIB=ON\n") << false;
        QTest::newRow("embedded") << QStringLiteral("Build configuration: XWITH_SSO_MIB=OFF\n") << false;
        QTest::newRow("oversized") << (QStringLiteral("Build configuration: WITH_SSO_MIB=OFF\n") + QString(32768, 'x')) << false;
    }
    void publicBuildconfigGate()
    {
        QFETCH(QString, config);
        QFETCH(bool, supported);
        SessionFixture fixture;
        QVERIFY(fixture.init({{"buildconfig", config}}));
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("buildconfig")).size(), 1, 3000);
        if (supported) {
            QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
            fixture.session.stop();
        }
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QCOMPARE(fixture.errors.count(), supported ? 0 : 1);
        QCOMPARE(fixture.events(QStringLiteral("transport")).size(), supported ? 1 : 0);
    }
    void publicBuildconfigLifecycle_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const char* name : {"cancel", "timeout", "crash", "exit", "conflicting-stderr"}) QTest::newRow(name) << QString::fromLatin1(name);
    }
    void publicBuildconfigLifecycle()
    {
        QFETCH(QString, scenario);
        SessionFixture fixture;
        QJsonObject settings;
        if (scenario == "conflicting-stderr") settings.insert("buildconfig_stderr", "WITH_SSO_MIB=ON\n");
        else if (scenario == "crash") settings.insert("buildconfig_crash", true);
        else if (scenario == "exit") settings.insert("buildconfig_exit", 1);
        else settings.insert("buildconfig_delay", 8);
        QVERIFY(fixture.init(settings));
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("buildconfig")).size(), 1, 3000);
        if (scenario == "cancel") fixture.session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 7000);
        QCOMPARE(fixture.errors.count(), scenario == "cancel" ? 0 : 1);
        QCOMPARE(fixture.events(QStringLiteral("transport")).size(), 0);
        const pid_t pid = fixture.events(QStringLiteral("buildconfig")).front().value("pid").toInt();
        QVERIFY(::kill(pid, 0) < 0 && errno == ESRCH);
        QVERIFY(fixture.write(QStringLiteral("settings.json"), "{}"));
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        fixture.session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 2, 3000);
    }
    void publicSplitCommandWaitsForNewline()
    {
        SessionFixture fixture;
        QVERIFY(fixture.init());
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        const int sequence = 999;
        const QByteArray record = QJsonDocument(QJsonObject{{QStringLiteral("sequence"), sequence},
            {QStringLiteral("bytes"), QString::fromLatin1(QByteArray("FIDO2 PIN: ").toBase64())}}).toJson(QJsonDocument::Compact);
        QVERIFY(fixture.appendCommand(record.left(5))); // Invalid JSON prefix, deliberately no newline.
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("partial-command")).size(), 1, 3000);
        QCOMPARE(fixture.events(QStringLiteral("partial-command")).at(0).value(QStringLiteral("length")).toInt(), 5);
        QTest::qWait(100);
        QCOMPARE(fixture.pins.count(), 0);
        QCOMPARE(fixture.ended.count(), 0);
        QVERIFY(fixture.session.active());
        QVERIFY(fixture.appendCommand(record.mid(5))); // Complete JSON is still not a complete record.
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("partial-command")).size(), 2, 3000);
        QCOMPARE(fixture.events(QStringLiteral("partial-command")).at(1).value(QStringLiteral("length")).toInt(), int(record.size()));
        QTest::qWait(100);
        QVERIFY(!fixture.delivered(sequence));
        QCOMPARE(fixture.pins.count(), 0);
        QCOMPARE(fixture.ended.count(), 0);
        QCOMPARE(fixture.errors.count(), 0);
        QVERIFY(fixture.appendCommand("\n"));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.delivered(sequence), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.pins.count(), 1, 3000);
        QCOMPARE(fixture.events(QStringLiteral("output")).size(), 1);
        fixture.session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QCOMPARE(fixture.errors.count(), 0);
    }
    void publicVersionGate_data()
    {
        QTest::addColumn<QString>("version");
        QTest::addColumn<int>("code");
        QTest::addColumn<bool>("supported");
        QTest::newRow("stock") << QStringLiteral("This is FreeRDP version 3.32.1\n") << 0 << true;
        QTest::newRow("package-prefix") << QStringLiteral("This is FreeRDP version [fixture] 3.32.1 (build)\n") << 0 << true;
        QTest::newRow("legacy-3.31.1") << QStringLiteral("This is FreeRDP version 3.31.1\n") << 0 << false;
        QTest::newRow("wrong-version") << QStringLiteral("This is FreeRDP version 3.31.2\n") << 0 << false;
        QTest::newRow("suffix-not-version") << QStringLiteral("This is FreeRDP version 3.32.10\n") << 0 << false;
        QTest::newRow("nonzero") << QStringLiteral("This is FreeRDP version 3.32.1\n") << 1 << false;
        QTest::newRow("oversized") << (QStringLiteral("This is FreeRDP version 3.32.1\n") + QString(4096, 'x')) << 0 << false;
    }
    void publicVersionGate()
    {
        QFETCH(QString, version);
        QFETCH(int, code);
        QFETCH(bool, supported);
        SessionFixture fixture;
        QVERIFY(fixture.init({{"version", version}, {"version_exit", code}}));
        QVERIFY(!fixture.session.active());
        fixture.session.start(fixture.profile());
        QVERIFY(fixture.session.active());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("version")).size(), 1, 3000);
        if (supported) {
            QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
            QCOMPARE(fixture.errors.count(), 0);
            QCOMPARE(fixture.ended.count(), 0);
            QVERIFY(fixtureHasPhase(fixture.statuses, QStringLiteral("connecting")));
            fixture.session.stop();
            QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
            QCOMPARE(fixture.errors.count(), 1);
            QCOMPARE(fixture.errors.at(0).at(0).toString(), QStringLiteral("FreeRDP 3.32.1 built with WITH_SSO_MIB=OFF is required (the omawin365-freerdp package); executable verification failed."));
        }
        QVERIFY(!fixture.session.active());
        QTest::qWait(100);
        QCOMPARE(fixture.events(QStringLiteral("transport")).size(), supported ? 1 : 0);
        QCOMPARE(fixture.pins.count(), 0);
        QCOMPARE(fixture.auth.count(), 0);
        fixture.session.stop();
        QTest::qWait(100);
        QCOMPARE(fixture.ended.count(), 1);
    }
    void publicCancelVersionAndFreshAttempt()
    {
        SessionFixture fixture;
        QVERIFY(fixture.init({{"version_delay", 0.3}}));
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("version")).size(), 1, 3000);
        fixture.session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QVERIFY(!fixture.session.active());
        QTest::qWait(400); // Beyond the cancelled version child's scheduled reply.
        QCOMPARE(fixture.events(QStringLiteral("transport")).size(), 0);
        QCOMPARE(fixture.errors.count(), 0);
        QVERIFY(fixtureHasPhase(fixture.statuses, QStringLiteral("disconnected")));
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        QCOMPARE(fixture.events(QStringLiteral("version")).size(), 2);
        QCOMPARE(fixture.ended.count(), 1);
        fixture.session.start(fixture.profile());
        QCOMPARE(fixture.errors.count(), 1);
        QCOMPARE(fixture.errors.at(0).at(0).toString(), QStringLiteral("Disconnect the current desktop before starting another session."));
        fixture.session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 2, 3000);
        QVERIFY(!fixture.session.active());
        QCOMPARE(fixture.events(QStringLiteral("transport")).size(), 1);
    }
    void publicPinIsIncrementalAndOneShot()
    {
        SessionFixture fixture;
        QVERIFY(fixture.init());
        fixture.session.submitPin(QStringLiteral("fixture-pin")); // No attempt/challenge yet.
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        fixture.session.submitPin(QStringLiteral("fixture-pin")); // Active, but not waiting.
        const int partial = fixture.output("FIDO2 ");
        QVERIFY(partial > 0);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.delivered(partial), 3000);
        QTest::qWait(100);
        QCOMPARE(fixture.pins.count(), 0);
        QCOMPARE(fixture.input(), QByteArray{});
        QVERIFY(fixture.output("PIN: ") > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.pins.count(), 1, 3000);
        QVERIFY(fixtureHasPhase(fixture.statuses, QStringLiteral("awaiting-pin")));
        fixture.session.submitPin(QStringLiteral("fixture-pin"));
        fixture.session.submitPin(QStringLiteral("duplicate-pin"));
        fixture.submitAuthUrl(fixtureCallback());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.input(), QByteArray("fixture-pin\n"), 3000);
        QTest::qWait(150);
        QCOMPARE(fixture.input(), QByteArray("fixture-pin\n"));
        QCOMPARE(fixture.errors.count(), 0);
        QVERIFY(fixtureHasPhase(fixture.statuses, QStringLiteral("awaiting-touch")));
        QCOMPARE(fixture.diagnostics.count(), 4); // Start, transport, request, submitted.
        QCOMPARE(fixture.diagnostics.at(2).at(0).toString(), QStringLiteral("native-pin-request"));
        QCOMPARE(fixture.diagnostics.at(3).at(0).toString(), QStringLiteral("native-pin-submitted"));
        // A second actual challenge is distinct from a duplicate submission.
        QVERIFY(fixture.output("\nFIDO2 PIN: ") > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.pins.count(), 2, 3000);
        fixture.session.submitPin(QStringLiteral("second-pin"));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.input(), QByteArray("fixture-pin\nsecond-pin\n"), 3000);
        fixture.session.stop();
        fixture.session.submitPin(QStringLiteral("late-pin"));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QCOMPARE(fixture.input(), QByteArray("fixture-pin\nsecond-pin\n"));
        QVERIFY(!fixture.session.active());
    }
    void publicPinValidation_data()
    {
        QTest::addColumn<QString>("invalid");
        QTest::addColumn<QString>("valid");
        QTest::newRow("too-short") << QStringLiteral("123") << QStringLiteral("1234");
        QTest::newRow("too-many-bytes") << QString(64, 'x') << QString(63, 'x');
        QTest::newRow("utf8-byte-limit") << QString(32, QChar(0x00e9)) << QString(31, QChar(0x00e9));
        QTest::newRow("too-few-characters") << QString(3, QChar(0x00e9)) << QString(4, QChar(0x00e9));
        QTest::newRow("newline") << QStringLiteral("1234\n") << QStringLiteral("valid-pin");
        QTest::newRow("carriage-return") << QStringLiteral("1234\r") << QStringLiteral("valid-pin");
        QTest::newRow("nul") << (QStringLiteral("1234") + QChar(0)) << QStringLiteral("valid-pin");
        QTest::newRow("delete") << (QStringLiteral("1234") + QChar(0x7f)) << QStringLiteral("valid-pin");
    }
    void publicPinValidation()
    {
        QFETCH(QString, invalid);
        QFETCH(QString, valid);
        SessionFixture fixture;
        QVERIFY(fixture.init());
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        QVERIFY(fixture.output("FIDO2 PIN: ") > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.pins.count(), 1, 3000);
        fixture.session.submitPin(invalid);
        QCOMPARE(fixture.errors.count(), 1);
        QCOMPARE(fixture.errors.at(0).at(0).toString(), QStringLiteral("The security key PIN must have at least four characters, at most 63 UTF-8 bytes, and no control characters."));
        QTest::qWait(100);
        QCOMPARE(fixture.input(), QByteArray{});
        QVERIFY(fixture.session.active());
        QCOMPARE(fixture.ended.count(), 0);
        fixture.session.submitPin(valid); // Invalid entry must leave the challenge waiting.
        QTRY_COMPARE_WITH_TIMEOUT(fixture.input(), valid.toUtf8() + '\n', 3000);
        fixture.session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QCOMPARE(fixture.errors.count(), 1);
    }
    void publicAuthorizationRound_data()
    {
        QTest::addColumn<QByteArray>("stateQuery");
        QTest::addColumn<QString>("callbackQuery");
        QTest::addColumn<QByteArray>("expected");
        QTest::newRow("matching-state") << QByteArray("&state=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") << QStringLiteral("code=fixture-code&state=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa&session_state=ignored") << QByteArray("fixture-code");
        QTest::newRow("no-request-state") << QByteArray{} << QStringLiteral("code=fixture-code&state=unsolicited") << QByteArray("fixture-code");
        QTest::newRow("present-empty-state") << QByteArray("&state=") << QStringLiteral("code=fixture-code&state=") << QByteArray("fixture-code");
        QTest::newRow("encoded-state") << QByteArray("&state=a%20b%2Bc") << QStringLiteral("code=fixture-code&state=a%20b%2Bc") << QByteArray("fixture-code");
        QTest::newRow("literal-plus-state") << QByteArray("&state=a+b") << QStringLiteral("code=fixture-code&state=a%2Bb") << QByteArray("fixture-code");
        QTest::newRow("encoded-code") << QByteArray("&state=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") << QStringLiteral("code=a%2Bb%20c%26d&state=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") << QByteArray("a%2Bb%20c%26d");
    }
    void publicAuthorizationRound()
    {
        QFETCH(QByteArray, stateQuery);
        QFETCH(QString, callbackQuery);
        QFETCH(QByteArray, expected);
        SessionFixture fixture;
        QVERIFY(fixture.init());
        fixture.submitAuthUrl(fixtureCallback());
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        fixture.submitAuthUrl(fixtureCallback()); // Active but no request.
        QByteArray request = fixtureAuthorization(stateQuery);
        request.chop(10); // Split the final "URL here: " from the prompt.
        const int partial = fixture.output(request);
        QVERIFY(partial > 0);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.delivered(partial), 3000);
        QTest::qWait(100);
        QCOMPARE(fixture.auth.count(), 0);
        QCOMPARE(fixture.input(), QByteArray{});
        QVERIFY(fixture.output("URL here: ") > 0);
        if (!OAuthContract::request(QUrl::fromEncoded(fixtureAuthorization(stateQuery).split('\n').front().mid(11)))) {
            QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
            QCOMPARE(fixture.auth.count(), 0);
            QCOMPARE(fixture.errors.count(), 1);
            QCOMPARE(fixture.input(), QByteArray{});
            return; // Historical absent/empty/non-hex states explicitly reject.
        }
        QTRY_COMPARE_WITH_TIMEOUT(fixture.auth.count(), 1, 3000);
        QCOMPARE(fixture.auth.at(0).at(0).value<OAuthContract::Request>().authorization.toEncoded(),
            QByteArray("https://login.microsoftonline.com/common/oauth2/v2.0/authorize?client_id=a85cf173-4192-42f8-81fa-777a763e6e2c&response_type=code&scope=https%3A%2F%2Fwww.wvd.microsoft.com%2F.default+openid+profile+offline_access&code_challenge=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb&code_challenge_method=S256&redirect_uri=https%3A%2F%2Flogin.microsoftonline.com%2Fcommon%2Foauth2%2Fnativeclient") + stateQuery);
        QVERIFY(fixtureHasPhase(fixture.statuses, QStringLiteral("signing-in")));
        fixture.session.submitPin(QStringLiteral("not-a-pin-request"));
        fixture.submitAuthUrl(fixtureCallback(callbackQuery));
        fixture.submitAuthUrl(fixtureCallback(QStringLiteral("code=duplicate&state=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")));
        const QByteArray response = "https://login.microsoftonline.com/common/oauth2/nativeclient?code=" + expected + "&state=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n";
        QTRY_COMPARE_WITH_TIMEOUT(fixture.input(), response, 3000);
        QTest::qWait(100);
        QCOMPARE(fixture.input(), response); // Matching state retained; metadata stripped, duplicate ignored.
        QCOMPARE(fixture.errors.count(), 0);
        QCOMPARE(fixture.diagnostics.count(), 4);
        QCOMPARE(fixture.diagnostics.at(2).at(0).toString(), QStringLiteral("authorization-request"));
        QCOMPARE(fixture.diagnostics.at(3).at(0).toString(), QStringLiteral("authorization-submitted"));
        fixture.session.stop();
        fixture.submitAuthUrl(fixtureCallback());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QCOMPARE(fixture.input(), response);
    }
    void publicRejectsInvalidCallback_data()
    {
        QTest::addColumn<QByteArray>("stateQuery");
        QTest::addColumn<QUrl>("callback");
        QTest::newRow("wrong-state") << QByteArray("&state=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") << fixtureCallback(QStringLiteral("code=fixture-code&state=wrong"));
        QTest::newRow("missing-state") << QByteArray("&state=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") << fixtureCallback(QStringLiteral("code=fixture-code"));
        QTest::newRow("empty-state-must-be-present") << QByteArray("&state=") << fixtureCallback(QStringLiteral("code=fixture-code"));
        QTest::newRow("duplicate-state") << QByteArray{} << fixtureCallback(QStringLiteral("code=fixture-code&state=a&state=b"));
        QTest::newRow("duplicate-code") << QByteArray{} << fixtureCallback(QStringLiteral("code=a&code=b"));
        QTest::newRow("empty-code") << QByteArray{} << fixtureCallback(QStringLiteral("code="));
        QTest::newRow("error") << QByteArray{} << fixtureCallback(QStringLiteral("code=a&error=synthetic"));
        QTest::newRow("control-code") << QByteArray{} << fixtureCallback(QStringLiteral("code=a%0Ab"));
        QTest::newRow("oversized-code") << QByteArray{} << fixtureCallback(QStringLiteral("code=") + QString(4096, 'x'));
        QTest::newRow("wrong-origin") << QByteArray{} << QUrl(QStringLiteral("https://fixture.invalid/common/oauth2/nativeclient?code=a"));
        QTest::newRow("wrong-path") << QByteArray{} << QUrl(QStringLiteral("https://login.microsoftonline.com/other?code=a"));
    }
    void publicRejectsInvalidCallback()
    {
        QFETCH(QByteArray, stateQuery);
        QFETCH(QUrl, callback);
        SessionFixture fixture;
        QVERIFY(fixture.init());
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        QVERIFY(fixture.output(fixtureAuthorization()) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.auth.count(), 1, 3000);
        fixture.submitAuthUrl(callback);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QCOMPARE(fixture.errors.count(), 1);
        QCOMPARE(fixture.errors.at(0).at(0).toString(), QStringLiteral("Microsoft returned an invalid or mismatched sign-in callback; disconnected without submitting it."));
        QCOMPARE(fixture.input(), QByteArray{});
        QVERIFY(!fixture.session.active());
    }
    void publicCertificateDenied_data()
    {
        QTest::addColumn<bool>("changed");
        QTest::addColumn<QByteArray>("role");
        QTest::addColumn<QString>("delivery");
        for (const bool changed : {false, true}) {
            const QByteArray prefix = changed ? "changed-" : "normal-";
            for (const char* role : {"Server", "Gateway", "Redirect"})
                QTest::newRow((prefix + role).constData()) << changed << QByteArray(role) << QStringLiteral("whole");
            for (const char* delivery : {"crlf", "newline", "header-split", "fingerprint-split", "suffix-split", "bytewise"})
                QTest::newRow((prefix + delivery).constData()) << changed << QByteArray("Server") << QString::fromLatin1(delivery);
        }
    }
    void publicCertificateDenied()
    {
        QFETCH(bool, changed);
        QFETCH(QByteArray, role);
        QFETCH(QString, delivery);
        SessionFixture fixture;
        QVERIFY(fixture.init());
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        const QString config = configPath(fixture);
        QVERIFY(QFileInfo::exists(config));
        QByteArray prompt = fixtureCertificate(changed, role);
        if (delivery == "crlf") prompt.replace("\n", "\r\n");
        if (delivery == "newline") prompt += '\n';
        if (delivery == "bytewise") {
            // Command acknowledgements serialize the child writes, not necessarily
            // every parent read. All prefix bytes must remain non-interactive.
            for (qsizetype i = 0; i < prompt.size(); ++i) {
                const int sequence = fixture.output(prompt.mid(i, 1));
                QVERIFY(sequence > 0);
                QTRY_VERIFY_WITH_TIMEOUT(fixture.delivered(sequence) || fixture.ended.count(), 3000);
                if (i < prompt.indexOf("(Y/T/N)") + 6) {
                    QCOMPARE(fixture.errors.count(), 0);
                    QCOMPARE(fixture.pins.count(), 0);
                    QCOMPARE(fixture.auth.count(), 0);
                }
                if (fixture.ended.count()) break; // Final trailing space need not reach a terminated child.
            }
        } else if (delivery.endsWith("split")) {
            const qsizetype split = delivery == "header-split" ? prompt.indexOf('\n') / 2 :
                delivery == "fingerprint-split" ? prompt.indexOf(fixtureFingerprint()) + 47 :
                prompt.indexOf("(Y/T/N)") + 5;
            QVERIFY(split > 0 && split < prompt.size());
            const int sequence = fixture.output(prompt.left(split));
            QVERIFY(sequence > 0);
            QTRY_VERIFY_WITH_TIMEOUT(fixture.delivered(sequence), 3000);
            QTest::qWait(30);
            QCOMPARE(fixture.errors.count(), 0);
            QCOMPARE(fixture.pins.count(), 0);
            QCOMPARE(fixture.auth.count(), 0);
            QVERIFY(fixture.session.active());
            QVERIFY(fixture.output(prompt.mid(split)) > 0);
        } else QVERIFY(fixture.output(prompt) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QTest::qWait(50); // Settle queued callbacks before negative assertions.
        singleCertificateDenial(fixture);
        QVERIFY(!QFileInfo::exists(config));
    }
    void publicCancelCertificateFragmentAndFreshAttempt_data()
    {
        QTest::addColumn<bool>("changed");
        QTest::newRow("normal") << false;
        QTest::newRow("changed") << true;
    }
    void publicCancelCertificateFragmentAndFreshAttempt()
    {
        QFETCH(bool, changed);
        SessionFixture fixture;
        QVERIFY(fixture.init({{"ignore_term", true}}));
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        const QByteArray certificate = fixtureCertificate(changed);
        const qsizetype split = certificate.indexOf("(Y/T/N)") + 5;
        const int prefix = fixture.output(certificate.left(split));
        QVERIFY(prefix > 0);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.delivered(prefix), 3000);
        QTest::qWait(30);
        QCOMPARE(fixture.errors.count(), 0);
        QCOMPARE(fixture.pins.count(), 0);
        QCOMPARE(fixture.auth.count(), 0);
        fixture.session.stop();
        QVERIFY(fixture.session.active());
        const int tail = fixture.output(certificate.mid(split) + "\nFIDO2 PIN: \n" + fixtureAuthorization());
        QVERIFY(tail > 0);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.delivered(tail), 1000);
        fixture.session.submitPin(QStringLiteral("cancelled-pin-marker"));
        fixture.submitAuthUrl(fixtureCallback());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3500);
        QCOMPARE(fixture.errors.count(), 0);
        QCOMPARE(fixture.pins.count(), 0);
        QCOMPARE(fixture.auth.count(), 0);
        QCOMPARE(fixture.input(), QByteArray{});
        QVERIFY(fixture.write(QStringLiteral("settings.json"), "{}"));
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 2, 3000);
        QVERIFY(fixture.output("FIDO2 PIN: ") > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.pins.count(), 1, 3000);
        fixture.session.submitPin(QStringLiteral("fresh-pin"));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.input(), QByteArray("fresh-pin\n"), 3000);
        fixture.session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 2, 3000);
        QCOMPARE(fixture.errors.count(), 0);
        QCOMPARE(fixture.events(QStringLiteral("version")).size(), 2);
        QCOMPARE(fixture.events(QStringLiteral("buildconfig")).size(), 2);
        noCertificateException(fixture);
    }
    void publicCertificateMixedRequests_data()
    {
        QTest::addColumn<QByteArray>("output");
        QTest::addColumn<QString>("message");
        QTest::addColumn<int>("pins");
        QTest::addColumn<int>("auth");
        const QString overlap = QStringLiteral("FreeRDP produced overlapping input requests; disconnected without sending secrets.");
        const auto cert = fixtureCertificate();
        QTest::newRow("certificate-pin") << (cert + "\nFIDO2 PIN: ") << certificateStoppedMessage() << 0 << 0;
        QTest::newRow("certificate-auth") << (cert + '\n' + fixtureAuthorization()) << certificateStoppedMessage() << 0 << 0;
        QTest::newRow("duplicate-certificates") << (cert + '\n' + fixtureCertificate(true)) << certificateStoppedMessage() << 0 << 0;
        QTest::newRow("pin-certificate") << (QByteArray("FIDO2 PIN: \n") + cert) << overlap << 1 << 0;
        QTest::newRow("auth-certificate") << (fixtureAuthorization() + '\n' + cert) << overlap << 0 << 1;
    }
    void publicCertificateMixedRequests()
    {
        QFETCH(QByteArray, output);
        QFETCH(QString, message);
        QFETCH(int, pins);
        QFETCH(int, auth);
        SessionFixture fixture;
        QVERIFY(fixture.init({{"ignore_term", true}}));
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        QVERIFY(fixture.output(output) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.errors.count(), 1, 3000);
        QCOMPARE(fixture.errors.first().first().toString(), message);
        QVERIFY(fixture.session.active());
        fixture.session.submitPin(QStringLiteral("late-pin-marker"));
        fixture.submitAuthUrl(fixtureCallback());
        QTest::qWait(100);
        QCOMPARE(fixture.input(), QByteArray{});
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3500);
        QCOMPARE(fixture.pins.count(), pins);
        QCOMPARE(fixture.auth.count(), auth);
        QCOMPARE(fixture.errors.count(), 1);
        QCOMPARE(fixture.input(), QByteArray{});
        QCOMPARE(fixture.events(QStringLiteral("version")).size(), 1);
        QCOMPARE(fixture.events(QStringLiteral("buildconfig")).size(), 1);
        QCOMPARE(fixture.events(QStringLiteral("transport")).size(), 1);
        noCertificateException(fixture);
    }
    void publicCertificateMalformedAndUnsupported_data()
    {
        QTest::addColumn<QByteArray>("output");
        QTest::addColumn<QString>("message");
        const QString invalid = QStringLiteral("FreeRDP produced an invalid or oversized authentication/certificate request; no input was sent.");
        QByteArray malformed = fixtureCertificate();
        malformed.replace(fixtureFingerprint(), "aa:bb");
        QTest::newRow("invalid-fingerprint") << malformed << invalid;
        QByteArray duplicate = fixtureCertificate();
        duplicate.replace("\tIssuer:      /CN=Fixture CA\n", "\tIssuer:      /CN=Fixture CA\n\tThumbprint:  " + fixtureFingerprint('c') + '\n');
        QTest::newRow("duplicate-field") << duplicate << invalid;
        QTest::newRow("unsupported-policy") << QByteArray("Synthetic certificate policy? ") << QStringLiteral("FreeRDP requested an unsupported interactive challenge or policy consent. No response was sent; complete any organization requirements with supported Microsoft tooling.");
    }
    void publicCertificateMalformedAndUnsupported()
    {
        QFETCH(QByteArray, output);
        QFETCH(QString, message);
        SessionFixture fixture;
        QVERIFY(fixture.init());
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        QVERIFY(fixture.output(output) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QCOMPARE(fixture.errors.count(), 1);
        QCOMPARE(fixture.errors.first().first().toString(), message);
        QCOMPARE(fixture.input(), QByteArray{});
        QCOMPARE(fixture.pins.count(), 0);
        QCOMPARE(fixture.auth.count(), 0);
        noCertificateException(fixture);
    }
    void publicStderrCertificateIsNotInteractive()
    {
        SessionFixture fixture;
        QVERIFY(fixture.init());
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        const int sequence = fixture.output(fixtureCertificate() + '\n' + fixtureCertificate(true), true);
        QVERIFY(sequence > 0);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.delivered(sequence), 3000);
        QTest::qWait(100);
        QVERIFY(fixture.session.active());
        QCOMPARE(fixture.errors.count(), 0);
        QCOMPARE(fixture.pins.count(), 0);
        QCOMPARE(fixture.auth.count(), 0);
        QCOMPARE(fixture.input(), QByteArray{});
        fixture.session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QCOMPARE(fixture.errors.count(), 0);
    }
    void publicCertificateDoesNotProbe_data()
    {
        QTest::addColumn<bool>("changed");
        QTest::newRow("normal") << false;
        QTest::newRow("changed") << true;
    }
    void publicCertificateDoesNotProbe()
    {
        QFETCH(bool, changed);
        QTcpServer listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        int connections = 0;
        connect(&listener, &QTcpServer::newConnection, &listener, [&] {
            while (auto* socket = listener.nextPendingConnection()) {
                ++connections;
                socket->abort();
                socket->deleteLater();
            }
        });
        // Positive controls before AND after denial establish observer sensitivity
        // and continued liveness. No TLS/root/store setup, external host or traffic.
        auto positiveControl = [&] {
            QTcpSocket socket;
            socket.connectToHost(QHostAddress::LocalHost, listener.serverPort());
            QTest::qWait(50);
        };
        positiveControl();
        QTRY_COMPARE_WITH_TIMEOUT(connections, 1, 3000);
        SessionFixture fixture;
        QVERIFY(fixture.init());
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        QVERIFY(fixture.output(fixtureCertificate(changed, "Server", "127.0.0.1", listener.serverPort())) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QTest::qWait(100); // Rejected attempt completed, not an immediate zero-count race.
        singleCertificateDenial(fixture);
        QCOMPARE(connections, 1);
        positiveControl();
        QTRY_COMPARE_WITH_TIMEOUT(connections, 2, 3000);
    }
    void publicCertificateNotificationsAreFenced_data()
    {
        QTest::addColumn<QString>("trigger");
        QTest::addColumn<bool>("ignoreTerm");
        for (const char* trigger : {"connection-error", "transport-stop", "status-error", "error"})
            QTest::newRow(trigger) << QString::fromLatin1(trigger) << false;
        QTest::newRow("error-term-ignoring-child") << QStringLiteral("error") << true;
    }
    void publicCertificateNotificationsAreFenced()
    {
        QFETCH(QString, trigger);
        QFETCH(bool, ignoreTerm);
        SessionFixture fixture;
        QVERIFY(fixture.init({{"ignore_term", ignoreTerm}}));
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        const QString config = configPath(fixture);
        bool exercised = false, remainedActive = false, configRetained = false, endedInside = false;
        const pid_t oldPid = fixture.events(QStringLiteral("transport")).first().value(QStringLiteral("pid")).toInt();
        auto nested = [&] {
            if (exercised) return;
            exercised = true;
            fixture.session.stop();
            fixture.session.stop();
            fixture.session.submitPin(QStringLiteral("late-pin-marker"));
            fixture.submitAuthUrl(fixtureCallback());
            // Observe attempt/config retention and no finalization during bounded
            // delivery, not executing-vs-zombie state or SIGKILL timing.
            QTest::qWait(ignoreTerm ? 2200 : 200);
            remainedActive = fixture.session.active();
            configRetained = QFileInfo::exists(config);
            endedInside = fixture.ended.count() != 0;
            fixture.session.start(fixture.profile()); // Inert while still fenced/active.
        };
        connect(&fixture.session, &Session::diagnosticEvent, &fixture.session, [&](const QString& event) {
            if (event == trigger) nested();
        });
        connect(&fixture.session, &Session::statusChanged, &fixture.session, [&](const QString& phase) {
            if (trigger == "status-error" && phase == "error") nested();
        });
        connect(&fixture.session, &Session::error, &fixture.session, [&](const QString& message) {
            if (trigger == "error" && message == certificateStoppedMessage()) nested();
        });
        bool configGoneAtEnd = false;
        connect(&fixture.session, &Session::ended, &fixture.session, [&] {
            if (fixture.ended.count() == 1) {
                configGoneAtEnd = !QFileInfo::exists(config);
                fixture.session.start(fixture.profile()); // ended must be the old attempt's final action.
            }
        });
        QVERIFY(fixture.output(fixtureCertificate() + '\n' + fixtureAuthorization() + "\nFIDO2 PIN: ") > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 2, ignoreTerm ? 10000 : 4000);
        QVERIFY(exercised);
        QVERIFY(remainedActive && configRetained && !endedInside);
        // After notification delivery, the old owned PID must be gone before
        // observing the replacement. This is cleanup, not a SIGKILL clock oracle.
        QVERIFY(::kill(oldPid, 0) < 0 && errno == ESRCH);
        QVERIFY(configGoneAtEnd);
        QCOMPARE(fixture.ended.count(), 1);
        QCOMPARE(fixture.errors.count(), 1); // Fenced start is inert; only the fixed denial is delivered.
        int denials = 0;
        for (const auto& error : fixture.errors) if (error.first().toString() == certificateStoppedMessage()) ++denials;
        QCOMPARE(denials, 1);
        QCOMPARE(fixture.pins.count(), 0);
        QCOMPARE(fixture.auth.count(), 0);
        QCOMPARE(fixture.input(), QByteArray{});
        QVERIFY(fixture.session.active());
        QTest::qWait(100);
        QCOMPARE(fixture.ended.count(), 1);
        QVERIFY(fixture.output("FIDO2 PIN: ") > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.pins.count(), 1, 3000);
        fixture.session.submitPin(QStringLiteral("fresh-pin"));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.input(), QByteArray("fresh-pin\n"), 3000);
        fixture.session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 2, 3000);
        QCOMPARE(fixture.events(QStringLiteral("version")).size(), 2);
        QCOMPARE(fixture.events(QStringLiteral("buildconfig")).size(), 2);
        noCertificateException(fixture);
    }
    void publicCertificateConfigAndStaleCallbacks()
    {
        SessionFixture fixture;
        QVERIFY(fixture.init({{"ignore_term", true}}));
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        QVERIFY(fixture.output(fixtureAuthorization()) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.auth.count(), 1, 3000);
        const auto old = fixture.auth.first().first().value<OAuthContract::Request>();
        // This line was delivered before denial; do not mistake it for a trust reply.
        const OAuthContract::Callback oldResult{fixtureCallback(), old.generation};
        fixture.session.submitAuthResult(oldResult);
        const QByteArray before = oldResult.url.toEncoded() + '\n';
        QTRY_COMPARE_WITH_TIMEOUT(fixture.input(), before, 3000);
        const QString config = configPath(fixture);
        bool activeAtError = false, configAtError = false, configAtEnd = true;
        connect(&fixture.session, &Session::error, &fixture.session, [&](const QString& message) {
            if (message == certificateStoppedMessage()) {
                activeAtError = fixture.session.active();
                configAtError = QFileInfo::exists(config);
            }
        });
        connect(&fixture.session, &Session::ended, &fixture.session, [&] { configAtEnd = QFileInfo::exists(config); });
        QVERIFY(fixture.output('\n' + fixtureCertificate(true)) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.errors.count(), 1, 3000);
        QVERIFY(activeAtError && configAtError);
        fixture.session.submitAuthResult(oldResult);
        fixture.session.submitPin(QStringLiteral("late-pin-marker"));
        fixture.session.stop();
        fixture.session.stop();
        // Child ignores TERM: observe the still-owned config and empty new input.
        QTest::qWait(100);
        QVERIFY(fixture.session.active());
        QVERIFY(QFileInfo::exists(config));
        QCOMPARE(fixture.input(), before);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3500);
        QVERIFY(!configAtEnd && !QFileInfo::exists(config));
        QCOMPARE(fixture.errors.first().first().toString(), certificateStoppedMessage());
        QVERIFY(fixture.write(QStringLiteral("settings.json"), "{}"));
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 2, 3000);
        QVERIFY(fixture.output(fixtureAuthorization()) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.auth.count(), 2, 3000);
        const auto fresh = fixture.auth.last().first().value<OAuthContract::Request>();
        QVERIFY(fresh.generation > old.generation);
        fixture.session.submitAuthResult(oldResult);
        QTest::qWait(100);
        QCOMPARE(fixture.input(), before);
        fixture.session.submitAuthResult({fixtureCallback(), fresh.generation});
        QTRY_COMPARE_WITH_TIMEOUT(fixture.input(), before + before, 3000);
        fixture.session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 2, 3000);
        QCOMPARE(fixture.errors.count(), 1);
        QCOMPARE(fixture.events(QStringLiteral("version")).size(), 2);
        QCOMPARE(fixture.events(QStringLiteral("buildconfig")).size(), 2);
        noCertificateException(fixture);
    }
    void publicCertificateWipesQueuedInput_data()
    {
        QTest::addColumn<bool>("authorization");
        QTest::addColumn<bool>("reject");
        QTest::newRow("pin-denied") << false << true;
        QTest::newRow("auth-denied") << true << true;
        QTest::newRow("pin-pressure-sensitivity") << false << false;
        QTest::newRow("auth-pressure-sensitivity") << true << false;
    }
    void publicCertificateWipesQueuedInput()
    {
        QFETCH(bool, authorization);
        QFETCH(bool, reject);
        SessionFixture fixture;
        QVERIFY(fixture.init({{"ignore_term", true}, {"pause_input", true}}));
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        QVERIFY(fixture.output(authorization ? fixtureAuthorization() : QByteArray("FIDO2 PIN: ")) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(authorization ? fixture.auth.count() : fixture.pins.count(), 1, 3000);
        // Use only the observed, owned nonblocking PTY descriptor. Fill the
        // kernel input queue with nonsecret padding while the fixture pauses reads,
        // ensuring the application must retain pending PIN/auth bytes.
        int master = -1;
        for (auto* notifier : fixture.session.findChildren<QSocketNotifier*>())
            if (notifier->type() == QSocketNotifier::Read && ::isatty(notifier->socket()))
                master = notifier->socket();
        QVERIFY2(master >= 0, "Backpressure oracle requires a QObject-owned PTY read notifier.");
        QVERIFY((::fcntl(master, F_GETFL) & O_NONBLOCK) != 0);
        QByteArray pressure;
        const QByteArray block(8192, 'q');
        bool blocked = false;
        for (int i = 0; i < 64; ++i) {
            const ssize_t count = ::write(master, block.constData(), block.size());
            if (count > 0) pressure += block.left(count);
            else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // Let the PTY line discipline settle, then confirm saturation.
                QTest::qWait(20);
                const ssize_t again = ::write(master, block.constData(), block.size());
                if (again > 0) pressure += block.left(again);
                else if (again < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { blocked = true; break; }
                else QFAIL("Unexpected owned PTY saturation result");
            } else QFAIL("Unexpected owned PTY write result");
        }
        QVERIFY(blocked && !pressure.isEmpty());
        QByteArray reply;
        if (authorization) {
            const auto request = fixture.auth.last().first().value<OAuthContract::Request>();
            const auto callback = fixtureCallback(QStringLiteral("code=") + QString(3957, 'x') + "&state=" + request.state);
            reply = callback.toEncoded() + '\n';
            QCOMPARE(reply.size(), 4095);
            fixture.session.submitAuthResult({callback, request.generation});
        } else {
            reply = "queued-pin-marker\n";
            fixture.session.submitPin(QStringLiteral("queued-pin-marker"));
        }
        QTest::qWait(30);
        QCOMPARE(fixture.input(), QByteArray{}); // No child reads, hence no delivered bytes yet.
        if (reject) {
            QVERIFY(fixture.output('\n' + fixtureCertificate()) > 0);
            QTRY_COMPARE_WITH_TIMEOUT(fixture.errors.count(), 1, 3000);
            QCOMPARE(fixture.errors.first().first().toString(), certificateStoppedMessage());
            QVERIFY(fixture.session.active());
        }
        QVERIFY(fixture.resumeInput());
        QTRY_VERIFY_WITH_TIMEOUT(fixture.input().size() >= pressure.size(), 1000);
        QTest::qWait(100);
        QCOMPARE(fixture.input().left(pressure.size()), pressure);
        if (reject) {
            // Already kernel-delivered prefixes are not revocable. This observes
            // non-delivery of the retained suffix, not the memory wipe itself;
            // private first-notification snapshots cover that. Padding is not a reply.
            const QByteArray delivered = fixture.input().mid(pressure.size());
            QVERIFY(reply.startsWith(delivered));
            QVERIFY(delivered.size() < reply.size());
            QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3500);
            QCOMPARE(fixture.input().mid(pressure.size()), delivered);
        } else {
            // Counterfactual: identical pressure, no denial. The observer must
            // see the complete queued reply once reads resume.
            QTRY_COMPARE_WITH_TIMEOUT(fixture.input(), pressure + reply, 3000);
            fixture.session.stop();
            QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3500);
        }
        QCOMPARE(fixture.errors.count(), reject ? 1 : 0);
        noCertificateException(fixture);
    }
    void publicCertificateFinalDrain_data()
    {
        QTest::addColumn<bool>("changed");
        QTest::newRow("normal") << false;
        QTest::newRow("changed") << true;
    }
    void publicCertificateFinalDrain()
    {
        QFETCH(bool, changed);
        SessionFixture fixture;
        QVERIFY(fixture.init());
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        const QString config = configPath(fixture);
        // Disable only the owned PTY's read activation, so the timer's final
        // drain, not a scheduler race, consumes the certificate after waitpid.
        int readers = 0;
        for (auto* notifier : fixture.session.findChildren<QSocketNotifier*>())
            if (notifier->type() == QSocketNotifier::Read && ::isatty(notifier->socket())) {
                notifier->setEnabled(false);
                ++readers;
            }
        QVERIFY2(readers > 0, "Final-drain oracle requires a QObject-owned PTY read notifier.");
        bool exitObserved = false, activeDuringExit = false, configDuringExit = false, endedDuringExit = false;
        connect(&fixture.session, &Session::diagnosticEvent, &fixture.session, [&](const QString& event) {
            if (event == "transport-exit" && !exitObserved) {
                exitObserved = true;
                fixture.session.stop();
                QTest::qWait(200); // Nested finalizer entry must be inert even after waitpid consumed the child.
                activeDuringExit = fixture.session.active();
                configDuringExit = QFileInfo::exists(config);
                endedDuringExit = fixture.ended.count() != 0;
            }
        });
        bool goneBeforeEnd = false;
        connect(&fixture.session, &Session::ended, &fixture.session, [&] {
            if (fixture.ended.count() == 1) {
                goneBeforeEnd = !QFileInfo::exists(config);
                fixture.session.start(fixture.profile());
            }
        });
        QVERIFY(fixture.outputAndExit(fixtureCertificate(changed) + "\nFIDO2 PIN: " + fixtureAuthorization()) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 2, 4000);
        QVERIFY(exitObserved && activeDuringExit && configDuringExit && !endedDuringExit);
        QVERIFY(goneBeforeEnd);
        QCOMPARE(fixture.errors.count(), 1);
        QCOMPARE(fixture.errors.first().first().toString(), certificateStoppedMessage());
        QCOMPARE(fixture.ended.count(), 1);
        QCOMPARE(fixture.pins.count(), 0);
        QCOMPARE(fixture.auth.count(), 0);
        QCOMPARE(fixture.input(), QByteArray{});
        QVERIFY(fixture.session.active());
        QVERIFY(fixture.output("FIDO2 PIN: ") > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.pins.count(), 1, 3000);
        fixture.session.submitPin(QStringLiteral("fresh-pin"));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.input(), QByteArray("fresh-pin\n"), 3000);
        fixture.session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 2, 3000);
        noCertificateException(fixture);
    }
    void publicStaleNotifierCannotReadReplacement()
    {
        SessionFixture fixture;
        QVERIFY(fixture.init());
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        QPointer<QSocketNotifier> stale;
        for (auto* notifier : fixture.session.findChildren<QSocketNotifier*>())
            if (notifier->type() == QSocketNotifier::Read && ::isatty(notifier->socket())) stale = notifier;
        QVERIFY(stale);
        const QSocketDescriptor oldSocket(stale->socket());
        connect(&fixture.session, &Session::ended, &fixture.session, [&] {
            if (fixture.ended.count() == 1) {
                // Retain only this disabled, owned notifier to model a delayed
                // old activation. Its closed descriptor is never read by the test.
                QCoreApplication::removePostedEvents(stale, QEvent::DeferredDelete);
                fixture.session.start(fixture.profile());
            }
        });
        fixture.session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 2, 4000);
        QVERIFY(stale);
        QSocketNotifier* reader = nullptr;
        for (auto* notifier : fixture.session.findChildren<QSocketNotifier*>())
            if (notifier != stale && notifier->type() == QSocketNotifier::Read && ::isatty(notifier->socket())) reader = notifier;
        QVERIFY(reader);
        reader->setEnabled(false);
        const int sequence = fixture.output(fixtureCertificate());
        QVERIFY(sequence > 0);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.delivered(sequence), 3000);
        QVERIFY(QMetaObject::invokeMethod(stale, "activated", Qt::DirectConnection,
            Q_ARG(QSocketDescriptor, oldSocket), Q_ARG(QSocketNotifier::Type, QSocketNotifier::Read)));
        QTest::qWait(100);
        QCOMPARE(fixture.errors.count(), 0);
        QCOMPARE(fixture.ended.count(), 1);
        QCOMPARE(fixture.pins.count(), 0);
        QCOMPARE(fixture.auth.count(), 0);
        QVERIFY(fixture.session.active());
        // Positive sensitivity control: the current notifier's identical signal
        // dispatch must consume that unread certificate and deny this attempt.
        QVERIFY(QMetaObject::invokeMethod(reader, "activated", Qt::DirectConnection,
            Q_ARG(QSocketDescriptor, QSocketDescriptor(reader->socket())),
            Q_ARG(QSocketNotifier::Type, QSocketNotifier::Read)));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 2, 3000);
        QCOMPARE(fixture.errors.count(), 1);
        QCOMPARE(fixture.errors.first().first().toString(), certificateStoppedMessage());
        QCOMPARE(fixture.input(), QByteArray{});
        QCOMPARE(fixture.events(QStringLiteral("version")).size(), 2);
        QCOMPARE(fixture.events(QStringLiteral("buildconfig")).size(), 2);
        noCertificateException(fixture);
    }
    void publicOldPromptActivationCannotSignalReplacement()
    {
        SessionFixture fixture;
        QVERIFY(fixture.init());
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        bool nested = false, replacementStartedInside = false;
        const pid_t originalPid = fixture.events(QStringLiteral("transport")).first().value(QStringLiteral("pid")).toInt();
        connect(&fixture.session, &Session::ended, &fixture.session, [&] {
            if (fixture.ended.count() == 1) fixture.session.start(fixture.profile());
        });
        connect(&fixture.session, &Session::statusChanged, &fixture.session, [&](const QString& phase) {
            if (phase == "awaiting-pin" && !nested) {
                nested = true;
                fixture.session.stop();
                // Yield until an ended receiver has actually launched a replacement;
                // the suspended PIN dispatch must not emit its old pinRequested.
                QElapsedTimer deadline;
                deadline.start();
                while (fixture.events(QStringLiteral("terminal")).size() < 2 && deadline.elapsed() < 3000)
                    QTest::qWait(10);
                const auto launched = fixture.events(QStringLiteral("transport"));
                // Capture while this old status receiver still suspends dispatch,
                // not after the outer wait eventually observes another terminal.
                replacementStartedInside = fixture.events(QStringLiteral("terminal")).size() == 2 &&
                    launched.size() == 2 && launched.last().value(QStringLiteral("pid")).toInt() != originalPid &&
                    fixture.ended.count() == 1 && fixture.session.active();
            }
        });
        QVERIFY(fixture.output(QByteArray("FIDO2 PIN: \n") + fixtureAuthorization()) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 2, 4000);
        QVERIFY(nested);
        QVERIFY2(replacementStartedInside, "Replacement must be running before the old status receiver returns.");
        QTest::qWait(100);
        QCOMPARE(fixture.pins.count(), 0);
        QCOMPARE(fixture.auth.count(), 0);
        QCOMPARE(fixture.errors.count(), 0);
        QCOMPARE(fixture.ended.count(), 1);
        QVERIFY(fixture.session.active());
        QCOMPARE(fixture.input(), QByteArray{});
        QVERIFY(fixture.output("FIDO2 PIN: ") > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.pins.count(), 1, 3000);
        fixture.session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 2, 3000);
    }
    void publicPromptFailures_data()
    {
        QTest::addColumn<QByteArray>("output");
        QTest::addColumn<QString>("message");
        const QString invalid = QStringLiteral("FreeRDP produced an invalid or oversized authentication/certificate request; no input was sent.");
        const QString unsupported = QStringLiteral("FreeRDP requested an unsupported interactive challenge or policy consent. No response was sent; complete any organization requirements with supported Microsoft tooling.");
        const QString overlap = QStringLiteral("FreeRDP produced overlapping input requests; disconnected without sending secrets.");
        QTest::newRow("unsupported-password") << QByteArray("Password: ") << unsupported;
        QTest::newRow("unknown-prompt") << QByteArray("Synthetic policy? ") << unsupported;
        QTest::newRow("callback-without-request") << QByteArray("Paste redirect URL here: ") << invalid;
        QTest::newRow("wrong-auth-origin") << QByteArray("Browse to: https://fixture.invalid/oauth2/v2.0/authorize\n") << invalid;
        QTest::newRow("oversized-output") << QByteArray(32769, 'x') << invalid;
        QTest::newRow("control-output") << QByteArray("noise\x01", 6) << invalid;
        QTest::newRow("overlapping-pin") << QByteArray("FIDO2 PIN: \nFIDO2 PIN: ") << overlap;
        QTest::newRow("overlapping-pin-auth") << (QByteArray("FIDO2 PIN: \n") + fixtureAuthorization()) << overlap;
        QTest::newRow("overlapping-auth-pin") << (fixtureAuthorization() + "\nFIDO2 PIN: ") << overlap;
    }
    void publicPromptFailures()
    {
        QFETCH(QByteArray, output);
        QFETCH(QString, message);
        SessionFixture fixture;
        QVERIFY(fixture.init());
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        QVERIFY(fixture.output(output) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QCOMPARE(fixture.errors.count(), 1);
        QCOMPARE(fixture.errors.at(0).at(0).toString(), message);
        QCOMPARE(fixture.input(), QByteArray{});
        fixture.session.submitPin(QStringLiteral("late-pin"));
        fixture.submitAuthUrl(fixtureCallback());
        QTest::qWait(100);
        QCOMPARE(fixture.input(), QByteArray{});
        QCOMPARE(fixture.ended.count(), 1);
    }
    void publicCancelChallengeAndReconnect_data()
    {
        QTest::addColumn<bool>("authorization");
        QTest::newRow("pin") << false;
        QTest::newRow("authorization") << true;
    }
    void publicCancelChallengeAndReconnect()
    {
        QFETCH(bool, authorization);
        SessionFixture fixture;
        QVERIFY(fixture.init({{"ignore_term", true}}));
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        QVERIFY(fixture.output(authorization ? fixtureAuthorization() : QByteArray("FIDO2 PIN: ")) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(authorization ? fixture.auth.count() : fixture.pins.count(), 1, 3000);
        fixture.session.stop();
        QVERIFY(fixture.session.active()); // Still reaping; a new attempt must wait.
        fixture.session.submitPin(QStringLiteral("cancelled-pin"));
        fixture.submitAuthUrl(fixtureCallback());
        fixture.session.start(fixture.profile());
        QCOMPARE(fixture.errors.count(), 1);
        QTest::qWait(150);
        QCOMPARE(fixture.input(), QByteArray{}); // Child ignores TERM and still observes input.
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3500);
        QVERIFY(!fixture.session.active());
        QCOMPARE(fixture.events(QStringLiteral("transport")).size(), 1);
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 2, 3000);
        fixture.session.submitPin(QStringLiteral("old-pin"));
        fixture.submitAuthUrl(fixtureCallback());
        QTest::qWait(100);
        QCOMPARE(fixture.input(), QByteArray{}); // Fresh transport has not requested anything.
        QVERIFY(fixture.output("FIDO2 PIN: ") > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.pins.count(), authorization ? 1 : 2, 3000);
        fixture.session.submitPin(QStringLiteral("fresh-pin"));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.input(), QByteArray("fresh-pin\n"), 3000);
        fixture.session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 2, 3500);
        QCOMPARE(fixture.errors.count(), 1); // Only deliberate start-while-active rejection.
        QVERIFY(!fixture.session.active());
    }
    void publicFailureAndTerminalDiagnostic_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<QString>("message");
        QTest::newRow("version-crash") << QStringLiteral("version-crash") << QStringLiteral("Could not inspect the installed FreeRDP executable.");
        QTest::newRow("version-timeout") << QStringLiteral("version-timeout") << QStringLiteral("Could not verify the installed FreeRDP version within five seconds.");
        QTest::newRow("missing-profile") << QStringLiteral("missing-profile") << QStringLiteral("The imported connection file is unavailable.");
        QTest::newRow("transport-exec-failure") << QStringLiteral("transport-exec-failure") << QStringLiteral("FreeRDP could not start its controlled terminal or executable.");
        QTest::newRow("clean-exit-before-desktop") << QStringLiteral("clean-exit-before-desktop") << QStringLiteral("FreeRDP ended before a connected desktop was observed.");
        QTest::newRow("diagnostic-exit") << QStringLiteral("diagnostic-exit") << QStringLiteral("FreeRDP reported authentication failure.");
    }
    void publicFailureAndTerminalDiagnostic()
    {
        QFETCH(QString, scenario);
        QFETCH(QString, message);
        SessionFixture fixture;
        QJsonObject settings;
        if (scenario == QStringLiteral("version-crash"))
            settings.insert(QStringLiteral("version_crash"), true);
        if (scenario == QStringLiteral("version-timeout"))
            settings.insert(QStringLiteral("version_delay"), 8);
        if (scenario == QStringLiteral("transport-exec-failure"))
            settings.insert(QStringLiteral("break_transport_exec"), true);
        QVERIFY(fixture.init(settings));
        fixture.session.start(scenario == QStringLiteral("missing-profile") ? fixture.path(QStringLiteral("absent.rdp")) : fixture.profile());
        if (scenario.endsWith(QStringLiteral("before-desktop")) || scenario == QStringLiteral("diagnostic-exit")) {
            QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
            if (scenario == QStringLiteral("diagnostic-exit")) {
                const int sequence = fixture.output("fixture-secret https://fixture.invalid/?code=fixture-token ERRCONNECT_AUTHENTICATION_FAILED\n", true);
                QVERIFY(sequence > 0);
                QTRY_VERIFY_WITH_TIMEOUT(fixture.delivered(sequence), 3000);
            }
            QVERIFY(fixture.exit(scenario == QStringLiteral("diagnostic-exit") ? 1 : 0));
        }
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 7000);
        QCOMPARE(fixture.errors.count(), 1);
        QCOMPARE(fixture.errors.at(0).at(0).toString(), message);
        QCOMPARE(fixture.input(), QByteArray{});
        QVERIFY(!fixture.session.active());
        fixture.session.stop();
        QTest::qWait(100);
        QCOMPARE(fixture.ended.count(), 1);
        QCOMPARE(fixture.errors.count(), 1);
    }
    void publicStderrCannotRequestSecretsAndDiagnosticsAreFixed()
    {
        // Release builds must ignore the developer-only raw stderr log switch (INV-3).
        QTemporaryDir logDir(QDir::tempPath() + QStringLiteral("/omawin365-stderr-log-XXXXXX"));
        QVERIFY(logDir.isValid());
        const QString rawLog = logDir.filePath(QStringLiteral("freerdp-stderr.log"));
        qputenv("OMAWIN365_FREERDP_STDERR_LOG", QFile::encodeName(rawLog));
        const auto restoreLog = qScopeGuard([] { qunsetenv("OMAWIN365_FREERDP_STDERR_LOG"); });
        SessionFixture fixture;
        QVERIFY(fixture.init());
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        const int sequence = fixture.output("FIDO2 PIN: \n" + fixtureAuthorization() +
            "\nfixture-secret ERRCONNECT_AUTHENTICATION_FAILED https://fixture.invalid/?code=fixture-token\n", true);
        QVERIFY(sequence > 0);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.delivered(sequence), 3000);
        QTest::qWait(150);
        QCOMPARE(fixture.pins.count(), 0);
        QCOMPARE(fixture.auth.count(), 0);
        fixture.session.submitPin(QStringLiteral("fixture-secret"));
        fixture.submitAuthUrl(fixtureCallback());
        QCOMPARE(fixture.input(), QByteArray{});
        const int stdoutSequence = fixture.output("fixture-secret ERRCONNECT_CONNECT_TRANSPORT_FAILED https://fixture.invalid/?code=fixture-token\n");
        QVERIFY(stdoutSequence > 0);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.delivered(stdoutSequence), 3000);
        QTest::qWait(150);
        QCOMPARE(fixture.input(), QByteArray{}); // Settle after submissions before checking absence.
        QVERIFY(fixture.exit(1));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QCOMPARE(fixture.errors.count(), 1);
        QCOMPARE(fixture.errors.at(0).at(0).toString(), QStringLiteral("FreeRDP reported a transport connection failure."));
        QVERIFY(!QFileInfo::exists(rawLog));
        const QStringList expected{QStringLiteral("connection-start"), QStringLiteral("transport-start"), QStringLiteral("transport-exit")};
        QStringList actual;
        for (const auto& signal : fixture.diagnostics)
            actual.append(signal.at(0).toString());
        QCOMPARE(actual, expected);
        for (const auto* observations : {&fixture.statuses, &fixture.errors, &fixture.diagnostics}) {
            for (const auto& signal : *observations) {
                for (const auto& value : signal) {
                    QVERIFY(!value.toString().contains(QStringLiteral("fixture-secret")));
                    QVERIFY(!value.toString().contains(QStringLiteral("fixture-token")));
                    QVERIFY(!value.toString().contains(QStringLiteral("fixture.invalid")));
                }
            }
        }
    }
    void publicSavedProfilePolicy_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("former-redirection-launch-fixture") << QByteArray("full address:s:fixture.invalid\nredirectdrives:i:1\nredirectsmartcards:i:1\n");
        QTest::newRow("redirection-on-complete-profile") << (supportedProfile + "redirectdrives:i:1\nredirectsmartcards:i:1\n");
        QTest::newRow("saved-secret") << (supportedProfile + "password:s:fixture-secret-marker\n");
        QTest::newRow("saved-opaque") << (supportedProfile + "pcb:s:fixture-secret-marker\n");
        QTest::newRow("saved-unknown") << (supportedProfile + "fixture-secret-marker:s:fixture-secret-marker\n");
        QTest::newRow("saved-duplicate") << (supportedProfile + "FULL ADDRESS:s:fixture-secret-marker\n");
        QTest::newRow("saved-malformed") << (supportedProfile + "/cert:ignore\n");
    }
    void publicSavedProfilePolicy()
    {
        QFETCH(QByteArray, bytes);
        SessionFixture fixture;
        QVERIFY(fixture.init());
        fixture.environment("XDG_DATA_HOME", QFile::encodeName(fixture.path(QStringLiteral("data"))));
        const QString root = fixture.path(QStringLiteral("data/omawin365"));
        QVERIFY(QDir().mkpath(root));
        QCOMPARE(::chmod(QFile::encodeName(root).constData(), 0700), 0);
        const QString id = QStringLiteral("12345678-1234-1234-1234-123456789abc");
        const QString saved = root + '/' + id + QStringLiteral(".rdpw");
        const QString index = root + QStringLiteral("/profiles.json");
        const QByteArray metadata = QJsonDocument(QJsonArray{QJsonObject{{"id", id}, {"name", "Old PC"}}}).toJson();
        auto writePrivate = [](const QString& path, const QByteArray& content) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(content) == content.size()
                && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        };
        auto read = [](const QString& path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
        };
        QVERIFY(writePrivate(saved, bytes));
        QVERIFY(writePrivate(index, metadata));
        ProfileStore reopened;
        QCOMPARE(reopened.profiles().size(), 1); // Existing files are not rewritten/deleted on load.
        QSignalSpy changes(&reopened, &ProfileStore::changed);
        fixture.session.start(reopened.profiles().first().path);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QVERIFY(!fixture.session.active());
        QCOMPARE(fixture.errors.count(), 1);
        QCOMPARE(fixture.lifecycle, (QStringList{"status:connecting", "status:error", "error", "ended"}));
        QCOMPARE(fixture.events(QStringLiteral("transport")).size(), 0);
        QCOMPARE(fixture.events(QStringLiteral("terminal")).size(), 0);
        QCOMPARE(fixture.pins.count(), 0);
        QCOMPARE(fixture.auth.count(), 0);
        QCOMPARE(changes.count(), 0);
        QCOMPARE(read(saved), bytes);
        QCOMPARE(read(index), metadata);
        for (const auto* spy : {&fixture.statuses, &fixture.errors, &fixture.diagnostics}) {
            for (const auto& signal : *spy) {
                for (const auto& value : signal)
                    QVERIFY(!value.toString().contains(QStringLiteral("fixture-secret-marker")));
            }
        }
        // Explicit synthetic migration permits a fresh start; the app never does this rewrite.
        QVERIFY(writePrivate(saved, supportedProfile));
        fixture.session.start(saved);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        fixture.session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 2, 3000);
        QCOMPARE(fixture.errors.count(), 1);
    }
    void publicProfileRecheckedAfterVersion()
    {
        SessionFixture fixture;
        QVERIFY(fixture.init({{"version_delay", 0.4}}));
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("version")).size(), 1, 3000);
        const QByteArray rejected = supportedProfile + "password:s:fixture-secret-marker\n";
        QVERIFY(fixture.write(QStringLiteral("synthetic.rdp"), rejected));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QCOMPARE(fixture.errors.count(), 1);
        QCOMPARE(fixture.events(QStringLiteral("transport")).size(), 0);
        QVERIFY(!fixture.session.active());
        QFile file(fixture.profile());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), rejected);
        QVERIFY(!fixture.errors.first().first().toString().contains(QStringLiteral("fixture-secret-marker")));
    }
    void publicRestrictedLaunchContract_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<bool>("saved");
        QTest::newRow("minimal") << supportedProfile << false;
        QTest::newRow("fuller-saved-utf8") << fullerProfile << true;
        QStringEncoder encoder(QStringConverter::Utf16LE, QStringConverter::Flag::WriteBom);
        const QByteArray utf16 = encoder.encode(QString::fromUtf8(fullerProfile));
        QTest::newRow("fuller-saved-utf16le") << utf16 << true;
    }
    void publicRestrictedLaunchContract()
    {
        QFETCH(QByteArray, bytes);
        QFETCH(bool, saved);
        SessionFixture fixture;
        QVERIFY(fixture.init());
        QVERIFY(fixture.write(QStringLiteral("synthetic.rdp"), bytes));
        QString launchPath = fixture.profile();
        QByteArray indexBefore;
        QString indexPath;
        auto read = [](const QString& path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
        };
        if (saved) {
            fixture.environment("XDG_DATA_HOME", QFile::encodeName(fixture.path(QStringLiteral("data"))));
            ProfileStore store;
            QString error;
            const auto profile = store.importFile(fixture.profile(), &error);
            QVERIFY2(!profile.id.isEmpty(), qPrintable(error));
            ProfileStore reopened;
            QCOMPARE(reopened.profiles().size(), 1);
            QCOMPARE(reopened.profiles().first().path, profile.path);
            launchPath = reopened.profiles().first().path;
            QCOMPARE(read(launchPath), bytes);
            indexPath = QFileInfo(launchPath).absolutePath() + QStringLiteral("/profiles.json");
            indexBefore = read(indexPath);
            QVERIFY(!indexBefore.isEmpty());
        }
        fixture.environment("WLOG_LEVEL", "TRACE");
        fixture.environment("WLOG_APPENDER", "FILE");
        fixture.environment("WLOG_FIXTURE_SECRET", "fixture-secret");
        fixture.environment("FREERDP_ASKPASS", "/fixture/askpass");
        fixture.environment("SSLKEYLOGFILE", "/fixture/tls-secret");
        fixture.environment("SESSION_FIXTURE_KEEP", "keep-unrelated");
        fixture.session.start(launchPath);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        for (const auto& kind : {QStringLiteral("version"), QStringLiteral("transport")}) {
            const auto records = fixture.events(kind);
            QCOMPARE(records.size(), 1);
            const auto environment = records.front().value(QStringLiteral("environment")).toObject();
            QCOMPARE(environment.value(QStringLiteral("WLOG_LEVEL")).toString(), QStringLiteral("WARN"));
            QCOMPARE(environment.value(QStringLiteral("WLOG_APPENDER")).toString(), QStringLiteral("CONSOLE"));
            QVERIFY(environment.value(QStringLiteral("WLOG_FIXTURE_SECRET")).isNull());
            QVERIFY(environment.value(QStringLiteral("FREERDP_ASKPASS")).isNull());
            QVERIFY(environment.value(QStringLiteral("SSLKEYLOGFILE")).isNull());
            QCOMPARE(environment.value(QStringLiteral("SESSION_FIXTURE_KEEP")).toString(), QStringLiteral("keep-unrelated"));
        }
        QCOMPARE(fixture.events(QStringLiteral("version")).front().value(QStringLiteral("argv")).toArray(),
            QJsonArray{QStringLiteral("/version")});
        const auto args = fixture.events(QStringLiteral("transport")).front().value(QStringLiteral("argv")).toArray();
        QCOMPARE(args.size(), 12);
        QCOMPARE(args.at(0).toString(), QFileInfo(launchPath).canonicalFilePath());
        const QStringList expected{QStringLiteral("+force-console-callbacks"), QStringLiteral("/gateway:type:arm"), QStringLiteral("/sec:aad"),
            QStringLiteral("/clipboard:files-to:off"), QStringLiteral("-auto-reconnect"),
            QStringLiteral("/t:OMAWIN365 - Windows 365")};
        for (int index = 0; index < expected.size(); ++index)
            QCOMPARE(args.at(index + 1).toString(), expected.at(index));
        QVERIFY(QRegularExpression(QStringLiteral("^/wm-class:omawin365-[0-9a-f-]{36}$")).match(args.at(7).toString()).hasMatch());
        QCOMPARE(args.at(8).toString(), QStringLiteral("/size:80%"));
        QCOMPARE(args.at(9).toString(), QStringLiteral("/log-level:WARN"));
        QCOMPARE(args.at(10).toString(), QStringLiteral("/log-filters:com.freerdp.client.x11:ERROR"));
        const QString tune = args.at(11).toString();
        const QString policy = QStringLiteral(
            "/tune:FreeRDP_RedirectWebAuthN:TRUE,FreeRDP_AuthenticationLevel:2,"
            "FreeRDP_IgnoreCertificate:FALSE,FreeRDP_AutoAcceptCertificate:FALSE,"
            "FreeRDP_AutoDenyCertificate:FALSE,FreeRDP_ExternalCertificateManagement:FALSE,"
            "FreeRDP_CertificateCallbackPreferPEM:FALSE,"
            "FreeRDP_RedirectDrives:FALSE,FreeRDP_RedirectHomeDrive:FALSE,"
            "FreeRDP_DrivesToRedirect:,FreeRDP_RedirectPrinters:FALSE,"
            "FreeRDP_RedirectSmartCards:FALSE,FreeRDP_RedirectSerialPorts:FALSE,"
            "FreeRDP_RedirectParallelPorts:FALSE,FreeRDP_AudioCapture:FALSE,"
            "FreeRDP_AudioPlayback:FALSE,FreeRDP_SupportSSHAgentChannel:FALSE,"
            "FreeRDP_DeviceRedirection:FALSE,FreeRDP_DeviceCount:0,"
            "FreeRDP_StaticChannelCount:0,FreeRDP_DynamicChannelCount:0,"
            "FreeRDP_AutoReconnectionEnabled:FALSE,FreeRDP_RemoteApplicationMode:FALSE,"
            "FreeRDP_DynamicResolutionUpdate:TRUE,FreeRDP_SupportDisplayControl:TRUE,"
            "FreeRDP_SmartSizing:FALSE,FreeRDP_ParentWindowId:0,FreeRDP_EmbeddedWindow:FALSE,"
            "FreeRDP_Decorations:TRUE,FreeRDP_ToggleFullscreen:TRUE,"
            "FreeRDP_ActionScript:,FreeRDP_ConfigPath:");
        QVERIFY(tune.startsWith(policy));
        const QString config = tune.mid(policy.size());
        QVERIFY(QDir::isAbsolutePath(config));
        QVERIFY(!config.contains(','));
        QVERIFY(QFileInfo(config).isDir());
        struct stat permissions{};
        QCOMPARE(::stat(QFile::encodeName(config).constData(), &permissions), 0);
        QCOMPARE(permissions.st_mode & 0777, mode_t(0700));
        const auto terminal = fixture.events(QStringLiteral("terminal")).front();
        QVERIFY(terminal.value(QStringLiteral("private")).toBool());
        QVERIFY(!terminal.value(QStringLiteral("echo")).toBool());
        QVERIFY(!terminal.value(QStringLiteral("canonical")).toBool());
        fixture.session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QVERIFY(!QFileInfo::exists(config));
        QCOMPARE(fixture.errors.count(), 0);
        QCOMPARE(read(fixture.profile()), bytes);
        QCOMPARE(read(launchPath), bytes);
        if (saved)
            QCOMPARE(read(indexPath), indexBefore);
    }
    void publicOwnedProcessIsReaped_data()
    {
        QTest::addColumn<bool>("destructor");
        QTest::addColumn<bool>("ignoreTerm");
        QTest::newRow("normal-stop") << false << false;
        QTest::newRow("stop-escalation") << false << true;
        QTest::newRow("destructor-no-event-loop") << true << true;
    }
    void publicOwnedProcessIsReaped()
    {
        QFETCH(bool, destructor);
        QFETCH(bool, ignoreTerm);
        pid_t pid = -1;
        {
            SessionFixture fixture;
            QVERIFY(fixture.init({{"ignore_term", ignoreTerm}}));
            fixture.session.start(fixture.profile());
            QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
            pid = fixture.events(QStringLiteral("transport")).front().value(QStringLiteral("pid")).toInt();
            QVERIFY(pid > 0);
            QCOMPARE(::kill(pid, 0), 0);
            QCOMPARE(::getsid(pid), pid); // Real production child owns a private process group/session.
            QCOMPARE(::getpgid(pid), pid);
            if (!destructor) {
                fixture.session.stop();
                if (ignoreTerm) {
                    QTest::qWait(150);
                    QVERIFY(fixture.session.active());
                    QCOMPARE(fixture.ended.count(), 0);
                    QCOMPARE(::kill(pid, 0), 0);
                }
                QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3500);
                QVERIFY(!fixture.session.active());
                QCOMPARE(fixture.errors.count(), 0);
                fixture.session.stop();
                QTest::qWait(100);
                QCOMPARE(fixture.ended.count(), 1);
            }
            // Destructor case intentionally performs no stop/event processing here.
        }
        int status = 0;
        const pid_t waited = ::waitpid(pid, &status, WNOHANG);
        const int waitError = errno;
        QCOMPARE(waited, pid_t(-1));
        QCOMPARE(waitError, ECHILD); // Session already reaped the exact observed fixture child.
        const int signalled = ::kill(pid, 0);
        const int signalError = errno;
        QCOMPARE(signalled, -1);
        QCOMPARE(signalError, ESRCH);
    }
    void publicDiagnosticsAreOptIn()
    {
        SessionFixture fixture;
        QVERIFY(fixture.init());
        fixture.session.setDiagnosticsEnabled(false);
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        QVERIFY(fixture.output("FIDO2 PIN: ") > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.pins.count(), 1, 3000);
        fixture.session.submitPin(QStringLiteral("fixture-pin"));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.input(), QByteArray("fixture-pin\n"), 3000);
        fixture.session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QCOMPARE(fixture.diagnostics.count(), 0);
        QCOMPARE(fixture.errors.count(), 0);
    }
    void publicSignalOrder_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("version-failure") << QStringLiteral("version") << QStringList{
            QStringLiteral("status:connecting"), QStringLiteral("status:error"), QStringLiteral("error"), QStringLiteral("ended")};
        const QStringList transportFailure{QStringLiteral("status:connecting"), QStringLiteral("status:connecting"),
            QStringLiteral("status:error"), QStringLiteral("error"), QStringLiteral("ended")};
        QTest::newRow("prompt-failure") << QStringLiteral("prompt") << transportFailure;
        QTest::newRow("pin-cancel") << QStringLiteral("pin") << QStringList{
            QStringLiteral("status:connecting"), QStringLiteral("status:connecting"), QStringLiteral("status:awaiting-pin"),
            QStringLiteral("status:disconnecting"), QStringLiteral("status:disconnected"), QStringLiteral("ended")};
        QTest::newRow("child-exit-before-desktop") << QStringLiteral("child") << transportFailure;
    }
    void publicSignalOrder()
    {
        QFETCH(QString, scenario);
        QFETCH(QStringList, expected);
        SessionFixture fixture;
        QJsonObject settings;
        if (scenario == QStringLiteral("version"))
            settings.insert(QStringLiteral("version"), QStringLiteral("This is FreeRDP version 3.31.2\n"));
        QVERIFY(fixture.init(settings));
        fixture.session.start(fixture.profile());
        if (scenario != QStringLiteral("version")) {
            QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
            if (scenario == QStringLiteral("prompt"))
                QVERIFY(fixture.output("Password: ") > 0);
            else if (scenario == QStringLiteral("pin")) {
                QVERIFY(fixture.output("FIDO2 PIN: ") > 0);
                QTRY_COMPARE_WITH_TIMEOUT(fixture.pins.count(), 1, 3000);
                fixture.session.stop();
            } else
                QVERIFY(fixture.exit(0));
        }
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QTest::qWait(100);
        QCOMPARE(fixture.lifecycle, expected); // One combined ordering across status/error/end signals.
        QCOMPARE(fixture.errors.count(), scenario == QStringLiteral("pin") ? 0 : 1);
        QCOMPARE(fixture.input(), QByteArray{});
        QVERIFY(!fixture.session.active());
    }
    void publicPythonIgnoresInheritedModulePath()
    {
        SessionFixture fixture;
        QVERIFY(fixture.init());
        // Without -I, startup imports this poison via PYTHONPATH before our adapter runs.
        QVERIFY(fixture.write(QStringLiteral("sitecustomize.py"),
            "from pathlib import Path\nPath(__file__).with_name('poison-ran').write_text('synthetic')\nraise SystemExit('synthetic Python isolation control')\n"));
        fixture.environment("PYTHONPATH", QFile::encodeName(fixture.path(QString{})));
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        QCOMPARE(fixture.events(QStringLiteral("version")).size(), 1);
        QVERIFY(!QFileInfo::exists(fixture.path(QStringLiteral("poison-ran"))));
        QCOMPARE(fixture.errors.count(), 0);
        fixture.session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QVERIFY(!QFileInfo::exists(fixture.path(QStringLiteral("poison-ran"))));
    }
    void publicConfigLifetimeAtFailure_data()
    {
        QTest::addColumn<bool>("promptFailure");
        QTest::newRow("prompt-error-before-reap") << true;
        QTest::newRow("child-exit-error-after-reap") << false;
    }
    void publicConfigLifetimeAtFailure()
    {
        QFETCH(bool, promptFailure);
        QList<bool> atError, atEnd;
        SessionFixture fixture;
        QVERIFY(fixture.init());
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        const auto args = fixture.events(QStringLiteral("transport")).front().value(QStringLiteral("argv")).toArray();
        const QString tune = args.last().toString();
        const QString marker = QStringLiteral("FreeRDP_ConfigPath:");
        QVERIFY(tune.contains(marker));
        const QString config = tune.mid(tune.lastIndexOf(marker) + marker.size());
        QVERIFY(QFileInfo(config).isDir());
        connect(&fixture.session, &Session::error, &fixture.session,
            [&, config] { atError.append(QFileInfo::exists(config)); });
        connect(&fixture.session, &Session::ended, &fixture.session,
            [&, config] { atEnd.append(QFileInfo::exists(config)); });
        if (promptFailure)
            QVERIFY(fixture.output("Password: ") > 0);
        else
            QVERIFY(fixture.exit(1));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QCOMPARE(atError, QList<bool>{promptFailure});
        QCOMPARE(atEnd, QList<bool>{false});
        QVERIFY(!QFileInfo::exists(config));
        QCOMPARE(fixture.errors.count(), 1);
    }
    void publicConsecutiveAuthorizationStatePresence_data()
    {
        QTest::addColumn<bool>("emptyFirst");
        QTest::newRow("empty-then-absent") << true;
        QTest::newRow("absent-then-empty") << false;
    }
    void publicConsecutiveAuthorizationStatePresence()
    {
        QFETCH(bool, emptyFirst);
        SessionFixture fixture;
        QVERIFY(fixture.init());
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("terminal")).size(), 1, 3000);
        QVERIFY(fixture.output(fixtureAuthorization(emptyFirst ? QByteArray("&state=") : QByteArray{})) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QCOMPARE(fixture.auth.count(), 0);
        QCOMPARE(fixture.errors.count(), 1);
        QCOMPARE(fixture.input(), QByteArray{});
    }
    void publicDestroyPendingVersion_data()
    {
        QTest::addColumn<bool>("cancel");
        QTest::newRow("checking") << false;
        QTest::newRow("ending") << true;
    }
    void publicDestroyPendingVersion()
    {
        QFETCH(bool, cancel);
        auto fixture = std::make_unique<SessionFixture>();
        QVERIFY(fixture->init({{"version_delay", 8}}));
        QSignalSpy statuses(&fixture->session, &Session::statusChanged);
        QSignalSpy errors(&fixture->session, &Session::error);
        QSignalSpy ended(&fixture->session, &Session::ended);
        QSignalSpy diagnostics(&fixture->session, &Session::diagnosticEvent);
        fixture->session.start(fixture->profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture->events(QStringLiteral("version")).size(), 1, 3000);
        const pid_t pid = fixture->events(QStringLiteral("version")).front().value(QStringLiteral("pid")).toInt();
        QVERIFY(pid > 0);
        QCOMPARE(::kill(pid, 0), 0);
        QCOMPARE(fixture->events(QStringLiteral("transport")).size(), 0);
        if (cancel)
            fixture->session.stop(); // No event processing before destruction of Ending.
        QVERIFY(fixture->session.active());
        const int statusCount = statuses.count();
        const int diagnosticCount = diagnostics.count();
        QCOMPARE(errors.count(), 0);
        QCOMPARE(ended.count(), 0);
        QElapsedTimer teardown;
        teardown.start();
        fixture.reset(); // No stop or event processing on the Checking path.
        QVERIFY(teardown.elapsed() < 3000); // Must not wait out the eight-second reply.
        QCOMPARE(statuses.count(), statusCount);
        QCOMPARE(diagnostics.count(), diagnosticCount);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(ended.count(), 0);
        int status = 0;
        const pid_t waited = ::waitpid(pid, &status, WNOHANG);
        const int waitError = errno;
        QCOMPARE(waited, pid_t(-1));
        QCOMPARE(waitError, ECHILD);
        const int signalled = ::kill(pid, 0);
        const int signalError = errno;
        QCOMPARE(signalled, -1);
        QCOMPARE(signalError, ESRCH);
        QTest::qWait(100); // Queued completion must not emit after destruction either.
        QCOMPARE(statuses.count(), statusCount);
        QCOMPARE(diagnostics.count(), diagnosticCount);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(ended.count(), 0);
    }
    void publicVersionCancellationRemainsActiveUntilEnd()
    {
        QList<bool> activeAtStatus;
        QList<int> endsAtStatus;
        QStringList details;
        SessionFixture fixture;
        QVERIFY(fixture.init({{"version_delay", 0.3}}));
        connect(&fixture.session, &Session::statusChanged, &fixture.session,
            [&](const QString& phase, const QString& detail) {
                if (phase == QStringLiteral("disconnected")) {
                    activeAtStatus.append(fixture.session.active());
                    endsAtStatus.append(fixture.ended.count());
                    details.append(detail);
                }
            });
        fixture.session.start(fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.events(QStringLiteral("version")).size(), 1, 3000);
        fixture.session.stop();
        QCOMPARE(activeAtStatus, QList<bool>{true});
        QCOMPARE(endsAtStatus, QList<int>{0});
        QCOMPARE(details, QStringList{QStringLiteral("Connection cancelled before FreeRDP started.")});
        QVERIFY(fixture.session.active());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.ended.count(), 1, 3000);
        QVERIFY(!fixture.session.active());
        QTest::qWait(400);
        QCOMPARE(fixture.events(QStringLiteral("transport")).size(), 0);
        QCOMPARE(fixture.errors.count(), 0);
        fixture.session.stop();
        QCOMPARE(fixture.ended.count(), 1);
    }
};
