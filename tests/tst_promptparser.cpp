#include "promptparser.h"
#include <QtTest>

namespace {
const QByteArray authorization =
    "https://login.microsoftonline.com/fixture-tenant/oauth2/v2.0/authorize?"
    "client_id=a85cf173-4192-42f8-81fa-777a763e6e2c&response_type=code&scope=https%3A%2F%2Fwww.wvd.microsoft.com%2F.default+openid+profile+offline_access&code_challenge=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb&code_challenge_method=S256&state=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa&redirect_uri=https%3A%2F%2Flogin.microsoftonline.com%2Fcommon%2Foauth2%2Fnativeclient";

QByteArray fingerprint(char digit)
{
    QByteArray result;
    for (int i = 0; i < 32; ++i) {
        if (i)
            result.append(':');
        result.append(digit);
        result.append(digit);
    }
    return result;
}

QByteArray certificate(bool changed)
{
    QByteArray result = changed
        ? "!!!Certificate for fixture.example:443 (RDP-Redirect) has changed!!!\n\nNew Certificate details:\n"
        : "Certificate details for fixture.example:443 (RDP-Gateway):\n";
    result += "\tCommon Name: fixture.example\n\tSubject:     /CN=fixture.example\n\tIssuer:      /CN=Fixture CA\n";
    result += "\tValid from:  fixture-date\n\tValid to:    fixture-date\n\tThumbprint:  " + fingerprint('a') + '\n';
    if (changed) {
        result += "\nOld Certificate details:\n\tSubject:     /CN=old.example\n\tIssuer:      /CN=Old CA\n";
        result += "\tThumbprint:  " + fingerprint('b') + "\n\n";
        result += "The above X.509 certificate does not match the certificate used for previous connections.\n"
                  "This may indicate that the certificate has been tampered with.\n"
                  "Please contact the administrator of the RDP server and clarify.\n";
    } else {
        result += "The above X.509 certificate could not be verified, possibly because you do not have\n"
                  "the CA certificate in your certificate store, or the certificate has expired.\n"
                  "Please look at the OpenSSL documentation on how to add a private CA to the store.\n";
    }
    result += "Do you trust the above certificate? (Y/T/N) ";
    return result;
}

QList<PromptParser::Event> feedBytes(PromptParser& parser, const QByteArray& bytes, bool bytewise)
{
    if (!bytewise)
        return parser.feed(bytes);
    QList<PromptParser::Event> events;
    for (char c : bytes)
        events.append(parser.feed(QByteArray(1, c)));
    return events;
}

void compareEvent(const PromptParser::Event& event, PromptParser::Kind kind,
                  const QUrl& url = QUrl(), const QString& detail = QString())
{
    QCOMPARE(event.kind, kind);
    QCOMPARE(event.url.toEncoded(), url.toEncoded());
    QVERIFY(event.host.isEmpty());
    QCOMPARE(event.port, quint16(0));
    QVERIFY(event.fingerprint.isEmpty());
    QVERIFY(!event.changed);
    QCOMPARE(event.detail, detail);
}

bool containsKind(const QList<PromptParser::Event>& events, PromptParser::Kind kind)
{
    for (const auto& event : events) {
        if (event.kind == kind)
            return true;
    }
    return false;
}
}

class PromptParserTest : public QObject
{
    Q_OBJECT
private slots:
    void consumedPromptIgnoresTailUntilNewline_data();
    void consumedPromptIgnoresTailUntilNewline();
    void rejectedLineIgnoresSuffixUntilNewline_data();
    void rejectedLineIgnoresSuffixUntilNewline();
    void resetAcceptsPromptWithoutNewline_data();
    void resetAcceptsPromptWithoutNewline();
    void diagnosticVocabularyPrecedence_data();
    void diagnosticVocabularyPrecedence();
    void everyPromptSplit();
    void bytewiseAuthorizationAndTwoRounds();
    void resetDropsPreviousAuthorizationAndFragments();
    void maliciousAuthorizationEndpoints();
    void callbackBoundaries();
    void modernRequestContract_data();
    void modernRequestContract();
    void strictCallbackCodec_data();
    void strictCallbackCodec();
    void changedCertificateUsesNewFingerprint();
    void certificateInjectedOrDuplicateFieldsFail();
    void overflowCannotPromoteSuffixToPrompt();
    void unsupportedAndControlPromptsFail();
    void diagnosticsCannotExposeRawSecrets();
    void diagnosticsRecognizePrefixedLines_data();
    void diagnosticsRecognizePrefixedLines();
    void diagnosticsDiscardInteractiveAndSensitiveText();
    void diagnosticsResetPreservesMode();
    void diagnosticsMalformedLinesCannotPromoteSuffix();
};

void PromptParserTest::consumedPromptIgnoresTailUntilNewline_data()
{
    QTest::addColumn<QByteArray>("input");
    QTest::addColumn<bool>("authorizationPrompt");
    QTest::addColumn<bool>("bytewise");
    for (bool bytewise : {false, true}) {
        const QByteArray suffix = bytewise ? "-bytewise" : "-whole";
        const auto pinRow = [bytewise, &suffix](const char* name, const QByteArray& tail) {
            QTest::newRow((name + suffix).constData())
                << QByteArray("FIDO2 PIN: ") + tail << false << bytewise;
        };
        pinRow("duplicate-pin", "FIDO2 PIN: ");
        pinRow("control-after-pin", QByteArray(1, char(0x1b)));
        pinRow("carriage-return-after-pin", "x\ry");
        pinRow("oversized-after-pin", QByteArray(PromptParser::maximumLine + 1, 'x'));
        QTest::newRow(("unsupported-after-authorization" + suffix).constData())
            << QByteArray("Browse to: ") + authorization + "\nPaste redirect URL here: Password: "
            << true << bytewise;
    }
}

void PromptParserTest::consumedPromptIgnoresTailUntilNewline()
{
    QFETCH(QByteArray, input);
    QFETCH(bool, authorizationPrompt);
    QFETCH(bool, bytewise);
    PromptParser parser;
    const auto events = feedBytes(parser, input + "\nFIDO2 PIN: ", bytewise);
    QCOMPARE(events.size(), 2);
    compareEvent(events[0], authorizationPrompt ? PromptParser::Kind::Authorization : PromptParser::Kind::Pin,
                 authorizationPrompt ? QUrl::fromEncoded(authorization) : QUrl());
    compareEvent(events[1], PromptParser::Kind::Pin);
}

void PromptParserTest::rejectedLineIgnoresSuffixUntilNewline_data()
{
    QTest::addColumn<QByteArray>("prefix");
    QTest::addColumn<bool>("bytewise");
    for (bool bytewise : {false, true}) {
        const QByteArray suffix = bytewise ? "-bytewise" : "-whole";
        QTest::newRow(("invalid-control" + suffix).constData()) << QByteArray("prefix\x1b") << bytewise;
        QTest::newRow(("invalid-carriage-return" + suffix).constData()) << QByteArray("prefix\rx") << bytewise;
        QTest::newRow(("overflow" + suffix).constData())
            << QByteArray(PromptParser::maximumLine + 1, 'x') << bytewise;
    }
}

void PromptParserTest::rejectedLineIgnoresSuffixUntilNewline()
{
    QFETCH(QByteArray, prefix);
    QFETCH(bool, bytewise);
    PromptParser parser;
    const QByteArray input = prefix + "FIDO2 PIN: Password: \x1b\rX"
        "ERRCONNECT_AUTHENTICATION_FAILED\nFIDO2 PIN: ";
    const auto events = feedBytes(parser, input, bytewise);
    QCOMPARE(events.size(), 2);
    compareEvent(events[0], PromptParser::Kind::InvalidPrompt);
    compareEvent(events[1], PromptParser::Kind::Pin);
}

void PromptParserTest::resetAcceptsPromptWithoutNewline_data()
{
    QTest::addColumn<QByteArray>("input");
    QTest::addColumn<PromptParser::Kind>("kind");
    QTest::addColumn<bool>("bytewise");
    for (bool bytewise : {false, true}) {
        const QByteArray suffix = bytewise ? "-bytewise" : "-whole";
        QTest::newRow(("consumed" + suffix).constData())
            << QByteArray("FIDO2 PIN: ") << PromptParser::Kind::Pin << bytewise;
        QTest::newRow(("invalid" + suffix).constData())
            << QByteArray("prefix\x1b") << PromptParser::Kind::InvalidPrompt << bytewise;
        QTest::newRow(("overflow" + suffix).constData())
            << QByteArray(PromptParser::maximumLine + 1, 'x') << PromptParser::Kind::InvalidPrompt << bytewise;
    }
}

void PromptParserTest::resetAcceptsPromptWithoutNewline()
{
    QFETCH(QByteArray, input);
    QFETCH(PromptParser::Kind, kind);
    QFETCH(bool, bytewise);
    PromptParser parser;
    const auto beforeReset = feedBytes(parser, input, bytewise);
    QCOMPARE(beforeReset.size(), 1);
    compareEvent(beforeReset[0], kind);
    parser.reset();
    const auto afterReset = feedBytes(parser, "FIDO2 PIN: ", bytewise);
    QCOMPARE(afterReset.size(), 1);
    compareEvent(afterReset[0], PromptParser::Kind::Pin);
}

void PromptParserTest::diagnosticVocabularyPrecedence_data()
{
    QTest::addColumn<QByteArray>("tokens");
    QTest::addColumn<QString>("detail");
    QTest::addColumn<bool>("diagnosticsOnly");
    QTest::addColumn<bool>("bytewise");
    for (bool diagnosticsOnly : {false, true}) {
        for (bool bytewise : {false, true}) {
            const QByteArray suffix = QByteArray(diagnosticsOnly ? "-diagnostics" : "-interactive") +
                (bytewise ? "-bytewise" : "-whole");
            const auto row = [diagnosticsOnly, bytewise, &suffix](const char* name,
                                                                const QByteArray& tokens,
                                                                const QString& detail) {
                QTest::newRow((name + suffix).constData()) << tokens << detail << diagnosticsOnly << bytewise;
            };
            const QString auth = QStringLiteral("FreeRDP reported authentication failure.");
            const QString transport = QStringLiteral("FreeRDP reported a transport connection failure.");
            row("transport-auth", "ERRCONNECT_CONNECT_TRANSPORT_FAILED ERRCONNECT_AUTHENTICATION_FAILED", auth);
            row("auth-transport", "ERRCONNECT_AUTHENTICATION_FAILED ERRCONNECT_CONNECT_TRANSPORT_FAILED", auth);
            row("security-transport-auth", "ERRCONNECT_SECURITY_NEGO_CONNECT_FAILED "
                "ERRCONNECT_CONNECT_TRANSPORT_FAILED ERRCONNECT_AUTHENTICATION_FAILED", auth);
            row("auth-transport-security", "ERRCONNECT_AUTHENTICATION_FAILED "
                "ERRCONNECT_CONNECT_TRANSPORT_FAILED ERRCONNECT_SECURITY_NEGO_CONNECT_FAILED", auth);
            row("security-transport", "ERRCONNECT_SECURITY_NEGO_CONNECT_FAILED ERRCONNECT_CONNECT_TRANSPORT_FAILED", transport);
            row("transport-security", "ERRCONNECT_CONNECT_TRANSPORT_FAILED ERRCONNECT_SECURITY_NEGO_CONNECT_FAILED", transport);
        }
    }
}

void PromptParserTest::diagnosticVocabularyPrecedence()
{
    QFETCH(QByteArray, tokens);
    QFETCH(QString, detail);
    QFETCH(bool, diagnosticsOnly);
    QFETCH(bool, bytewise);
    PromptParser parser(diagnosticsOnly ? PromptParser::Mode::DiagnosticsOnly : PromptParser::Mode::Interactive);
    const auto events = feedBytes(parser, "[ERROR] " + tokens + " fixture-private-text\r\n", bytewise);
    QCOMPARE(events.size(), 1);
    compareEvent(events[0], PromptParser::Kind::Diagnostic, QUrl(), detail);
}

void PromptParserTest::everyPromptSplit()
{
    const QList<QByteArray> prompts = {
        "FIDO2 PIN:       ",
        "Browse to: " + authorization + "\r\nPaste redirect URL here: ",
        certificate(false), certificate(true)
    };
    const QList<PromptParser::Kind> kinds = {
        PromptParser::Kind::Pin, PromptParser::Kind::Authorization,
        PromptParser::Kind::Certificate, PromptParser::Kind::Certificate
    };
    for (qsizetype row = 0; row < prompts.size(); ++row) {
        for (qsizetype split = 0; split <= prompts[row].size(); ++split) {
            PromptParser parser;
            auto events = parser.feed(prompts[row].left(split));
            events.append(parser.feed(prompts[row].mid(split)));
            QCOMPARE(events.size(), 1);
            QCOMPARE(events.front().kind, kinds[row]);
            if (kinds[row] == PromptParser::Kind::Authorization)
                QCOMPARE(events.front().url.toEncoded(), authorization);
            if (kinds[row] == PromptParser::Kind::Certificate) {
                QCOMPARE(events.front().host, QStringLiteral("fixture.example"));
                QCOMPARE(events.front().port, quint16(443));
                QCOMPARE(events.front().fingerprint, QByteArray(32, char(0xaa)));
            }
        }
    }
}

void PromptParserTest::bytewiseAuthorizationAndTwoRounds()
{
    PromptParser parser;
    const QByteArray round = "Browse to: " + authorization + "\nPaste redirect URL here: \n";
    for (int pass = 0; pass < 2; ++pass) {
        QList<PromptParser::Event> events;
        for (char c : round)
            events.append(parser.feed(QByteArray(1, c)));
        QCOMPARE(events.size(), 1);
        QCOMPARE(events.front().kind, PromptParser::Kind::Authorization);
        QCOMPARE(events.front().url.toEncoded(), authorization);
    }
    QVERIFY(containsKind(parser.feed("Paste redirect URL here: "), PromptParser::Kind::InvalidPrompt));
}

void PromptParserTest::resetDropsPreviousAuthorizationAndFragments()
{
    PromptParser parser;
    parser.feed("Browse to: " + authorization + "\nFIDO2");
    parser.reset();
    QVERIFY(!containsKind(parser.feed(" PIN: \n"), PromptParser::Kind::Pin));
    QVERIFY(containsKind(parser.feed("Paste redirect URL here: "), PromptParser::Kind::InvalidPrompt));
    parser.reset();
    QVERIFY(containsKind(parser.feed("FIDO2 PIN: "), PromptParser::Kind::Pin));
}

void PromptParserTest::maliciousAuthorizationEndpoints()
{
    const QList<QByteArray> urls = {
        "http://login.microsoftonline.com/common/oauth2/v2.0/authorize",
        "https://login.microsoftonline.com.attacker.invalid/common/oauth2/v2.0/authorize",
        "https://login.microsoftonline.com@attacker.invalid/common/oauth2/v2.0/authorize",
        "https://login.microsoftonline.com:444/common/oauth2/v2.0/authorize",
        authorization + "&redirect_uri=https%3A%2F%2Fattacker.invalid%2Fcallback",
        authorization + "&state=one&state=two",
        authorization + "#fragment"
    };
    for (const auto& url : urls) {
        PromptParser parser;
        const auto events = parser.feed("Browse to: " + url + "\nPaste redirect URL here: ");
        QVERIFY(containsKind(events, PromptParser::Kind::InvalidPrompt));
        QVERIFY(!containsKind(events, PromptParser::Kind::Authorization));
    }
    PromptParser parser;
    QVERIFY(!containsKind(parser.feed("log: Browse to: " + authorization + '\n'), PromptParser::Kind::Authorization));
}

void PromptParserTest::callbackBoundaries()
{
    QVERIFY(PromptParser::validCallback(QUrl(QStringLiteral("https://login.microsoftonline.com/common/oauth2/nativeclient?code=fixture&state=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"))));
    for (const QString& value : {
        QStringLiteral("https://login.microsoftonline.com/common/oauth2/nativeclient/extra?code=fixture"),
        QStringLiteral("https://login.microsoftonline.com.attacker.invalid/common/oauth2/nativeclient?code=fixture"),
        QStringLiteral("https://user@login.microsoftonline.com/common/oauth2/nativeclient?code=fixture"),
        QStringLiteral("https://login.microsoftonline.com/common/oauth2/nativeclient?code=one&code=two"),
        QStringLiteral("https://login.microsoftonline.com/common/oauth2/nativeclient?code=fixture&error=denied"),
        QStringLiteral("https://login.microsoftonline.com/common/oauth2/nativeclient?code="),
        QStringLiteral("https://login.microsoftonline.com/common/oauth2/nativeclient?code=fixture%0Ainjection"),
        QStringLiteral("https://login.microsoftonline.com/common/oauth2/nativeclient?code=fixture%7F"),
        QStringLiteral("https://login.microsoftonline.com/common/oauth2/nativeclient?code=fixture#fragment")
    })
        QVERIFY(!PromptParser::validCallback(QUrl(value)));
    const QString endpoint = QStringLiteral("https://login.microsoftonline.com/common/oauth2/nativeclient?code=");
    const QString suffix = "&state=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const int available = 4095 - endpoint.toLatin1().size() - suffix.size() - 1;
    QVERIFY(PromptParser::validCallback(QUrl(endpoint + QString(available, 'a') + suffix)));
    QVERIFY(!PromptParser::validCallback(QUrl(endpoint + QString(available + 1, 'a') + suffix)));
    // Encoded bytes, not decoded character count, determine the terminal limit.
    QVERIFY(!PromptParser::validCallback(QUrl(endpoint + QString(1400, QChar(0x20ac)))));
}

void PromptParserTest::modernRequestContract_data()
{
    QTest::addColumn<QByteArray>("query");
    QTest::addColumn<bool>("accepted");
    const QByteArray original = authorization.mid(authorization.indexOf('?') + 1);
    const auto altered = [&](const char* label, const QByteArray& from, const QByteArray& to, bool accepted = false) {
        QByteArray value = original;
        value.replace(from, to);
        QTest::newRow(label) << value << accepted;
    };
    QTest::newRow("modern-avd") << original << true;
    altered("decoded-key", "client_id=", "%63lient_id=", true);
    altered("percent-space-scope", "+openid+profile+offline_access", "%20openid%20profile%20offline_access", true);
    altered("rds-label", "https%3A%2F%2Fwww.wvd.microsoft.com%2F.default+openid+profile+offline_access", "ms-device-service://termsrv.wvd.microsoft.com/name/fixture-host/user_impersonation", true);
    altered("rds-leading-hyphen", "https%3A%2F%2Fwww.wvd.microsoft.com%2F.default+openid+profile+offline_access", "ms-device-service://termsrv.wvd.microsoft.com/name/-host/user_impersonation");
    altered("rds-long-label", "https%3A%2F%2Fwww.wvd.microsoft.com%2F.default+openid+profile+offline_access", "ms-device-service://termsrv.wvd.microsoft.com/name/" + QByteArray(64, 'x') + "/user_impersonation");
    altered("wrong-client", "a85cf173-4192-42f8-81fa-777a763e6e2c", "fixture");
    altered("wrong-scope", "www.wvd.microsoft.com", "fixture.invalid");
    altered("scope-order", "+openid+profile", "+profile+openid");
    altered("plain", "=S256", "=plain");
    altered("missing-state", "&state=" + QByteArray(64, 'a'), "");
    altered("empty-state", "&state=" + QByteArray(64, 'a'), "&state=");
    altered("missing-challenge", "&code_challenge=" + QByteArray(43, 'b'), "");
    altered("challenge-padding", QByteArray(43, 'b'), QByteArray(42, 'b') + "=");
    altered("invalid-escape", "client_id=", "client_id=%GG");
    altered("invalid-utf8", "client_id=", "client_id=%FF");
    altered("incomplete-state-utf8", "&state=" + QByteArray(64, 'a'), "&state=" + QByteArray(64, 'a') + "%C3");
    altered("incomplete-challenge-utf8", QByteArray(43, 'b'), QByteArray(43, 'b') + "%E2%82");
    altered("incomplete-key-utf8", "client_id=", "client_id%F0%9F%92=");
    altered("control", "client_id=", "client_id=%00");
    QTest::newRow("decoded-duplicate") << (original + "&%73tate=" + QByteArray(64, 'a')) << false;
    QTest::newRow("unknown-field") << (original + "&login_hint=fixture") << false;
}

void PromptParserTest::modernRequestContract()
{
    QFETCH(QByteArray, query);
    QFETCH(bool, accepted);
    const QByteArray raw = authorization.left(authorization.indexOf('?') + 1) + query;
    const auto contract = OAuthContract::request(QUrl::fromEncoded(raw, QUrl::StrictMode));
    QCOMPARE(contract.has_value(), accepted);
    PromptParser parser;
    const auto events = parser.feed("Browse to: " + raw + "\nPaste redirect URL here: ");
    QCOMPARE(containsKind(events, PromptParser::Kind::Authorization), accepted);
    if (accepted) {
        QCOMPARE(contract->authorization, QUrl::fromEncoded(raw, QUrl::StrictMode));
        QVERIFY(events.front().authorization);
        QCOMPARE(events.front().authorization->state, contract->state);
    }
}

void PromptParserTest::strictCallbackCodec_data()
{
    QTest::addColumn<QByteArray>("query");
    QTest::addColumn<QByteArray>("encoded");
    QTest::newRow("literal-plus") << QByteArray("code=a+b") << QByteArray("a%20b");
    QTest::newRow("encoded-plus") << QByteArray("code=a%2Bb") << QByteArray("a%2Bb");
    QTest::newRow("unicode") << QByteArray("%63ode=%E2%82%AC%252B") << QByteArray("%E2%82%AC%252B");
    QTest::newRow("duplicates") << QByteArray("code=a&%63ode=b") << QByteArray{};
    QTest::newRow("duplicate-state") << (QByteArray("code=a&%73tate=") + QByteArray(64, 'a')) << QByteArray{};
    QTest::newRow("utf8-overlong") << QByteArray("code=%C0%AF") << QByteArray{};
    QTest::newRow("surrogate") << QByteArray("code=%ED%A0%80") << QByteArray{};
    QTest::newRow("incomplete-two-byte-utf8") << QByteArray("code=a%C3") << QByteArray{};
    QTest::newRow("incomplete-three-byte-utf8") << QByteArray("code=a%E2%82") << QByteArray{};
    QTest::newRow("incomplete-four-byte-utf8") << QByteArray("code=a%F0%9F%92") << QByteArray{};
    QTest::newRow("incomplete-key-utf8") << QByteArray("code%C3=a") << QByteArray{};
    QTest::newRow("incomplete-metadata-utf8") << QByteArray("code=a&session_state=x%E2%82") << QByteArray{};
    QTest::newRow("malformed") << QByteArray("code=%GG") << QByteArray{};
    QTest::newRow("truncated-escape") << QByteArray("code=%2") << QByteArray{};
    QTest::newRow("decoded-control") << QByteArray("code=%C2%85") << QByteArray{};
    QTest::newRow("mixture") << QByteArray("code=a&error=denied") << QByteArray{};
    QTest::newRow("empty") << QByteArray("code=") << QByteArray{};
}

void PromptParserTest::strictCallbackCodec()
{
    QFETCH(QByteArray, query);
    QFETCH(QByteArray, encoded);
    const QString state(64, 'a');
    const QByteArray endpoint = "https://login.microsoftonline.com/common/oauth2/nativeclient";
    const QUrl url = QUrl::fromEncoded(endpoint + '?' + query + "&state=" + state.toUtf8(), QUrl::StrictMode);
    const auto result = OAuthContract::reply(url, state);
    const QByteArray line = OAuthContract::serialize(result);
    QCOMPARE(line, encoded.isEmpty() ? QByteArray{} : endpoint + "?code=" + encoded + "&state=" + state.toUtf8() + '\n');
    QVERIFY(OAuthContract::serialize(OAuthContract::reply(url, state.toUpper())).isEmpty());
}

void PromptParserTest::changedCertificateUsesNewFingerprint()
{
    PromptParser parser;
    const auto events = parser.feed(certificate(true));
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.front().kind, PromptParser::Kind::Certificate);
    QVERIFY(events.front().changed);
    QCOMPARE(events.front().fingerprint, QByteArray(32, char(0xaa)));
    QVERIFY(events.front().fingerprint != QByteArray(32, char(0xbb)));
}

void PromptParserTest::certificateInjectedOrDuplicateFieldsFail()
{
    const QByteArray original = certificate(false);
    QList<QByteArray> corruptions;
    QByteArray duplicate = original;
    duplicate.replace("\tIssuer:      /CN=Fixture CA\n", "\tIssuer:      /CN=Fixture CA\n\tThumbprint:  " + fingerprint('c') + '\n');
    corruptions.append(duplicate);
    QByteArray header = original;
    header.replace("\tCommon Name: fixture.example\n", "\tCommon Name: fixture.example\nCertificate details for attacker.invalid:443 (RDP-Gateway):\n");
    corruptions.append(header);
    QByteArray premature = original;
    premature.replace("\tSubject:     /CN=fixture.example\n", "Do you trust the above certificate? (Y/T/N) \n");
    corruptions.append(premature);
    QByteArray malformed = original;
    malformed.replace(fingerprint('a'), "aa:bb");
    corruptions.append(malformed);
    for (const auto& value : corruptions) {
        PromptParser parser;
        const auto events = parser.feed(value);
        QVERIFY(containsKind(events, PromptParser::Kind::InvalidPrompt));
        QVERIFY(!containsKind(events, PromptParser::Kind::Certificate));
    }
}

void PromptParserTest::overflowCannotPromoteSuffixToPrompt()
{
    PromptParser parser;
    auto events = parser.feed(QByteArray(PromptParser::maximumLine + 1, 'x') + "FIDO2 PIN: ");
    QVERIFY(containsKind(events, PromptParser::Kind::InvalidPrompt));
    QVERIFY(!containsKind(events, PromptParser::Kind::Pin));
    QVERIFY(!containsKind(parser.feed("Paste redirect URL here: \n"), PromptParser::Kind::Authorization));
    QVERIFY(containsKind(parser.feed("FIDO2 PIN: "), PromptParser::Kind::Pin));
}

void PromptParserTest::unsupportedAndControlPromptsFail()
{
    for (const QByteArray& value : {QByteArray("Password: "), QByteArray("Unknown challenge: "),
                                  QByteArray("GatewayPassword: "),
                                  QByteArray("I understand and agree to the terms of this policy (Y/N) ")}) {
        PromptParser parser;
        QVERIFY(containsKind(parser.feed(value), PromptParser::Kind::UnsupportedPrompt));
    }
    for (const QByteArray& value : {QByteArray("FIDO2\r PIN: \n"), QByteArray("FIDO2\x1b PIN: \n")}) {
        PromptParser parser;
        const auto events = parser.feed(value);
        QVERIFY(containsKind(events, PromptParser::Kind::InvalidPrompt));
        QVERIFY(!containsKind(events, PromptParser::Kind::Pin));
    }
}

void PromptParserTest::diagnosticsCannotExposeRawSecrets()
{
    PromptParser parser;
    const auto events = parser.feed("[ERROR] ERRCONNECT_AUTHENTICATION_FAILED secret-fixture-code\n");
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.front().kind, PromptParser::Kind::Diagnostic);
    QCOMPARE(events.front().detail, QStringLiteral("FreeRDP reported authentication failure."));
    QVERIFY(!events.front().detail.contains(QStringLiteral("secret-fixture-code")));
    QVERIFY(events.front().url.isEmpty());
}

void PromptParserTest::diagnosticsRecognizePrefixedLines_data()
{
    QTest::addColumn<QByteArray>("code");
    QTest::addColumn<QString>("detail");
    QTest::newRow("authentication") << QByteArray("ERRCONNECT_AUTHENTICATION_FAILED")
        << QStringLiteral("FreeRDP reported authentication failure.");
    QTest::newRow("transport") << QByteArray("ERRCONNECT_CONNECT_TRANSPORT_FAILED")
        << QStringLiteral("FreeRDP reported a transport connection failure.");
    QTest::newRow("security") << QByteArray("ERRCONNECT_SECURITY_NEGO_CONNECT_FAILED")
        << QStringLiteral("FreeRDP reported a security negotiation failure.");
}

void PromptParserTest::diagnosticsRecognizePrefixedLines()
{
    QFETCH(QByteArray, code);
    QFETCH(QString, detail);
    const QByteArray line =
        "[13:11:21:394] [128018:0001f414] [ERROR][com.freerdp.core] - [get_next_addrinfo]: " +
        code + " secret-fixture-code https://fixture.invalid/?token=private\r\n";
    const auto verify = [&detail](const QList<PromptParser::Event>& events) {
        QCOMPARE(events.size(), 1);
        const auto& event = events.front();
        QCOMPARE(event.kind, PromptParser::Kind::Diagnostic);
        QCOMPARE(event.detail, detail);
        QVERIFY(event.url.isEmpty());
        QVERIFY(event.host.isEmpty());
        QCOMPARE(event.port, quint16(0));
        QVERIFY(event.fingerprint.isEmpty());
        QVERIFY(!event.changed);
    };
    for (qsizetype split = 0; split <= line.size(); ++split) {
        PromptParser parser(PromptParser::Mode::DiagnosticsOnly);
        auto events = parser.feed(line.left(split));
        events.append(parser.feed(line.mid(split)));
        verify(events);
    }
    PromptParser parser(PromptParser::Mode::DiagnosticsOnly);
    QList<PromptParser::Event> events;
    for (char c : line)
        events.append(parser.feed(QByteArray(1, c)));
    verify(events);
}

void PromptParserTest::diagnosticsDiscardInteractiveAndSensitiveText()
{
    PromptParser parser(PromptParser::Mode::DiagnosticsOnly);
    const QByteArray input =
        "Browse to: " + authorization + "\nPaste redirect URL here: \nFIDO2 PIN: \n" +
        certificate(true) + "\nPassword: \nUnknown challenge: \n" +
        "log: secret-fixture-code https://fixture.invalid/?token=private\n" +
        "ERRCONNECT_CONNECT_FAILED [0x00020006].\n";
    QList<PromptParser::Event> discarded;
    for (char c : input)
        discarded.append(parser.feed(QByteArray(1, c)));
    QVERIFY(discarded.isEmpty());
    const auto events = parser.feed("[ERROR] ERRCONNECT_AUTHENTICATION_FAILED secret-fixture-code\n");
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.front().kind, PromptParser::Kind::Diagnostic);
    QCOMPARE(events.front().detail, QStringLiteral("FreeRDP reported authentication failure."));
}

void PromptParserTest::diagnosticsResetPreservesMode()
{
    PromptParser parser(PromptParser::Mode::DiagnosticsOnly);
    parser.feed("[ERROR] ERRCONNECT_");
    parser.reset();
    QVERIFY(parser.feed("AUTHENTICATION_FAILED\nPassword: \nFIDO2 PIN: \n").isEmpty());
    const auto events = parser.feed("[ERROR] - [function]: ERRCONNECT_CONNECT_TRANSPORT_FAILED\n");
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.front().kind, PromptParser::Kind::Diagnostic);
    QCOMPARE(events.front().detail, QStringLiteral("FreeRDP reported a transport connection failure."));
}

void PromptParserTest::diagnosticsMalformedLinesCannotPromoteSuffix()
{
    const QByteArray diagnostic = "ERRCONNECT_AUTHENTICATION_FAILED\n";
    const QList<QByteArray> prefixes = {
        QByteArray("prefix\r"), QByteArray("prefix\x1b"), QByteArray("prefix\0", 7),
        QByteArray("prefix") + char(0x7f), QByteArray(PromptParser::maximumLine + 1, 'x')
    };
    for (const auto& prefix : prefixes) {
        PromptParser parser(PromptParser::Mode::DiagnosticsOnly);
        QList<PromptParser::Event> rejected;
        for (char c : prefix + diagnostic)
            rejected.append(parser.feed(QByteArray(1, c)));
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(rejected.front().kind, PromptParser::Kind::InvalidPrompt);
        QVERIFY(rejected.front().detail.isEmpty());
        QVERIFY(rejected.front().url.isEmpty());
        const auto recovered = parser.feed(diagnostic);
        QCOMPARE(recovered.size(), 1);
        QCOMPARE(recovered.front().kind, PromptParser::Kind::Diagnostic);
        QCOMPARE(recovered.front().detail, QStringLiteral("FreeRDP reported authentication failure."));
    }
    PromptParser parser(PromptParser::Mode::DiagnosticsOnly);
    const QByteArray boundary =
        QByteArray(PromptParser::maximumLine - (diagnostic.size() - 1), 'x') + diagnostic;
    const auto events = parser.feed(boundary);
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.front().kind, PromptParser::Kind::Diagnostic);
    QCOMPARE(events.front().detail, QStringLiteral("FreeRDP reported authentication failure."));
}

QTEST_GUILESS_MAIN(PromptParserTest)
#include "tst_promptparser.moc"
