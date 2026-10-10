#include "synthetic_profile.h"
#include "signature_fixture.h"
#include "profilestore.h"
#include "rdpprofile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSet>
#include <QStringConverter>
#include <QTemporaryDir>
#include <QTest>
#include <chrono>
#include <fcntl.h>
#include <future>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace {
const QByteArray validProfile = supportedProfile + "redirectwebauthn:i:1\r\n";

bool writeFile(const QString& path, const QByteArray& content)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size()
        && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

QByteArray contents(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

QByteArray setting(QByteArray bytes, const QByteArray& key, const QByteArray& value, char type = 's')
{
    const auto start = bytes.indexOf(key + ':');
    const auto end = bytes.indexOf("\r\n", start);
    bytes.replace(start, end - start, key + ':' + type + ':' + value);
    return bytes;
}

QByteArray encoded(const QByteArray& bytes, QStringConverter::Encoding encoding, bool bom = true)
{
    QStringEncoder encoder(encoding, bom ? QStringConverter::Flag::WriteBom : QStringConverter::Flag::Default);
    return encoder.encode(QString::fromUtf8(bytes));
}

// Application-policy fixtures, not observed regional exports or an endpoint registry.
const QByteArray metadataUuid = "ABCDEFAB-1234-5678-9aBc-def012345678";

QByteArray activityValue(const QByteArray& geo = "AU__False", const QByteArray& region = "australiaeast",
                         const QByteArray& ring = "1", const QByteArray& unit = "123",
                         const QByteArray& uuid = metadataUuid)
{
    return "ms-wvd-ep:" + uuid + "?ScaleUnitPath={\"Geo\"%3a\"" + geo
        + "\"%2c\"Ring\"%3a" + ring + "%2c\"Region\"%3a\"" + region
        + "\"%2c\"ScaleUnit\"%3a" + unit + '}';
}

QByteArray metadataUrl(const QByteArray& key, const QByteArray& host = "rdweb-g-au-r1.wvd.microsoft.com")
{
    return "https://" + host + (key == "diagnosticserviceurl" ? QByteArray("/api/arm/DiagnosticEvents/v1")
        : QByteArray("/api/arm/hubdiscovery?resourceId=") + metadataUuid);
}

QByteArray regionalProfile()
{
    // Deliberately mismatched namespaces: no inferred GEO/host/region mapping.
    QByteArray bytes = setting(fullerProfile, "geo", "fixture_secret_marker");
    bytes = setting(bytes, "diagnosticserviceurl", metadataUrl("diagnosticserviceurl"));
    bytes = setting(bytes, "hubdiscoverygeourl", metadataUrl("hubdiscoverygeourl", "rdweb-g-usg-r9.wvd.azure.us"));
    return setting(bytes, "activityhint", activityValue("Other_Token", "eastus2"));
}

void regionalRows(bool accepted)
{
    auto profileRow = [](const QByteArray& name, const QByteArray& bytes) {
        QByteArray lf = bytes;
        lf.replace("\r\n", "\n");
        QTest::newRow(("regional-" + name + "-utf8-crlf").constData()) << bytes;
        QTest::newRow(("regional-" + name + "-utf8-lf").constData()) << lf;
        QTest::newRow(("regional-" + name + "-utf16-crlf").constData()) << encoded(bytes, QStringConverter::Utf16LE);
        QTest::newRow(("regional-" + name + "-utf16-lf").constData()) << encoded(lf, QStringConverter::Utf16LE);
    };
    auto row = [&](const QByteArray& name, const QByteArray& key, const QByteArray& value) {
        profileRow(name, supportedProfile + key + ":s:" + value + "\r\n");
    };
    if (accepted) {
        // Reclassified exact old negatives remain covered as positive opaque tokens.
        for (const auto& token : {"eu", "US", "EUX", "au__false", "Au__False", "AU__false", "AU__FALSE",
                 "AU_False", "AU___False", "AU__True", "AU__0", "AU__", "AU", "EU__False", "US__False",
                 "APAC", "unknown", "xAU__False", "AU__Falsex", "fixture_secret_marker"})
            row(QByteArray("token-") + token, "geo", token);
        for (const auto& token : {QByteArray("A"), QByteArray("Z") + QByteArray(31, '_')}) {
            row("token-boundary-" + QByteArray::number(token.size()), "geo", token);
            row("activity-token-boundary-" + QByteArray::number(token.size()), "activityhint", activityValue(token));
        }
        for (const auto& key : {QByteArray("diagnosticserviceurl"), QByteArray("hubdiscoverygeourl")}) {
            for (const auto& host : {QByteArray("rdweb-g-eu-r1.wvd.microsoft.com"), QByteArray("rdweb-g-au-r1.wvd.microsoft.com"),
                     QByteArray("rdweb-g-us-r0.wvd.microsoft.com"), QByteArray("rdweb-g-usg-r1.wvd.azure.us"),
                     QByteArray("rdweb-g-aa-r9.wvd.azure.us"), QByteArray("rdweb-g-") + QByteArray(16, 'z') + "-r0.wvd.microsoft.com"})
                row(key + '-' + host, key, metadataUrl(key, host));
        }
        for (const auto& region : {QByteArray("westeurope"), QByteArray("northeurope"), QByteArray("australiaeast"),
                 QByteArray("eastus2"), QByteArray("a"), QByteArray("a") + QByteArray(30, '-') + '9', QByteArray("north-europe")})
            row("activity-region-" + region, "activityhint", activityValue("US", region));
        // Exact former activity Geo EU->US and Region westeurope->northeurope controls.
        row("activity-geo-US", "activityhint", activityValue("US", "westeurope", "3", "123",
            "55555555-6666-7777-8888-999999999999"));
        row("activity-region-northeurope-exact-old", "activityhint", activityValue("EU", "northeurope", "3", "123",
            "55555555-6666-7777-8888-999999999999"));
        for (const auto& ring : {"0", "9"})
            for (const auto& unit : {"1", "9999"})
                row(QByteArray("activity-numeric-") + ring + '-' + unit, "activityhint", activityValue("AU__False", "eastus2", ring, unit));
        profileRow("combined-mismatched-namespaces", regionalProfile());
        for (const auto& key : {QByteArray("geo"), QByteArray("diagnosticserviceurl"), QByteArray("hubdiscoverygeourl"), QByteArray("activityhint")}) {
            const QByteArray value = key == "geo" ? QByteArray("AU__False") : key == "activityhint" ? activityValue() : metadataUrl(key);
            profileRow("key-case-" + key, supportedProfile + key.toUpper() + ":s:" + value + "\r\n");
        }
        return;
    }
    for (const auto& token : {QByteArray{}, QByteArray(33, 'a'), QByteArray("0abc"), QByteArray("_abc"),
             QByteArray("a-b"), QByteArray("a.b"), QByteArray("a%5fb"), QByteArray("a\\b"), QByteArray("a\"b"), QByteArray("a:b")}) {
        row("token-invalid-" + token.toHex(), "geo", token);
        row("activity-token-invalid-" + token.toHex(), "activityhint", activityValue(token));
    }
    for (const auto& region : {QByteArray{}, QByteArray(33, 'a'), QByteArray("North"), QByteArray("north_europe"),
             QByteArray("1east"), QByteArray("-east"), QByteArray("east-"), QByteArray("east%32"), QByteArray("east\\us")})
        row("activity-region-invalid-" + region.toHex(), "activityhint", activityValue("US", region));
    for (const auto& ring : {"10", "00", "-1", "+1", "1.0", ""}) {
        row(QByteArray("activity-ring-invalid-") + ring, "activityhint", activityValue("US", "eastus2", ring));
        for (const auto& key : {QByteArray("diagnosticserviceurl"), QByteArray("hubdiscoverygeourl")})
            row(key + "-ring-invalid-" + ring, key, metadataUrl(key, QByteArray("rdweb-g-us-r") + ring + ".wvd.microsoft.com"));
    }
    for (const auto& unit : {"0", "10000", "01", "+1", "-1", "1.0", ""})
        row(QByteArray("activity-unit-invalid-") + unit, "activityhint", activityValue("US", "eastus2", "1", unit));
    for (const auto& uuid : {QByteArray{}, metadataUuid.chopped(1), metadataUuid + '9', '{' + metadataUuid + '}',
             QByteArray("gbcdefab-1234-5678-9abc-def012345678")}) {
        row("activity-uuid-invalid-" + uuid.toHex(), "activityhint", activityValue("US", "eastus2", "1", "123", uuid));
        row("hub-uuid-invalid-" + uuid.toHex(), "hubdiscoverygeourl",
            "https://rdweb-g-us-r1.wvd.microsoft.com/api/arm/hubdiscovery?resourceId=" + uuid);
    }
    for (const auto& key : {QByteArray("diagnosticserviceurl"), QByteArray("hubdiscoverygeourl")}) {
        for (const auto& label : {QByteArray("a"), QByteArray(17, 'a'), QByteArray("US"), QByteArray("us2"), QByteArray("u-s")})
            row(key + "-host-label-invalid-" + label, key, metadataUrl(key, "rdweb-g-" + label + "-r1.wvd.microsoft.com"));
        for (const auto& host : {"rdweb.wvd.microsoft.com", "workspace.wvd.microsoft.com", "a.afdfp-rdgateway.wvd.microsoft.com",
                 "rdweb-g-us-r1.wvd.microsoft.com.evil.test", "extra.rdweb-g-us-r1.wvd.microsoft.com",
                 "rdweb-g-us-r1.wvd.azure.cn", "rdweb-g-us-r1.WVD.microsoft.com", "rdweb-g-us-r1.wvd.MICROSOFT.com",
                 "RDWEB-g-us-r1.wvd.microsoft.com", "rdweb-g-us-r1.wvd.microsoft.com.", "127.0.0.1", "[::1]",
                 "xn--rdweb-g-us-r1.wvd.microsoft.com", "rdweb-g-us-r1XwvdXmicrosoftXcom"})
            row(key + "-authority-invalid-" + host, key, metadataUrl(key, host));
        for (const auto& port : {":443", ":444", ":0443", ":", ":fixture-secret-marker"})
            row(key + "-port-invalid-" + port, key, metadataUrl(key, QByteArray("rdweb-g-us-r1.wvd.microsoft.com") + port));
        const QByteArray url = metadataUrl(key);
        for (const auto& pair : QList<QPair<QByteArray, QByteArray>>{{"https://", "http://"}, {"https://", "HTTPS://"},
                 {"https://", "https://fixture-secret-marker@"}, {"/api/arm/", "/API/arm/"},
                 {"/api/arm/", "/api/%61rm/"}, {"rdweb-g-au", "rdweb-g-%61u"}}) {
            QByteArray changed = url;
            changed.replace(pair.first, pair.second);
            row(key + "-url-invalid-" + pair.second, key, changed);
        }
        for (const auto& suffix : {"/", "/extra", "?", "?x=fixture-secret-marker", "&extra=1", "#fixture-secret-marker"})
            row(key + "-suffix-invalid-" + suffix, key, url + suffix);
    }
    const QByteArray hub = metadataUrl("hubdiscoverygeourl");
    for (const auto& pair : QList<QPair<QByteArray, QByteArray>>{{"resourceId=", "resourceid="}, {"resourceId=", "resource%49d="},
             {"?resourceId=", "?other="}, {"/hubdiscovery?", "/HubDiscovery?"}, {"?resourceId=", "/?resourceId="}}) {
        QByteArray changed = hub;
        changed.replace(pair.first, pair.second);
        row("hub-query-invalid-" + pair.second, "hubdiscoverygeourl", changed);
    }
    row("hub-repeated-param", "hubdiscoverygeourl", hub + "&resourceId=" + metadataUuid);
    row("hub-encoded-separator", "hubdiscoverygeourl", hub + "%26extra=1");
    const QByteArray activity = activityValue();
    for (const auto& pair : QList<QPair<QByteArray, QByteArray>>{{"%3a", "%3A"}, {"%2c", "%2C"}, {"%3a", ":"}, {"%2c", ","},
             {"{", "%7b"}, {"{", "{ "}, {"\"Geo\"", "\"geo\""}, {"ScaleUnitPath=", "scaleUnitPath="},
             {"\"Geo\"%3a\"AU__False\"", "\"Geo\"%3a1"}, {"\"Region\"%3a\"australiaeast\"", "\"Region\"%3a1"},
             {"\"Ring\"%3a1", "\"Ring\"%3a\"1\""}, {"\"ScaleUnit\"%3a123", "\"ScaleUnit\"%3a\"123\""},
             {"}", "%2c\"Geo\"%3a\"US\"}"}, {"}", "%2c\"Extra\"%3a1}"}, {"}", "}&extra=1"}, {"}", "}#fragment"},
             {"\"Geo\"%3a\"AU__False\"%2c\"Ring\"%3a1", "\"Ring\"%3a1%2c\"Geo\"%3a\"AU__False\""}}) {
        QByteArray changed = activity;
        changed.replace(pair.first, pair.second);
        row("activity-structure-invalid-" + pair.second, "activityhint", changed);
    }
    for (const auto& key : {QByteArray("geo"), QByteArray("diagnosticserviceurl"), QByteArray("hubdiscoverygeourl"), QByteArray("activityhint")}) {
        const QByteArray value = key == "geo" ? QByteArray("fixture_secret_marker") : key == "activityhint" ? activity : metadataUrl(key);
        for (const auto& suffix : {QByteArray(" "), QByteArray("\t"), QByteArray::fromHex("c3a9"), QByteArray::fromHex("e282"), QByteArray(1, '\0')})
            row(key + "-character-invalid-" + suffix.toHex(), key, value + suffix);
        row(key + "-leading-space", key, ' ' + value);
        for (const auto& type : {"i", "b", "S", "ss", ""})
            profileRow(key + "-type-invalid-" + type, supportedProfile + key + ':' + type + ':' + value + "\r\n");
        const QByteArray valid = supportedProfile + key + ":s:" + value + "\r\n";
        for (const auto& line : {key + ":s:" + value, key.toUpper() + ":s:" + value, key.toUpper() + ":b:fixture-secret-marker"})
            profileRow(key + "-duplicate-" + line.left(line.indexOf(':') + 3), valid + line + "\r\n");
        for (const auto& forbidden : {"password", "gatewayaccesstoken", "signature", "signscope", "unknown_fixture_secret_key"})
            profileRow(key + "-forbidden-" + forbidden, valid + forbidden + ":s:fixture-secret-marker\r\n");
    }
}

void invalidNameRows()
{
    QTest::addColumn<QString>("name");
    QTest::newRow("empty") << QStringLiteral("");
    QTest::newRow("spaces") << QStringLiteral("   ");
    QTest::newRow("over-limit") << QString(161, u'a');
    QTest::newRow("utf16-over-limit") << (QString::fromUcs4(U"\U0001f4bb").repeated(80) + u'a');
    QTest::newRow("nul") << (QStringLiteral("Cloud") + QChar::Null + QStringLiteral("PC"));
    QTest::newRow("tab") << QStringLiteral("Cloud\tPC");
    QTest::newRow("newline-at-edge") << QStringLiteral("\nCloud PC");
    QTest::newRow("carriage-return") << QStringLiteral("Cloud\rPC");
    QTest::newRow("delete") << (QStringLiteral("Cloud") + QChar(0x7f));
    QTest::newRow("c1-control") << (QStringLiteral("Cloud") + QChar(0x85));
    QTest::newRow("line-separator") << (QStringLiteral("Cloud") + QChar(0x2028));
    QTest::newRow("paragraph-separator") << (QStringLiteral("Cloud") + QChar(0x2029));
}
}

class ProfileTests final : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        temporary = std::make_unique<QTemporaryDir>();
        QVERIFY(temporary->isValid());
        oldDataHome = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", temporary->path().toUtf8());
    }

    void cleanup()
    {
        if (oldDataHome.isNull())
            qunsetenv("XDG_DATA_HOME");
        else
            qputenv("XDG_DATA_HOME", oldDataHome);
        temporary.reset();
    }

    void importPreservesSourceAndPersistsPrivately()
    {
        const QString source = temporary->filePath(QStringLiteral("Enterprise Cloud PC.rdpw"));
        QVERIFY(writeFile(source, validProfile));
        ProfileStore store;
        QString error;
        const Profile profile = store.importFile(source, &error);
        QVERIFY2(!profile.id.isEmpty(), qPrintable(error));
        QCOMPARE(profile.name, QStringLiteral("Enterprise Cloud PC"));
        QVERIFY(profile.path != source);
        QCOMPARE(contents(source), validProfile);
        QCOMPARE(contents(profile.path), validProfile);
        struct stat fileStat {}, directoryStat {};
        QVERIFY(::stat(QFile::encodeName(profile.path).constData(), &fileStat) == 0);
        QVERIFY(::stat(QFile::encodeName(QFileInfo(profile.path).absolutePath()).constData(), &directoryStat) == 0);
        QCOMPARE(fileStat.st_mode & 0777, mode_t(0600));
        QCOMPARE(directoryStat.st_mode & 0777, mode_t(0700));
        ProfileStore reopened;
        QCOMPARE(reopened.profiles().size(), 1);
        QCOMPARE(reopened.profiles().first().id, profile.id);
        QCOMPARE(reopened.profiles().first().path, profile.path);
    }

    void renamePersistsWithoutChangingIdentityOrConnectionBytes()
    {
        const QString source = temporary->filePath(QStringLiteral("original.rdpw"));
        QVERIFY(writeFile(source, validProfile));
        ProfileStore store;
        QString error;
        const auto original = store.importFile(source, &error);
        QVERIFY2(!original.id.isEmpty(), qPrintable(error));
        QSignalSpy changes(&store, &ProfileStore::changed);
        QVERIFY2(store.renameProfile(original.id, QStringLiteral("  Work  Cloud PC  "), &error), qPrintable(error));
        QVERIFY(error.isEmpty());
        QCOMPARE(changes.count(), 1);
        const auto renamed = store.profiles().first();
        QCOMPARE(renamed.name, QStringLiteral("Work  Cloud PC"));
        QCOMPARE(renamed.id, original.id);
        QCOMPARE(renamed.path, original.path);
        QCOMPARE(contents(source), validProfile);
        QCOMPARE(contents(original.path), validProfile);
        ProfileStore reopened;
        QCOMPARE(reopened.profiles().size(), 1);
        QCOMPARE(reopened.profiles().first().name, renamed.name);
        QCOMPARE(reopened.profiles().first().id, original.id);
        QCOMPARE(reopened.profiles().first().path, original.path);
        QVERIFY(store.renameProfile(original.id, QStringLiteral(" Work  Cloud PC "), &error));
        QCOMPARE(changes.count(), 1);
    }

    void duplicateLabelsRemainIndependent()
    {
        const QString source = temporary->filePath(QStringLiteral("original.rdpw"));
        QVERIFY(writeFile(source, validProfile));
        ProfileStore store;
        QString error;
        const auto first = store.importFile(source, &error, QStringLiteral("Shared Cloud PC"));
        QVERIFY2(!first.id.isEmpty(), qPrintable(error));
        const auto second = store.importFile(source, &error, QStringLiteral("Other Cloud PC"));
        QVERIFY2(!second.id.isEmpty(), qPrintable(error));
        QVERIFY(first.id != second.id);
        QVERIFY(first.path != second.path);
        QVERIFY(store.renameProfile(second.id, first.name, &error));
        ProfileStore reopened;
        QCOMPARE(reopened.profiles().size(), 2);
        QCOMPARE(reopened.profiles().at(0).id, first.id);
        QCOMPARE(reopened.profiles().at(1).id, second.id);
        QCOMPARE(reopened.profiles().at(0).name, first.name);
        QCOMPARE(reopened.profiles().at(1).name, first.name);
        QVERIFY(reopened.removeProfile(first.id, &error));
        QCOMPARE(reopened.profiles().size(), 1);
        QCOMPARE(reopened.profiles().first().id, second.id);
        QCOMPARE(contents(second.path), validProfile);
        QCOMPARE(contents(source), validProfile);
    }

    void rejectsInvalidRename_data()
    {
        invalidNameRows();
    }

    void rejectsInvalidRename()
    {
        QFETCH(QString, name);
        const QString source = temporary->filePath(QStringLiteral("original.rdpw"));
        QVERIFY(writeFile(source, validProfile));
        ProfileStore store;
        QString error;
        const auto original = store.importFile(source, &error);
        QVERIFY2(!original.id.isEmpty(), qPrintable(error));
        const QString index = temporary->filePath(QStringLiteral("omawin365/profiles.json"));
        const QByteArray before = contents(index);
        QSignalSpy changes(&store, &ProfileStore::changed);
        QVERIFY(!store.renameProfile(original.id, name, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(changes.count(), 0);
        QCOMPARE(store.profiles().size(), 1);
        QCOMPARE(store.profiles().first().name, original.name);
        QCOMPARE(store.profiles().first().id, original.id);
        QCOMPARE(store.profiles().first().path, original.path);
        QCOMPARE(contents(index), before);
        QCOMPARE(contents(original.path), validProfile);
        QCOMPARE(contents(source), validProfile);
        ProfileStore reopened;
        QCOMPARE(reopened.profiles().first().name, original.name);
    }

    void rejectsUnknownRename()
    {
        const QString source = temporary->filePath(QStringLiteral("original.rdpw"));
        QVERIFY(writeFile(source, validProfile));
        ProfileStore store;
        QString error;
        const auto original = store.importFile(source, &error);
        QVERIFY2(!original.id.isEmpty(), qPrintable(error));
        const QString index = temporary->filePath(QStringLiteral("omawin365/profiles.json"));
        const QByteArray before = contents(index);
        QSignalSpy changes(&store, &ProfileStore::changed);
        QVERIFY(!store.renameProfile(QStringLiteral("not-a-saved-id"), QStringLiteral("Work PC"), &error));
        QCOMPARE(error, QStringLiteral("The selected saved connection no longer exists."));
        QCOMPARE(changes.count(), 0);
        QCOMPARE(store.profiles().first().name, original.name);
        QCOMPARE(contents(index), before);
        QCOMPARE(contents(original.path), validProfile);
        QCOMPARE(contents(source), validProfile);
    }

    void renameFailurePreservesExistingMetadata()
    {
        const QString source = temporary->filePath(QStringLiteral("original.rdpw"));
        QVERIFY(writeFile(source, validProfile));
        ProfileStore store;
        QString error;
        const auto original = store.importFile(source, &error);
        QVERIFY2(!original.id.isEmpty(), qPrintable(error));
        const QString index = temporary->filePath(QStringLiteral("omawin365/profiles.json"));
        const QByteArray before = contents(index);
        QVERIFY(::chmod(QFile::encodeName(index).constData(), 0644) == 0);
        QSignalSpy changes(&store, &ProfileStore::changed);
        QVERIFY(!store.renameProfile(original.id, QStringLiteral("Work PC"), &error));
        QVERIFY(error.contains(QStringLiteral("unsafe permissions or ownership")));
        QCOMPARE(changes.count(), 0);
        QCOMPARE(store.profiles().first().name, original.name);
        QCOMPARE(contents(index), before);
        QCOMPARE(contents(original.path), validProfile);
        QCOMPARE(contents(source), validProfile);
        QVERIFY(::chmod(QFile::encodeName(index).constData(), 0600) == 0);
        ProfileStore reopened;
        QCOMPARE(reopened.profiles().first().name, original.name);
        QVERIFY(store.renameProfile(original.id, QStringLiteral("Work PC"), &error));
        QCOMPARE(changes.count(), 1);
        QCOMPARE(store.profiles().first().name, QStringLiteral("Work PC"));
    }

    void renameRefusesUnsafeRoot()
    {
        const QString source = temporary->filePath(QStringLiteral("original.rdpw"));
        QVERIFY(writeFile(source, validProfile));
        ProfileStore store;
        QString error;
        const auto original = store.importFile(source, &error);
        QVERIFY2(!original.id.isEmpty(), qPrintable(error));
        const QString root = QFileInfo(original.path).absolutePath();
        const QString index = QDir(root).filePath(QStringLiteral("profiles.json"));
        const QByteArray before = contents(index);
        QVERIFY(::chmod(QFile::encodeName(root).constData(), 0755) == 0);
        QSignalSpy changes(&store, &ProfileStore::changed);
        QVERIFY(!store.renameProfile(original.id, QStringLiteral("Work PC"), &error));
        QVERIFY(error.contains(QStringLiteral("0700")));
        QCOMPARE(changes.count(), 0);
        QCOMPARE(store.profiles().first().name, original.name);
        QCOMPARE(contents(index), before);
        QCOMPARE(contents(original.path), validProfile);
        QCOMPARE(contents(source), validProfile);
    }

    void renameRefusesSymlinkReplacement()
    {
        const QString source = temporary->filePath(QStringLiteral("original.rdpw"));
        QVERIFY(writeFile(source, validProfile));
        ProfileStore store;
        QString error;
        const auto original = store.importFile(source, &error);
        QVERIFY2(!original.id.isEmpty(), qPrintable(error));
        const QString index = temporary->filePath(QStringLiteral("omawin365/profiles.json"));
        const QByteArray before = contents(index);
        QVERIFY(QFile::remove(original.path));
        QVERIFY(QFile::link(source, original.path));
        QSignalSpy changes(&store, &ProfileStore::changed);
        QVERIFY(!store.renameProfile(original.id, QStringLiteral("Work PC"), &error));
        QVERIFY(error.contains(QStringLiteral("unsafe ownership or permissions")));
        QCOMPARE(changes.count(), 0);
        QCOMPARE(store.profiles().first().name, original.name);
        QCOMPARE(contents(index), before);
        QCOMPARE(contents(source), validProfile);
    }

    void importsExplicitDisplayName_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("expected");
        QTest::newRow("trim-preserves-internal-spaces") << QStringLiteral("  Portal  Cloud PC  ") << QStringLiteral("Portal  Cloud PC");
        QTest::newRow("160-units") << QString(160, u'a') << QString(160, u'a');
        const QString unicode = QString::fromUcs4(U"\U0001f4bb").repeated(80);
        QTest::newRow("160-units-unicode") << unicode << unicode;
    }

    void importsExplicitDisplayName()
    {
        QFETCH(QString, name);
        QFETCH(QString, expected);
        const QString source = temporary->filePath(QStringLiteral("random-download.rdpw"));
        QVERIFY(writeFile(source, validProfile));
        ProfileStore store;
        QString error;
        const auto profile = store.importFile(source, &error, name);
        QVERIFY2(!profile.id.isEmpty(), qPrintable(error));
        QCOMPARE(profile.name, expected);
        QCOMPARE(contents(source), validProfile);
        QCOMPARE(contents(profile.path), validProfile);
        ProfileStore reopened;
        QCOMPARE(reopened.profiles().size(), 1);
        QCOMPARE(reopened.profiles().first().name, expected);
        QCOMPARE(reopened.profiles().first().id, profile.id);
        QCOMPARE(reopened.profiles().first().path, profile.path);
        QVERIFY(store.renameProfile(profile.id, QStringLiteral("Temporary label"), &error));
        QVERIFY(store.renameProfile(profile.id, name, &error));
        QCOMPARE(store.profiles().first().name, expected);
    }

    void rejectsInvalidInitialDisplayName_data()
    {
        invalidNameRows();
    }

    void rejectsInvalidInitialDisplayName()
    {
        QFETCH(QString, name);
        const QString source = temporary->filePath(QStringLiteral("original.rdpw"));
        QVERIFY(writeFile(source, validProfile));
        ProfileStore store;
        QString error;
        const auto original = store.importFile(source, &error);
        QVERIFY2(!original.id.isEmpty(), qPrintable(error));
        const QDir root(QFileInfo(original.path).absolutePath());
        const QString index = root.filePath(QStringLiteral("profiles.json"));
        const QByteArray before = contents(index);
        const auto filesBefore = root.entryList(QDir::Files | QDir::Hidden);
        QSignalSpy changes(&store, &ProfileStore::changed);
        QVERIFY(store.importFile(source, &error, name).id.isEmpty());
        QVERIFY(!error.isEmpty());
        QCOMPARE(changes.count(), 0);
        QCOMPARE(store.profiles().size(), 1);
        QCOMPARE(store.profiles().first().name, original.name);
        QCOMPARE(root.entryList(QDir::Files | QDir::Hidden), filesBefore);
        QCOMPARE(contents(index), before);
        QCOMPARE(contents(original.path), validProfile);
        QCOMPARE(contents(source), validProfile);
    }

    void importsUtf16WithoutChangingBytes()
    {
        const QString source = temporary->filePath(QStringLiteral("Cloud PC.rdpw"));
        QStringEncoder encoder(QStringConverter::Utf16LE, QStringConverter::Flag::WriteBom);
        const QByteArray encoded = encoder.encode(QString::fromUtf8(validProfile));
        QVERIFY(writeFile(source, encoded));
        ProfileStore store;
        QString error;
        const auto profile = store.importFile(source, &error);
        QVERIFY2(!profile.id.isEmpty(), qPrintable(error));
        QCOMPARE(contents(profile.path), encoded);
    }

    void rejectsInvalidConnections_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("json-wrapper") << QByteArray("{\"url\":\"rdp://example.test\"}");
        QTest::newRow("missing-arm") << QByteArray("full address:s:example.test\ngatewayhostname:s:gateway.example.test\n");
        QTest::newRow("bad-number") << (validProfile + "screen mode id:i:two\n");
        QTest::newRow("embedded-cli-option") << (validProfile + "/cert:ignore\n");
        QTest::newRow("embedded-nul") << (validProfile + QByteArray("username:s:a\0b", 14));
        QTest::newRow("invalid-utf8") << (validProfile + QByteArray("name:s:\xff\xff\n", 10));
        QTest::newRow("over-limit") << QByteArray(1024 * 1024 + 1, 'a');
    }

    void rejectsInvalidConnections()
    {
        QFETCH(QByteArray, bytes);
        const QString source = temporary->filePath(QStringLiteral("bad.rdpw"));
        QVERIFY(writeFile(source, bytes));
        ProfileStore store;
        QString error;
        QVERIFY(store.importFile(source, &error).id.isEmpty());
        QVERIFY(!error.isEmpty());
        QCOMPARE(store.profiles().size(), 0);
        QCOMPARE(contents(source), bytes);
    }

    void supportedSubset_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("minimal-crlf") << supportedProfile;
        QByteArray lf = supportedProfile;
        lf.replace("\r\n", "\n");
        QTest::newRow("lf") << lf;
        QTest::newRow("no-final-separator") << supportedProfile.chopped(2);
        QTest::newRow("empty-lines") << ("\n\r\n" + supportedProfile + "\n\r\n");
        QTest::newRow("utf16le-crlf") << encoded(supportedProfile, QStringConverter::Utf16LE);
        QTest::newRow("utf16le-lf") << encoded(lf, QStringConverter::Utf16LE);
        QByteArray upper = supportedProfile;
        for (const auto& line : supportedProfile.split('\n')) {
            const auto key = line.left(line.indexOf(':'));
            if (!key.isEmpty())
                upper.replace(key + ':', key.toUpper() + ':');
        }
        QTest::newRow("uppercase-keys") << upper;
        QByteArray armCase = supportedProfile;
        armCase.replace("/subscriptions/", "/SUBSCRIPTIONS/");
        armCase.replace("/resourcegroups/", "/RESOURCEGROUPS/");
        armCase.replace("/providers/Microsoft.DesktopVirtualization/hostpools/", "/PROVIDERS/MICROSOFT.DESKTOPVIRTUALIZATION/HOSTPOOLS/");
        QTest::newRow("arm-fixed-case") << armCase;
        QTest::newRow("gateway-443") << setting(supportedProfile, "gatewayhostname", "Gateway.Example.test:443");
        QTest::newRow("dns-one-byte") << setting(supportedProfile, "full address", "a");
        QTest::newRow("dns-label-63") << setting(supportedProfile, "full address", QByteArray(63, 'a'));
        const QByteArray maxDns = QByteArray(63, 'a') + '.' + QByteArray(63, 'b') + '.' + QByteArray(63, 'c') + '.' + QByteArray(61, 'd');
        QTest::newRow("dns-total-253") << setting(supportedProfile, "full address", maxDns);
        QTest::newRow("dns-numeric-inner-labels") << setting(setting(supportedProfile, "full address", "123.0x7f.example.test"),
            "gatewayhostname", "10.0x10.example.test:443");
        QByteArray names = supportedProfile;
        names.replace("fixture_group", QByteArray(90, 'g'));
        names.replace("fixture-pool", QByteArray(64, 'h'));
        QTest::newRow("arm-name-maxima") << names;
        names = supportedProfile;
        names.replace("fixture_group", "_");
        names.replace("fixture-pool", "-");
        QTest::newRow("arm-name-minima") << names;
        QTest::newRow("optional-tenant") << (supportedProfile + "aadtenantid:s:ABCDEFAB-1234-5678-9ABC-DEF012345678\n");
        QByteArray upperUuids = supportedProfile;
        upperUuids.replace("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE");
        upperUuids.replace("01234567-89ab-cdef-0123-456789abcdef", "01234567-89AB-CDEF-0123-456789ABCDEF");
        QTest::newRow("routing-uuid-case") << upperUuids;
        QTest::newRow("optional-provider") << (supportedProfile + "resourceprovider:s:arm\n");
        QTest::newRow("optional-webauthn") << validProfile;
        QTest::newRow("all-optionals") << (validProfile + "aadtenantid:s:abcdefab-1234-5678-9abc-def012345678\nresourceprovider:s:arm\n");
        QTest::newRow("exact-byte-limit") << (supportedProfile + QByteArray(RdpProfile::maximumBytes - supportedProfile.size(), '\n'));
        auto row = [](const QByteArray& name, const QByteArray& bytes) { QTest::newRow(name.constData()) << bytes; };
        row("fuller-utf8-crlf", fullerProfile);
        row("fuller-utf16le-crlf", encoded(fullerProfile, QStringConverter::Utf16LE));
        QByteArray fullerLf = fullerProfile;
        fullerLf.replace("\r\n", "\n");
        row("fuller-utf8-lf", fullerLf);
        row("fuller-utf16le-lf", encoded(fullerLf, QStringConverter::Utf16LE));
        row("fuller-no-final-separator", fullerProfile.chopped(2));
        // Only this observed nonsecret token is real; all other metadata is invented.
        const QByteArray observedGeo = supportedProfile + "geo:s:AU__False\r\n";
        row("geo-observed-utf8", observedGeo);
        row("geo-observed-utf16le", encoded(observedGeo, QStringConverter::Utf16LE));
        const QByteArray fullerGeo = setting(fullerProfile, "geo", "AU__False");
        row("geo-observed-fuller-utf8", fullerGeo);
        row("geo-observed-fuller-utf16le", encoded(fullerGeo, QStringConverter::Utf16LE));
        for (auto line : euOptionalSettings.split('\n')) {
            if (line.isEmpty()) continue;
            line.chop(1);
            const auto key = line.left(line.indexOf(':'));
            row("eu-independent-" + key, supportedProfile + line + "\r\n");
            row("eu-independent-utf16-" + key, encoded(supportedProfile + line + "\r\n", QStringConverter::Utf16LE));
            row("eu-key-case-" + key, supportedProfile + key.toUpper() + line.mid(key.size()) + "\n");
            QByteArray omitted = fullerProfile;
            omitted.remove(omitted.indexOf(key + ':'), line.size() + 2);
            row("eu-optional-omitted-" + key, omitted);
        }
        QByteArray alternateFirst = "alternate full address:s:CLOUDPC.EXAMPLE.TEST\r\n" + supportedProfile;
        row("alternate-before-full-address", alternateFirst);
        for (const auto& hostname : {QByteArray(63, 'a'), QByteArray(63, 'a') + '.' + QByteArray(63, 'b') + '.' + QByteArray(63, 'c') + '.' + QByteArray(61, 'd')}) {
            const auto bytes = setting(setting(fullerProfile, "full address", hostname), "alternate full address", hostname.toUpper());
            row("alternate-equal-dns-boundary-" + QByteArray::number(hostname.size()), bytes);
        }

        for (const auto& key : {"wvd endpoint pool", "workspace id"})
            row(QByteArray("eu-uppercase-uuid-") + key, setting(fullerProfile, key, "ABCDEFAB-1234-5678-9ABC-DEF012345678"));
        for (const auto& size : {"1", "9999"})
            row(QByteArray("desktop-all-boundaries-") + size, setting(fullerProfile, "remotedesktopname",
                QByteArray("Cloud PC Enterprise ") + size + "vCPU/" + size + "GB/" + size + "GB"));
        for (const auto& label : {"Example Desktop 4vCPU/16GB/256GB", "Example Desktop", "Example Desktop 1.2-a_b"})
            row(QByteArray("desktop-other-product-") + label, setting(fullerProfile, "remotedesktopname", label));
        row("desktop-name-boundary", setting(fullerProfile, "remotedesktopname", QByteArray(64, 'a')));
        for (const auto& pair : QList<QPair<QByteArray, QByteArray>>{{"3", "1"}, {"0", "9999"}, {"9", "9999"}}) {
            QByteArray bytes = fullerProfile;
            bytes.replace("Ring\"%3a3", "Ring\"%3a" + pair.first);
            bytes.replace("ScaleUnit\"%3a123", "ScaleUnit\"%3a" + pair.second);
            row("activity-boundary-" + pair.first + '-' + pair.second, bytes);
        }
        QByteArray uuidCase = fullerProfile;
        uuidCase.replace("44444444-5555-6666-7777-888888888888", "ABCDEFAB-1234-5678-9ABC-DEF012345678");
        uuidCase.replace("55555555-6666-7777-8888-999999999999", "ABCDEFAB-1234-5678-9ABC-DEF012345678");
        row("hub-activity-uppercase-uuid", uuidCase);
        for (const auto& geo : {"eu", "US", "EUX"})
            row(QByteArray("geo-") + geo, setting(fullerProfile, "geo", geo));
        for (const auto& key : {QByteArray("diagnosticserviceurl"), QByteArray("hubdiscoverygeourl")}) {
            QByteArray bytes = fullerProfile;
            bytes.replace("rdweb-g-eu-r1", "rdweb-g-us-r1");
            const auto start = bytes.indexOf(key + ":s:") + key.size() + 3;
            row("eu-url-" + key + "-rdweb-g-us-r1", setting(fullerProfile, key,
                bytes.mid(start, bytes.indexOf("\r\n", start) - start)));
        }
        for (const auto& pair : QList<QPair<QByteArray, QByteArray>>{{"\"EU\"", "\"US\""}, {"westeurope", "northeurope"}}) {
            QByteArray bytes = fullerProfile;
            bytes.replace(pair.first, pair.second);
            row("activity-reclassified-" + pair.second, bytes);
        }
        regionalRows(true);
    }

    void supportedSubset()
    {
        QFETCH(QByteArray, bytes);
        QString error = QStringLiteral("stale");
        QVERIFY2(RdpProfile::validate(bytes, &error), qPrintable(error));
        QVERIFY(error.isEmpty());
        const QString source = temporary->filePath(QStringLiteral("download.rdpw"));
        QVERIFY(writeFile(source, bytes));
        ProfileStore store;
        QSignalSpy changes(&store, &ProfileStore::changed);
        // Every accepted grammar row exercises both public import branches.
        for (const auto& name : {QString{}, QStringLiteral("Browser acquired PC")}) {
            const auto profile = store.importFile(source, &error, name);
            QVERIFY2(!profile.id.isEmpty(), qPrintable(error));
            QVERIFY(error.isEmpty());
            QCOMPARE(contents(source), bytes);
            QCOMPARE(contents(profile.path), bytes);
            QByteArray captured;
            QVERIFY2(RdpProfile::readAndValidate(profile.path, &captured, &error), qPrintable(error));
            QCOMPARE(captured, bytes);
            struct stat st {};
            QCOMPARE(::stat(QFile::encodeName(profile.path).constData(), &st), 0);
            QCOMPARE(st.st_mode & 0777, mode_t(0600));
            ProfileStore reopened;
            QCOMPARE(reopened.profiles().last().id, profile.id);
            QCOMPARE(reopened.profiles().last().name, profile.name);
            QCOMPARE(reopened.profiles().last().path, profile.path);
        }
        QCOMPARE(changes.count(), 2);
    }

    void signaturesSupported_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        auto row = [](const QByteArray& tag, const QByteArray& bytes) {
            QByteArray lf = bytes;
            lf.replace("\r\n", "\n");
            QTest::newRow((tag + "-utf8-crlf").constData()) << bytes;
            QTest::newRow((tag + "-utf8-lf").constData()) << lf;
            QTest::newRow((tag + "-utf16-crlf").constData()) << encoded(bytes, QStringConverter::Utf16LE);
            QTest::newRow((tag + "-utf16-lf").constData()) << encoded(lf, QStringConverter::Utf16LE);
        };
        int authRow = 0;
        for (const auto& auth : {QByteArray{}, QByteArray("enablerdsaadauth:i:0\r\n"), QByteArray("enablerdsaadauth:i:1\r\n")})
            row("auth-" + QByteArray::number(authRow++), supportedProfile + auth + signaturePair());
        for (const int size : {1, 2, 3, 12274, 12275, 12276})
            row("payload-bound-" + QByteArray::number(size), supportedProfile + signaturePair("Full Address", syntheticSignature(QByteArray(size, '\xff'))));
        row("private-non-cms", supportedProfile + signaturePair());
        row("scope-before-signature", "SIGNSCOPE:s:Full Address\r\n" + supportedProfile + "SIGNATURE:s:" + syntheticSignature() + "\r\n");
        for (int n = 0; n < 19; ++n) {
            const auto& entry = historicalScopeExamples[n];
            row("scope-" + QByteArray::number(n), fullerProfile + signaturePair(n == 0 ? QByteArray(entry.label) : "Full Address," + QByteArray(entry.label)));
        }
        for (const bool reverse : {false, true}) {
            const auto scope = allSyntheticScopes(reverse);
            QVERIFY(!scope.isEmpty()); // Historical example, not a finite current vocabulary.
            row(reverse ? "all-reverse" : "all", fullerProfile + signaturePair(scope, syntheticSignature(QByteArray(12276, 'x'))));
        }
        const QByteArray paired = supportedProfile + signaturePair();
        QTest::newRow("raw-max-utf8") << (paired + QByteArray(RdpProfile::maximumBytes - paired.size(), '\n'));
        QByteArray utf16Max = encoded(paired, QStringConverter::Utf16LE);
        utf16Max += QByteArray::fromHex("0a00").repeated((RdpProfile::maximumBytes - utf16Max.size()) / 2);
        QTest::newRow("raw-max-utf16") << utf16Max;
        row("no-terminal-separator", paired.chopped(2));
        row("changed-valid-address", setting(paired, "full address", "changed.example.test"));
        row("changed-opaque-payload", supportedProfile + signaturePair("Full Address", syntheticSignature("unrelated changed private bytes")));
        // Exact old rejection vectors whose ONLY removed constraint was shape,
        // cap, canonical text or scope vocabulary/presence are now admissions.
        int reclassified = 0;
        auto formerlyRejected = [&](const QByteArray& bytes) {
            row("reclassified-" + QByteArray::number(reclassified++), bytes);
        };
        formerlyRejected(supportedProfile + signaturePair("Full Address", syntheticSignature(QByteArray(12277, 'x'))));
        QByteArray noncanonicalOnePad = syntheticSignature("xx");
        noncanonicalOnePad[noncanonicalOnePad.size() - 2] = 'h';
        formerlyRejected(supportedProfile + signaturePair("Full Address", noncanonicalOnePad));
        const auto sig = syntheticSignature("x");
        for (const auto& value : {QByteArray(19, 'A'), QByteArray(16385, 'A'), QByteArray(16388, 'A'), sig.chopped(1), sig + "====",
                 sig + ':', QByteArray("prefix:") + sig, sig.left(5) + ' ' + sig.mid(5), sig.left(5) + '-' + sig.mid(6),
                 sig.left(5) + '_' + sig.mid(6), sig.left(5) + '=' + sig.mid(6), sig.chopped(3) + "===", sig.chopped(2) + "B=", sig.chopped(3) + "B=="})
            formerlyRejected(supportedProfile + signaturePair("Full Address", value));
        auto blob = [](quint32 count, const QByteArray& payload, const QByteArray& markers = QByteArray::fromHex("0100010001000000")) {
            QByteArray header = markers;
            for (int shift = 0; shift < 32; shift += 8) header += char(count >> shift);
            return (header + payload).toBase64();
        };
        for (const quint32 count : {0u, 2u, 0xffffffffu})
            formerlyRejected(supportedProfile + signaturePair("Full Address", blob(count, "x")));
        formerlyRejected(supportedProfile + signaturePair("Full Address", blob(1, "x", QByteArray::fromHex("0200010001000000"))));
        formerlyRejected(supportedProfile + signaturePair("Full Address", blob(0, {})));
        formerlyRejected(supportedProfile + signaturePair("Full Address", QByteArray(11, 'x').toBase64()));
        for (const auto& value : {QByteArray(1024, 'A'), QByteArray(1025, 'A'), QByteArray("Full Address,").repeated(20),
                 QByteArray("Full Address,Full Address"), QByteArray(",Full Address"), QByteArray("Full Address,"), QByteArray("Full Address,,AudioMode"),
                 QByteArray("full address"), QByteArray("Full Address, AudioMode"), QByteArray("Full Address,SmartRawPrinters"),
                 QByteArray("Full Address,EnableRdsAadAuth"), QByteArray("GatewayHostname"), QByteArray("Full Address,Server Port")})
            formerlyRejected(fullerProfile + signaturePair(value));
        for (int n = 1; n < 19; ++n) {
            const auto& entry = historicalScopeExamples[n];
            if (supportedProfile.contains(QByteArray(entry.key) + ':')) continue;
            formerlyRejected(supportedProfile + signaturePair("Full Address," + QByteArray(entry.label)));
        }
        // Brand-new opaque text controls, no shallow interpretation of bytes.
        for (const auto& value : {QByteArray("x"), QByteArray("not-base64:private text! <,>[]{}\\\"'&%_~"),
                 QByteArray("unknown label repeated,unknown label repeated"), QByteArray("x  x")}) {
            row("opaque-" + value.toHex(), supportedProfile + signaturePair(value, value));
        }
        QByteArray printable;
        for (char c = 0x20; c <= 0x7e; ++c) printable += c;
        row("every-printable-ascii", supportedProfile + signaturePair('x' + printable + 'x', 'x' + printable + 'x'));
        row("short-reverse-case", "SIGNSCOPE:s:s\r\n" + supportedProfile + "SiGnAtUrE:s:x\r\n");
        // Actual value volume at the RAW file budget, separately for encodings
        // and LF/CRLF. No padding the file with empty lines as a size proxy.
        for (const bool utf16 : {false, true}) {
            for (const bool lf : {false, true}) {
                const QByteArray skeleton = supportedProfile + signaturePair("s", "s");
                QByteArray form = skeleton;
                if (lf) form.replace("\r\n", "\n");
                const qsizetype room = (RdpProfile::maximumBytes - (utf16 ? 2 : 0)) / (utf16 ? 2 : 1) - form.size();
                for (const auto& field : {QByteArray("signature"), QByteArray("signscope"), QByteArray("both")}) {
                    QByteArray bytes = skeleton;
                    if (field == "both") {
                        bytes = setting(bytes, "signature", QByteArray(1 + room / 2, 'a'));
                        bytes = setting(bytes, "signscope", QByteArray(1 + room - room / 2, 'b'));
                    } else bytes = setting(bytes, field, QByteArray(1 + room, 'x'));
                    if (lf) bytes.replace("\r\n", "\n");
                    if (utf16) bytes = encoded(bytes, QStringConverter::Utf16LE);
                    QCOMPARE(bytes.size(), qsizetype(RdpProfile::maximumBytes));
                    const auto tag = "opaque-raw-budget-" + field + (utf16 ? "-utf16" : "-utf8") + (lf ? "-lf" : "-crlf");
                    QTest::newRow(tag.constData()) << bytes;
                }
            }
        }
    }
    void signaturesSupported() { supportedSubset(); }

    void signaturesRejected_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<QString>("expected");
        const QString invalid = QStringLiteral("The connection file contains an invalid setting value.");
        int rowNumber = 0;
        auto row = [&](const QByteArray& tag, const QByteArray& bytes, const QString& expected = QString{}) {
            const auto unique = QByteArray::number(rowNumber++) + '-' + tag;
            for (const bool lf : {false, true}) {
                QByteArray form = bytes;
                if (lf) form.replace("\r\n", "\n");
                const auto name = unique + (lf ? "-lf" : "-crlf");
                QTest::newRow((name + "-utf8").constData()) << form << expected;
                QTest::newRow((name + "-utf16").constData()) << encoded(form, QStringConverter::Utf16LE) << expected;
            }
        };
        auto badSignature = [&](const QByteArray& tag, const QByteArray& value) {
            row(tag, supportedProfile + signaturePair("Full Address", value), invalid);
        };
        // Removed-policy shape failures are explicitly positive below. Retain
        // every independent empty/padding/non-ASCII/type/pair/control/size guard.
        const auto sig = syntheticSignature("x");
        for (const auto& value : {QByteArray{}, ' ' + sig, sig + ' ', QByteArray::fromHex("c3a9") + sig})
            badSignature("retained-character-" + value.toHex().left(24), value);
        for (const auto& value : {QByteArray{}, QByteArray(" Full Address"), QByteArray("Full Address "),
                 QByteArray("Full Address,AudioMode "), QByteArray("Full Address,") + QByteArray::fromHex("c3a9")})
            row("retained-scope-character-" + value.toHex().left(40), fullerProfile + signaturePair(value), invalid);
        row("mapped-prohibited", setting(fullerProfile, "rdgiskdcproxy", "1", 'i') + signaturePair("Full Address,RDGIsKDCProxy"), invalid);
        const QString missing = QStringLiteral("The connection file is missing a required supported setting.");
        row("lone-signature", supportedProfile + "signature:s:" + sig + "\r\n", missing);
        row("lone-scope", supportedProfile + "signscope:s:Full Address\r\n", missing);
        const auto pair = supportedProfile + signaturePair();
        for (const auto& key : {QByteArray("signature"), QByteArray("signscope")}) {
            for (const auto& type : {"S", "i", "b", "ss", ""}) {
                QByteArray bytes = pair;
                bytes.replace(key + ":s:", key + ':' + type + ':');
                row(key + "-type-" + type, bytes, QString::fromLatin1(qstrlen(type) == 1 ? "The connection file contains an unsupported setting type." : "The connection file contains a malformed setting."));
            }
            for (const auto& type : {"s", "b"})
                row(key + "-duplicate-" + type, pair + key.toUpper() + ':' + type + ":fixture-private-sentinel\r\n", QStringLiteral("The connection file contains a duplicate setting."));
            QByteArray padded = pair;
            padded.replace(key + ':', ' ' + key + ':');
            row(key + "-padded-key", padded, QStringLiteral("The connection file contains an unsupported setting."));
        }
        for (const auto& key : {"password", "gatewayaccesstoken", "unclassifiedmetadata", "SmartRawPrinters"})
            row(QByteArray("unrelated-") + key, pair + key + ":s:fixture-private-sentinel\r\n", QStringLiteral("The connection file contains an unsupported setting."));
        row("folded", supportedProfile + signaturePair("Full Address", sig.left(8) + "\r\n" + sig.mid(8)));
        row("tab", supportedProfile + signaturePair("Full Address", sig + '\t'));
        for (const auto& key : {QByteArray("signature"), QByteArray("signscope")}) {
            for (const auto& value : {QByteArray{}, QByteArray(" x"), QByteArray("x "), QByteArray(" "),
                     QByteArray("x") + QByteArray::fromHex("c3a9"), QByteArray("x") + QByteArray::fromHex("c285")})
                row(key + "-opaque-invalid-" + value.toHex(), setting(pair, key, value));
            for (const auto& hex : {"00", "09", "0b", "0c", "1f", "7f"})
                row(key + "-control-" + hex, setting(pair, key, 'x' + QByteArray::fromHex(hex) + 'x'),
                    QStringLiteral("The connection file contains an unsupported character."));
            row(key + "-empty-duplicate", pair + key.toUpper() + ":s:\r\n", QStringLiteral("The connection file contains a duplicate setting."));
        }
        QTest::newRow("raw-over-utf8") << (pair + QByteArray(RdpProfile::maximumBytes + 1 - pair.size(), '\n')) << QString{};
        QByteArray utf16Over = encoded(pair, QStringConverter::Utf16LE);
        utf16Over += QByteArray::fromHex("0a00").repeated((RdpProfile::maximumBytes - utf16Over.size()) / 2 + 1);
        QTest::newRow("raw-over-utf16") << utf16Over << QString{};
        QTest::newRow("utf8-bom-pair") << (QByteArray::fromHex("efbbbf") + pair) << QString{};
        QTest::newRow("utf16-odd-pair") << (encoded(pair, QStringConverter::Utf16LE) + 'x') << QString{};
        QTest::newRow("invalid-utf8-pair") << (pair + QByteArray::fromHex("e282")) << QString{};
        QTest::newRow("lone-cr-pair") << (pair + '\r') << QString{};
    }
    void signaturesRejected()
    {
        QFETCH(QByteArray, bytes);
        QFETCH(QString, expected);
        QString error;
        QVERIFY(!RdpProfile::validate(bytes, &error));
        if (!expected.isNull()) QCOMPARE(error, expected);
        QVERIFY(!error.contains(QStringLiteral("fixture-private-sentinel")));
        unsupportedSubset();
    }

    void signaturesImportRollback_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<QString>("name");
        for (const auto& value : {QByteArray("0"), QByteArray("1")}) {
            for (const bool lf : {false, true}) {
                QByteArray bytes = fullerProfile + "forcehidpioptimizations:i:" + value + "\r\n" + signaturePair("unknown, repeated, repeated", "fixture-private-sentinel: !");
                if (lf) bytes.replace("\r\n", "\n");
                for (const auto& name : {QString{}, QStringLiteral("Browser PC")}) {
                    const auto tag = "hidpi-" + value + (lf ? "-lf" : "-crlf") + (name.isNull() ? "-manual" : "-acquisition");
                    QTest::newRow((tag + "-utf8").constData()) << bytes << name;
                    QTest::newRow((tag + "-utf16").constData()) << encoded(bytes, QStringConverter::Utf16LE) << name;
                }
            }
        }
    }
    void signaturesImportRollback() { observedGeoImportRollback(); }

    void signatureRenamePreservesBytes()
    {
        const auto bytes = fullerProfile + signaturePair(allSyntheticScopes(true));
        const auto source = temporary->filePath(QStringLiteral("opaque.rdpw"));
        QVERIFY(writeFile(source, bytes));
        ProfileStore store;
        QString error;
        const auto profile = store.importFile(source, &error);
        QVERIFY2(!profile.id.isEmpty(), qPrintable(error));
        QVERIFY(store.renameProfile(profile.id, QStringLiteral("Local name"), &error));
        QCOMPARE(contents(source), bytes);
        QCOMPARE(contents(profile.path), bytes);
        ProfileStore reopened;
        QCOMPARE(reopened.profiles().first().name, QStringLiteral("Local name"));
    }

    void hiDpiSupported_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        auto row = [](const QByteArray& tag, const QByteArray& profile) {
            for (const bool lf : {false, true}) {
                QByteArray bytes = profile;
                if (lf) bytes.replace("\r\n", "\n");
                const auto name = tag + (lf ? "-lf" : "-crlf");
                QTest::newRow((name + "-utf8").constData()) << bytes;
                QTest::newRow((name + "-utf16").constData()) << encoded(bytes, QStringConverter::Utf16LE);
            }
        };
        row("hidpi-absent", supportedProfile);
        for (const auto& value : {QByteArray("0"), QByteArray("1")}) {
            for (const auto& key : {QByteArray("forcehidpioptimizations"), QByteArray("FORCEHIDPIOPTIMIZATIONS"), QByteArray("ForceHiDpiOptimizations")})
                row("hidpi-" + value + '-' + key, supportedProfile + key + ":i:" + value + "\r\n");
            row("combined-" + value, setting(fullerProfile, "enablerdsaadauth", "1", 'i') + "forcehidpioptimizations:i:" + value + "\r\n"
                + signaturePair("unknown label,unknown label", "fixture-private-sentinel: punctuation !"));
        }
    }
    void hiDpiSupported() { supportedSubset(); }

    void hiDpiRejected_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        auto row = [](const QByteArray& name, const QByteArray& profile) {
            for (const bool lf : {false, true}) {
                QByteArray bytes = profile;
                if (lf) bytes.replace("\r\n", "\n");
                const auto tag = name + (lf ? "-lf" : "-crlf");
                QTest::newRow((tag + "-utf8").constData()) << bytes;
                QTest::newRow((tag + "-utf16").constData()) << encoded(bytes, QStringConverter::Utf16LE);
            }
        };
        const QByteArray key = "forcehidpioptimizations";
        for (const auto& value : {QByteArray{}, QByteArray("2"), QByteArray("-1"), QByteArray("+1"), QByteArray("01"), QByteArray("0x1"),
                 QByteArray("1.0"), QByteArray("1x"), QByteArray(" 1"), QByteArray("1 "), QByteArray(" "), QByteArray("1") + QByteArray::fromHex("c3a9")})
            row("hidpi-value-" + value.toHex(), supportedProfile + key + ":i:" + value + "\r\n");
        for (const auto& hex : {"00", "09", "0b", "0c", "7f"})
            row(QByteArray("hidpi-control-") + hex, supportedProfile + key + ":i:1" + QByteArray::fromHex(hex) + "\r\n");
        for (const auto& type : {"I", "s", "b", "ii", ""})
            row(QByteArray("hidpi-type-") + type, supportedProfile + key + ':' + type + ":1\r\n");
        const auto accepted = supportedProfile + key + ":i:1\r\n" + signaturePair("unknown scope", "x: !");
        for (const auto& line : {key + ":i:0", key.toUpper() + ":i:1", key.toUpper() + ":b:fixture-secret-marker", key + ":s:"})
            row("hidpi-duplicate-" + line, accepted + line + "\r\n");
        for (const auto& near : {QByteArray("forcehidpioptimization"), QByteArray("forcehidpioptimizationsx"), ' ' + key, key + ' ',
                 QByteArray("force hidpioptimizations"), QByteArray("forcehidpioptimizat") + QByteArray::fromHex("c4b0") + "ons"})
            row("hidpi-near-name-" + near.toHex(), supportedProfile + near + ":i:1\r\n");
        for (const auto& other : {"unclassifiedmetadata", "password", "gatewayaccesstoken"})
            row(QByteArray("hidpi-independent-") + other, accepted + other + ":s:fixture-secret-marker\r\n");
    }
    void hiDpiRejected() { unsupportedSubset(); }

    void aadAuthSupported_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        for (const auto& value : {QByteArray{}, QByteArray("0"), QByteArray("1")}) {
            const QByteArray token = value.isEmpty() ? QByteArray("absent") : value;
            for (const bool upperKey : {false, true}) {
                if (upperKey && value.isEmpty()) continue;
                const QByteArray key = upperKey ? "ENABLERDSAADAUTH" : "enablerdsaadauth";
                const QByteArray profile = value.isEmpty() ? supportedProfile : supportedProfile + key + ":i:" + value + "\r\n";
                for (const bool lf : {false, true}) {
                    QByteArray bytes = profile;
                    if (lf) bytes.replace("\r\n", "\n");
                    const QByteArray name = "auth-" + token + (upperKey ? "-upper" : "-lower") + (lf ? "-lf" : "-crlf");
                    QTest::newRow((name + "-utf8").constData()) << bytes;
                    QTest::newRow((name + "-utf16").constData()) << encoded(bytes, QStringConverter::Utf16LE);
                }
            }
        }
        for (const bool lf : {false, true}) {
            QByteArray bytes = setting(fullerProfile, "enablerdsaadauth", "1", 'i');
            if (lf) bytes.replace("\r\n", "\n");
            const QByteArray name = lf ? "auth-fuller-one-lf" : "auth-fuller-one-crlf";
            QTest::newRow((name + "-utf8").constData()) << bytes;
            QTest::newRow((name + "-utf16").constData()) << encoded(bytes, QStringConverter::Utf16LE);
        }
    }
    void aadAuthSupported() { supportedSubset(); }

    void aadAuthRejected_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        auto row = [](const QByteArray& name, const QByteArray& bytes) {
            for (const bool lf : {false, true}) {
                QByteArray form = bytes;
                if (lf) form.replace("\r\n", "\n");
                const QByteArray tag = name + (lf ? "-lf" : "-crlf");
                QTest::newRow((tag + "-utf8").constData()) << form;
                QTest::newRow((tag + "-utf16").constData()) << encoded(form, QStringConverter::Utf16LE);
            }
        };
        for (const auto& value : {"", " ", "2", "-0", "-1", "+0", "+1", "00", "01", "0x1", "1.0", "1x", " 1", "1 ", "2147483648"})
            row(QByteArray("auth-value-") + QByteArray(value).toHex(), supportedProfile + "enablerdsaadauth:i:" + value + "\r\n");
        for (const auto& type : {"s", "b", "I", "ii"})
            row(QByteArray("auth-type-") + type, supportedProfile + "enablerdsaadauth:" + type + ":1\r\n");
        const QByteArray one = supportedProfile + "enablerdsaadauth:i:1\r\n";
        for (const auto& line : {"enablerdsaadauth:i:0", "ENABLERDSAADAUTH:i:1", "ENABLERDSAADAUTH:b:fixture-secret-marker"})
            row(QByteArray("auth-duplicate-") + line, one + line + "\r\n");
        for (const auto& key : {"signature", "signscope", "unclassifiedmetadata"})
            row(QByteArray("auth-one-forbidden-") + key, one + key + ":s:fixture-secret-marker\r\n");
    }
    void aadAuthRejected() { unsupportedSubset(); }

    void aadAuthImportRollback_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<QString>("name");
        const QByteArray profile = supportedProfile + "enablerdsaadauth:i:1\r\n";
        for (const bool lf : {false, true}) {
            QByteArray bytes = profile;
            if (lf) bytes.replace("\r\n", "\n");
            const QByteArray tag = lf ? "auth-one-lf" : "auth-one-crlf";
            for (const auto& name : {QString{}, QStringLiteral("Browser PC")}) {
                const QByteArray branch = name.isNull() ? "-manual" : "-acquisition";
                QTest::newRow((tag + branch + "-utf8").constData()) << bytes << name;
                QTest::newRow((tag + branch + "-utf16").constData()) << encoded(bytes, QStringConverter::Utf16LE) << name;
            }
        }
    }
    void aadAuthImportRollback() { observedGeoImportRollback(); }

    void observedGeoImports_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<QString>("name");
        const QByteArray bytes = supportedProfile + "geo:s:AU__False\r\n";
        QTest::newRow("manual-utf8") << bytes << QString{};
        QTest::newRow("browser-utf8") << bytes << QStringLiteral("Browser PC");
        QTest::newRow("manual-utf16le") << encoded(bytes, QStringConverter::Utf16LE) << QString{};
        QTest::newRow("browser-utf16le") << encoded(bytes, QStringConverter::Utf16LE) << QStringLiteral("Browser PC");
    }

    void observedGeoImports()
    {
        QFETCH(QByteArray, bytes);
        QFETCH(QString, name);
        const QString source = temporary->filePath(QStringLiteral("Synthetic PC.rdpw"));
        QVERIFY(writeFile(source, bytes));
        ProfileStore store;
        QString error;
        QSignalSpy changes(&store, &ProfileStore::changed);
        const auto profile = store.importFile(source, &error, name);
        QVERIFY2(!profile.id.isEmpty(), qPrintable(error));
        QVERIFY(error.isEmpty());
        QCOMPARE(changes.count(), 1);
        QCOMPARE(profile.name, name.isNull() ? QStringLiteral("Synthetic PC") : name);
        QCOMPARE(contents(source), bytes);
        QCOMPARE(contents(profile.path), bytes);
        QByteArray captured;
        QVERIFY2(RdpProfile::readAndValidate(profile.path, &captured, &error), qPrintable(error));
        QCOMPARE(captured, bytes);
        struct stat st {};
        QCOMPARE(::stat(QFile::encodeName(profile.path).constData(), &st), 0);
        QCOMPARE(st.st_mode & 0777, mode_t(0600));
        ProfileStore reopened;
        QCOMPARE(reopened.profiles().size(), 1);
        QCOMPARE(reopened.profiles().first().id, profile.id);
        QCOMPARE(reopened.profiles().first().name, profile.name);
        QCOMPARE(reopened.profiles().first().path, profile.path);
    }

    void observedGeoImportRollback_data() { observedGeoImports_data(); }

    void observedGeoImportRollback()
    {
        QFETCH(QByteArray, bytes);
        QFETCH(QString, name);
        QString error;
        QVERIFY2(RdpProfile::validate(bytes, &error), qPrintable(error));
        const QString good = temporary->filePath(QStringLiteral("good.rdpw"));
        const QString source = temporary->filePath(QStringLiteral("download.rdpw"));
        QVERIFY(writeFile(good, supportedProfile));
        QVERIFY(writeFile(source, bytes));
        ProfileStore store;
        const auto existing = store.importFile(good, &error);
        QVERIFY2(!existing.id.isEmpty(), qPrintable(error));
        const QDir root(QFileInfo(existing.path).absolutePath());
        const QString index = root.filePath(QStringLiteral("profiles.json"));
        const auto before = contents(index);
        const auto files = root.entryList(QDir::Files | QDir::Hidden);
        // Validation and the owned-copy write succeed; index commit must fail.
        QVERIFY(::chmod(QFile::encodeName(index).constData(), 0644) == 0);
        QSignalSpy changes(&store, &ProfileStore::changed);
        QVERIFY(store.importFile(source, &error, name).id.isEmpty());
        QVERIFY(error.contains(QStringLiteral("unsafe permissions or ownership")));
        QCOMPARE(changes.count(), 0);
        QCOMPARE(store.profiles().size(), 1);
        QCOMPARE(store.profiles().first().id, existing.id);
        QCOMPARE(root.entryList(QDir::Files | QDir::Hidden), files);
        QCOMPARE(contents(index), before);
        QCOMPARE(contents(existing.path), supportedProfile);
        QCOMPARE(contents(source), bytes);
        QVERIFY(::chmod(QFile::encodeName(index).constData(), 0600) == 0);
        ProfileStore reopened;
        QCOMPARE(reopened.profiles().size(), 1);
        QCOMPARE(reopened.profiles().first().id, existing.id);
    }

    void regionalMetadataImports_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<QString>("name");
        const QList<QPair<QByteArray, QByteArray>> fixtures{
            {"geo", supportedProfile + "geo:s:fixture_secret_marker\r\n"},
            {"diagnostic", supportedProfile + "diagnosticserviceurl:s:" + metadataUrl("diagnosticserviceurl") + "\r\n"},
            {"hub", supportedProfile + "hubdiscoverygeourl:s:" + metadataUrl("hubdiscoverygeourl", "rdweb-g-usg-r1.wvd.azure.us") + "\r\n"},
            {"activity", supportedProfile + "activityhint:s:" + activityValue() + "\r\n"},
            {"combined", regionalProfile()}};
        for (const auto& fixture : fixtures) {
            QByteArray lf = fixture.second;
            lf.replace("\r\n", "\n");
            const QList<QPair<QByteArray, QByteArray>> forms{{"utf8-crlf", fixture.second}, {"utf8-lf", lf},
                {"utf16-crlf", encoded(fixture.second, QStringConverter::Utf16LE)}, {"utf16-lf", encoded(lf, QStringConverter::Utf16LE)}};
            for (const auto& form : forms) {
                QTest::newRow((fixture.first + '-' + form.first + "-manual").constData()) << form.second << QString{};
                QTest::newRow((fixture.first + '-' + form.first + "-acquisition").constData()) << form.second << QStringLiteral("Browser PC");
            }
        }
    }
    void regionalMetadataImports() { observedGeoImports(); }
    void regionalMetadataImportRollback_data() { regionalMetadataImports_data(); }
    void regionalMetadataImportRollback() { observedGeoImportRollback(); }

    void unsupportedSubset_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        auto row = [](const QByteArray& name, const QByteArray& bytes) { QTest::newRow(name.constData()) << bytes; };
        const QByteArray all = supportedProfile + "aadtenantid:s:abcdefab-1234-5678-9abc-def012345678\r\nresourceprovider:s:arm\r\nredirectwebauthn:i:1\r\n";
        for (auto line : all.split('\n')) {
            if (line.isEmpty()) continue;
            line.chop(1);
            const auto key = line.left(line.indexOf(':'));
            row("duplicate-" + key, all + line + "\n");
            row("case-duplicate-" + key, all + key.toUpper() + line.mid(key.size()) + "\n");
            row("different-type-duplicate-" + key, all + key + ":b:fixture-secret-marker\n");
            row("empty-" + key, setting(all, key, "", key == "redirectwebauthn" ? 'i' : 's'));
            row("binary-" + key, setting(all, key, "fixture-secret-marker", 'b'));
            row("wrong-type-" + key, setting(all, key, "1", key == "redirectwebauthn" ? 's' : 'i'));
            row("uppercase-type-" + key, setting(all, key, "1", 'S'));
            const QByteArray value = line.mid(key.size() + 3);
            const char type = key == "redirectwebauthn" ? 'i' : 's';
            row("leading-value-space-" + key, setting(all, key, ' ' + value, type));
            row("trailing-value-space-" + key, setting(all, key, value + ' ', type));
            QByteArray padded = all;
            padded.replace(key + ':', " " + key + ':');
            row("padded-key-" + key, padded);
            padded = all;
            padded.replace(key + ':', key + " :");
            row("trailing-key-space-" + key, padded);
            if (supportedProfile.contains(key + ':')) {
                QByteArray missing = all;
                missing.remove(missing.indexOf(key + ':'), line.size() + 2);
                row("missing-" + key, missing);
            }
        }
        for (const auto& key : {"password", "password 51", "gatewayaccesstoken", "endpointfedauth", "pcb", "username", "domain", "signature", "signscope", "redirectdrives", "redirectsmartcards", "screen mode id", "fixture-secret-marker"}) {
            row(QByteArray("excluded-") + key, supportedProfile + key + ":s:fixture-secret-marker\n");
            row(QByteArray("excluded-empty-") + key, supportedProfile + key + ":s:\n");
        }
        for (const auto& value : {"0", "2", "01", "+1", "-1", "0x1", "1.0", "1x", " 1", "1 ", "2147483648", "9999999999999999999999999"})
            row(QByteArray("integer-") + value, supportedProfile + "redirectwebauthn:i:" + value + "\n");
        for (const auto& key : {QByteArray("full address"), QByteArray("gatewayhostname")}) {
            for (const auto& value : {".example.test", "example.test.", "a..test", "-a.test", "a-.test", "https://example.test", "user@example.test", "example.test/path", "example.test?token=fixture-secret-marker", "example.test#fixture-secret-marker", "127.0.0.1", "[::1]", "example.test:444", "example.test:0443", "example.test:443:443", "example.test:443/path", "a_b.test",
                     // libc numeric-host forms: getaddrinfo resolves these as IPv4 literals, not DNS names.
                     "2130706433", "0x7f000001", "127.1", "0x7f.0.0.1", "1.2.3", "0X7F.1"})
                row(key + '-' + value, setting(supportedProfile, key, value));
            row(key + "-label-64", setting(supportedProfile, key, QByteArray(64, 'a') + ".test"));
            row(key + "-total-254", setting(supportedProfile, key, QByteArray(63, 'a') + '.' + QByteArray(63, 'b') + '.' + QByteArray(63, 'c') + '.' + QByteArray(62, 'd')));
            row(key + "-unicode", setting(supportedProfile, key, QByteArray::fromHex("c3a9") + ".test"));
        }
        row("address-port-443", setting(supportedProfile, "full address", "example.test:443"));
        for (const auto& pair : QList<QPair<QByteArray, QByteArray>>{{"fixture_group", ""}, {"fixture_group", QByteArray(91, 'g')}, {"fixture_group", "group.name"}, {"fixture-pool", ""}, {"fixture-pool", QByteArray(65, 'h')}, {"fixture-pool", "pool/name"}, {"/subscriptions/", "https://example.test/subscriptions/"}, {"/resourcegroups/", "/resourcegroup/"}, {"Microsoft.DesktopVirtualization", "Microsoft.Other"}, {"11111111-2222-3333-4444-555555555555", "not-a-uuid"}}) {
            QByteArray bytes = supportedProfile;
            bytes.replace(pair.first, pair.second);
            row("arm-" + pair.first + '-' + pair.second, bytes);
        }
        QByteArray arm = supportedProfile.split('\n').at(2).trimmed().mid(10);
        QByteArray unicodeArm = supportedProfile;
        unicodeArm.replace("Microsoft", QByteArray("Micro") + QByteArray::fromHex("c5bf") + "oft");
        row("arm-unicode-case-equivalent", unicodeArm);
        row("arm-suffix", setting(supportedProfile, "armpath", arm + "/extra"));
        row("arm-percent", setting(supportedProfile, "armpath", arm.replace("fixture_group", "%66ixture_group")));
        for (const auto& value : {"mth://localhost/a/b", "MTH://localhost/aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee/01234567-89ab-cdef-0123-456789abcdef", "mth://remote/aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee/01234567-89ab-cdef-0123-456789abcdef", "mth://localhost/aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee/01234567-89ab-cdef-0123-456789abcdef/"})
            row(QByteArray("routing-") + value, setting(supportedProfile, "loadbalanceinfo", value));
        for (const auto& key : {QByteArray("remoteapplicationprogram"), QByteArray("aadtenantid")}) {
            for (const auto& value : {"fixture-secret-marker", "{99999999-8888-7777-6666-555555555555}", "99999999888877776666555555555555", "99999999-8888-7777-6666-55555555555g", "||99999999-8888-7777-6666-555555555555/"})
                row(key + '-' + value, key == "aadtenantid" ? supportedProfile + key + ":s:" + value + "\n" : setting(supportedProfile, key, value));
        }
        row("program-unprefixed-uuid", setting(supportedProfile, "remoteapplicationprogram", "99999999-8888-7777-6666-555555555555"));
        row("tenant-prefixed-uuid", supportedProfile + "aadtenantid:s:||99999999-8888-7777-6666-555555555555\n");
        row("uuid-extra-hex", supportedProfile + "aadtenantid:s:abcdefab-1234-5678-9abc-def0123456789\n");
        row("uuid-short-hex", supportedProfile + "aadtenantid:s:abcdefab-1234-5678-9abc-def01234567\n");
        row("unicode-key-case", supportedProfile + QByteArray::fromHex("c4b0") + "oadbalanceinfo:s:fixture-secret-marker\n");
        row("provider-case", supportedProfile + "resourceprovider:s:ARM\n");
        row("provider-suffix", supportedProfile + "resourceprovider:s:arm:fixture-secret-marker\n");
        for (const auto& line : {" ", "\t", "# comment", "/cert:ignore", "fixture-secret-marker", "full address:ss:example.test", "full address:s", ":s:example.test"})
            row(QByteArray("malformed-") + line, supportedProfile + line + "\n");
        row("empty-file", {});
        row("utf8-bom", QByteArray::fromHex("efbbbf") + supportedProfile);
        row("utf16be", encoded(supportedProfile, QStringConverter::Utf16BE));
        row("bomless-utf16le", encoded(supportedProfile, QStringConverter::Utf16LE, false));
        row("bomless-utf16be", encoded(supportedProfile, QStringConverter::Utf16BE, false));
        row("utf32le", encoded(supportedProfile, QStringConverter::Utf32LE));
        row("utf32be", encoded(supportedProfile, QStringConverter::Utf32BE));
        row("utf16-odd", encoded(supportedProfile, QStringConverter::Utf16LE) + 'a');
        row("utf16-high-surrogate", encoded(supportedProfile, QStringConverter::Utf16LE) + QByteArray::fromHex("00d8"));
        row("utf16-low-surrogate", encoded(supportedProfile, QStringConverter::Utf16LE) + QByteArray::fromHex("00dc"));
        row("utf16-second-bom", QByteArray::fromHex("fffe") + encoded(supportedProfile, QStringConverter::Utf16LE));
        for (const auto& hex : {"00", "09", "0b", "0c", "7f", "c285", "e280a8", "e280a9", "efbbbf", "ff", "c080", "eda080", "f4908080", "e282"})
            row(QByteArray("character-") + hex, supportedProfile + QByteArray::fromHex(hex));
        row("bare-cr", supportedProfile + "\r");
        row("cr-within-value", setting(supportedProfile, "full address", "example\r.test"));
        row("over-byte-limit", supportedProfile + QByteArray(RdpProfile::maximumBytes + 1 - supportedProfile.size(), '\n'));
        // Each new key is independently positive above; exercise its complete
        // type/value/serialization boundaries here through pure and both public imports.
        for (auto line : euOptionalSettings.split('\n')) {
            if (line.isEmpty()) continue;
            line.chop(1);
            const auto colon = line.indexOf(':');
            const auto key = line.left(colon);
            const char type = line.at(colon + 1);
            const auto value = line.mid(colon + 3);
            auto invalid = [&](const QByteArray& name, const QByteArray& replacement, char replacementType) {
                row("eu-" + key + '-' + name, setting(fullerProfile, key, replacement, replacementType));
            };
            row("eu-duplicate-" + key, fullerProfile + line + "\n");
            row("eu-case-duplicate-" + key, fullerProfile + key.toUpper() + line.mid(key.size()) + "\n");
            row("eu-type-duplicate-" + key, fullerProfile + key.toUpper() + ":b:fixture-secret-marker\n");
            invalid("empty", "", type);
            invalid("wrong-type", value, type == 'i' ? 's' : 'i');
            invalid("binary", value, 'b');
            invalid("uppercase-type", value, type == 'i' ? 'I' : 'S');
            invalid("secret-suffix", value + ":fixture-secret-marker", type);
            invalid("direct-secret-suffix", value + "fixture-secret-marker", type);
            invalid("leading-space", ' ' + value, type);
            invalid("trailing-space", value + ' ', type);
            invalid("control", value + '\t', type);
            invalid("unicode", value + QByteArray::fromHex("c3a9"), type);
            invalid("malformed-utf8", value + QByteArray::fromHex("e282"), type);
            invalid("standalone-cr", value + '\r' + "fixture-secret-marker", type);
            QByteArray padded = fullerProfile;
            padded.replace(key + ':', ' ' + key + ':');
            row("eu-padded-key-" + key, padded);
            padded = fullerProfile;
            padded.replace(key + ':', key + " :");
            row("eu-trailing-key-padding-" + key, padded);
            padded = fullerProfile;
            padded.replace(key + ':' + type + ':', key + ":ss:");
            row("eu-multichar-type-" + key, padded);
            if (type == 'i') {
                for (const auto& number : {"2", "9", "00", "01", "+0", "+1", "-1", "0x0", "0x1", "1.0", "1x", "2147483647", "2147483648", "9999999999999999999999999"})
                    invalid(QByteArray("integer-") + number, number, type);
                if (key != "enablerdsaadauth") // Only this explicitly approved rule accepts both literals.
                    invalid("opposite-literal", value == "0" ? "1" : "0", type);
            } else if (value == "*") {
                for (const auto& devices : {"**", "a", "*:fixture-secret-marker", "/tmp/device", "C:\\device", "channel", "*,other", "*;other"})
                    invalid(QByteArray("device-") + devices, devices, type);
            }
        }
        for (const auto& key : {"wvd endpoint pool", "workspace id"}) {
            for (const auto& uuid : {"fixture-secret-marker", "{abcdefab-1234-5678-9abc-def012345678}", "abcdefab123456789abcdef012345678", "abcdefab-1234-5678-9abc-def01234567", "abcdefab-1234-5678-9abc-def0123456789", "abcdefab-1234-5678-9abc-def01234567g"})
                row(QByteArray("eu-uuid-") + key + '-' + uuid, setting(fullerProfile, key, uuid));
        }
        for (const auto& geo : {"EU?token=fixture-secret-marker"})
            row(QByteArray("geo-") + geo, setting(fullerProfile, "geo", geo));
        auto geoRejected = [&](const QByteArray& name, const QByteArray& line) {
            const QByteArray bytes = supportedProfile + line + "\r\n";
            row("geo-observed-" + name, bytes);
            row("geo-observed-utf16-" + name, encoded(bytes, QStringConverter::Utf16LE));
        };
        // Token neighbours now inside T are covered positively in regionalRows.
        for (const auto& geo : {" AU__False", "AU__False ", "AU%5f%5fFalse",
                 "AU__False:fixture-secret-marker", "AU__Falsefixture-secret-marker", ""})
            geoRejected(QByteArray("value-") + geo, QByteArray("geo:s:") + geo);
        for (const auto& line : {"geo:i:AU__False", "geo:b:AU__False", "geo:S:AU__False", "geo:ss:AU__False",
                 "geo:s", "geo:s:AU__False\r\nGEO:s:EU", "geo:s:AU__False\r\ngeo:s:AU__False",
                 "geo:s:AU__False\r\nGEO:b:fixture-secret-marker", "geo:s:AU__False\t"})
            geoRejected(QByteArray("serialization-") + line, line);
        geoRejected("non-ascii", QByteArray("geo:s:AU__False") + QByteArray::fromHex("c3a9"));
        for (const auto& address : {"other.example.test", "cloudpc.example.test:443", "127.0.0.1", "cloudpc.example.test.", "-cloudpc.example.test", "cloudpc..test", "cloudpc_example.test", "https://cloudpc.example.test"})
            row(QByteArray("alternate-") + address, setting(fullerProfile, "alternate full address", address));
        row("alternate-full-changed", setting(fullerProfile, "full address", "other.example.test"));
        row("alternate-label-64", setting(fullerProfile, "alternate full address", QByteArray(64, 'a')));
        row("alternate-total-254", setting(fullerProfile, "alternate full address", QByteArray(63, 'a') + '.' + QByteArray(63, 'b') + '.' + QByteArray(63, 'c') + '.' + QByteArray(62, 'd')));
        for (const auto& key : {QByteArray("diagnosticserviceurl"), QByteArray("hubdiscoverygeourl")}) {
            const auto start = fullerProfile.indexOf(key + ":s:") + key.size() + 3;
            const auto url = fullerProfile.mid(start, fullerProfile.indexOf("\r\n", start) - start);
            for (const auto& pair : QList<QPair<QByteArray, QByteArray>>{{"https://", "http://"}, {"https://", "HTTPS://"}, {".com/", ".com:443/"}, {".com/", ".COM/"}, {"https://", "https://user@"}, {"/api/arm/", "/API/arm/"}, {"/api/arm/", "/api/%61rm/"}}) {
                QByteArray changed = url;
                changed.replace(pair.first, pair.second);
                row("eu-url-" + key + '-' + pair.second, setting(fullerProfile, key, changed));
            }
            for (const auto& suffix : {"/", "?token=fixture-secret-marker", "#fixture-secret-marker", "&extra=1"})
                row("eu-url-suffix-" + key + suffix, setting(fullerProfile, key, url + suffix));
        }
        QByteArray hubKeyCase = fullerProfile;
        hubKeyCase.replace("?resourceId=", "?resourceid=");
        row("hub-query-key-case", hubKeyCase);
        for (const auto& uuid : {"", "not-a-uuid", "{44444444-5555-6666-7777-888888888888}", "44444444-5555-6666-7777-88888888888g", "44444444-5555-6666-7777-8888888888889"})
            row(QByteArray("hub-uuid-") + uuid, setting(fullerProfile, "hubdiscoverygeourl",
                QByteArray("https://rdweb-g-eu-r1.wvd.microsoft.com/api/arm/hubdiscovery?resourceId=") + uuid));
        for (int component = 0; component < 3; ++component) {
            for (const auto& number : {"0", "00", "01", "+1", "-1", "10000", "1.0", "1x", "99999999999999999"}) {
                QList<QByteArray> sizes{"2", "8", "128"};
                sizes[component] = number;
                row("desktop-number-" + QByteArray::number(component) + '-' + number, setting(fullerProfile, "remotedesktopname",
                    "Cloud PC Enterprise " + sizes[0] + "vCPU/" + sizes[1] + "GB/" + sizes[2] + "GB"));
            }
        }
        for (const auto& label : {"Cloud PC Enterprise 2VCPU/8GB/128GB", "Cloud PC Enterprise 2vCPU/8Gb/128GB", "Cloud PC Enterprise 2vCPU/8GB/128GB/1", "Cloud PC Enterprise 2vCPU /8GB/128GB"})
            row(QByteArray("desktop-syntax-") + label, setting(fullerProfile, "remotedesktopname", label));
        for (const auto& label : {"", " Example", "Example\"x", "Example:x", QByteArray(65, 'a').constData()})
            row(QByteArray("desktop-name-rejected-") + label, setting(fullerProfile, "remotedesktopname", label));
        const QByteArray activityKey = "activityhint:s:";
        const auto activityStart = fullerProfile.indexOf(activityKey) + activityKey.size();
        const auto activity = fullerProfile.mid(activityStart, fullerProfile.indexOf("\r\n", activityStart) - activityStart);
        for (const auto& pair : QList<QPair<QByteArray, QByteArray>>{
                 {"ms-wvd-ep:", "MS-WVD-EP:"}, {"55555555-6666-7777-8888-999999999999", "not-a-uuid"},
                 {"55555555-6666-7777-8888-999999999999", "{55555555-6666-7777-8888-999999999999}"},
                 {"%3a", "%3A"}, {"%2c", "%2C"}, {"%3a", ":"}, {"%2c", ","}, {"{", "%7b"},
                 {"\"Geo\"", "\"geo\""},
                 {"Ring\"%3a3", "Ring\"%3a10"}, {"Ring\"%3a3", "Ring\"%3a03"}, {"Ring\"%3a3", "Ring\"%3a-1"},
                 {"Ring\"%3a3", "Ring\"%3a+1"}, {"Ring\"%3a3", "Ring\"%3a1.0"}, {"Ring\"%3a3", "Ring\"%3a"},
                 {"ScaleUnit\"%3a123", "ScaleUnit\"%3a0"}, {"ScaleUnit\"%3a123", "ScaleUnit\"%3a0123"},
                 {"ScaleUnit\"%3a123", "ScaleUnit\"%3a10000"}, {"ScaleUnit\"%3a123", "ScaleUnit\"%3a+1"},
                 {"ScaleUnit\"%3a123", "ScaleUnit\"%3a1.0"}, {"ScaleUnit\"%3a123", "ScaleUnit\"%3a-1"},
                 {"ScaleUnit\"%3a123", "ScaleUnit\"%3a"}, {"ScaleUnitPath=", "ScaleUnitPath= "},
                 {"\"Geo\"%3a\"EU\"%2c\"Ring\"%3a3", "\"Ring\"%3a3%2c\"Geo\"%3a\"EU\""},
                 {"}", "%2c\"Extra\"%3a1}"}, {"}", "}&token=fixture-secret-marker"}, {"}", "}#fragment"}}) {
            QByteArray changed = activity;
            changed.replace(pair.first, pair.second);
            row("activity-syntax-" + pair.first + '-' + pair.second, setting(fullerProfile, "activityhint", changed));
        }
        row("fuller-enabled-kdc-proxy", setting(fullerProfile, "rdgiskdcproxy", "1", 'i'));
        row("fuller-unclassified-metadata", fullerProfile + "unclassifiedmetadata:s:fixture-secret-marker\n");
        row("fuller-utf8-bom", QByteArray::fromHex("efbbbf") + fullerProfile);
        row("fuller-utf16be", encoded(fullerProfile, QStringConverter::Utf16BE));
        row("fuller-utf16-odd", encoded(fullerProfile, QStringConverter::Utf16LE) + 'x');
        row("fuller-utf16-duplicate", encoded(fullerProfile + "GEO:s:EU\n", QStringConverter::Utf16LE));
        row("fuller-utf16-secret-suffix", encoded(setting(fullerProfile, "activityhint", activity + ":fixture-secret-marker"), QStringConverter::Utf16LE));
        regionalRows(false);
    }

    void unsupportedSubset()
    {
        QFETCH(QByteArray, bytes);
        QString pureError;
        QVERIFY(!RdpProfile::validate(bytes, &pureError));
        const QSet<QString> safeErrors{
            QStringLiteral("The connection file must be readable and no larger than 1 MiB."),
            QStringLiteral("The connection file has an unsupported or invalid encoding."),
            QStringLiteral("The connection file contains an invalid line separator."),
            QStringLiteral("The connection file contains an unsupported character."),
            QStringLiteral("The connection file contains a malformed setting."),
            QStringLiteral("The connection file contains an unsupported setting."),
            QStringLiteral("The connection file contains a duplicate setting."),
            QStringLiteral("The connection file contains an unsupported setting type."),
            QStringLiteral("The connection file contains an invalid setting value."),
            QStringLiteral("The connection file is missing a required supported setting.")};
        QVERIFY2(safeErrors.contains(pureError), qPrintable(pureError));
        const QString source = temporary->filePath(QStringLiteral("bad.rdpw"));
        const QString good = temporary->filePath(QStringLiteral("good.rdpw"));
        QVERIFY(writeFile(good, supportedProfile));
        ProfileStore store;
        QString error;
        const auto existing = store.importFile(good, &error);
        QVERIFY2(!existing.id.isEmpty(), qPrintable(error));
        const QDir root(QFileInfo(existing.path).absolutePath());
        const auto files = root.entryList(QDir::Files | QDir::Hidden);
        const auto index = contents(root.filePath(QStringLiteral("profiles.json")));
        QVERIFY(writeFile(source, bytes));
        QSignalSpy changes(&store, &ProfileStore::changed);
        // Both manual import (no label) and browser acquisition (explicit label).
        for (const auto& name : {QString{}, QStringLiteral("Browser PC")}) {
            QVERIFY(store.importFile(source, &error, name).id.isEmpty());
            QCOMPARE(error, pureError);
            QVERIFY(!error.contains(QStringLiteral("fixture-secret-marker")));
            QCOMPARE(changes.count(), 0);
            QCOMPARE(store.profiles().size(), 1);
            QCOMPARE(root.entryList(QDir::Files | QDir::Hidden), files);
            QCOMPARE(contents(root.filePath(QStringLiteral("profiles.json"))), index);
            QCOMPARE(contents(existing.path), supportedProfile);
            QCOMPARE(contents(source), bytes);
        }
    }

    void removalDeletesOnlyOwnedCopy()
    {
        const QString source = temporary->filePath(QStringLiteral("original.rdpw"));
        QVERIFY(writeFile(source, validProfile));
        ProfileStore store;
        QString error;
        const auto profile = store.importFile(source, &error);
        QVERIFY2(!profile.id.isEmpty(), qPrintable(error));
        QVERIFY(store.removeProfile(profile.id, &error));
        QVERIFY(!QFileInfo::exists(profile.path));
        QCOMPARE(contents(source), validProfile);
        ProfileStore reopened;
        QCOMPARE(reopened.profiles().size(), 0);
    }

    void refusesSymlinkReplacement()
    {
        const QString source = temporary->filePath(QStringLiteral("original.rdpw"));
        QVERIFY(writeFile(source, validProfile));
        ProfileStore store;
        QString error;
        const auto profile = store.importFile(source, &error);
        QVERIFY2(!profile.id.isEmpty(), qPrintable(error));
        QVERIFY(QFile::remove(profile.path));
        QVERIFY(QFile::link(source, profile.path));
        QVERIFY(!store.removeProfile(profile.id, &error));
        QCOMPARE(contents(source), validProfile);
        QCOMPARE(store.profiles().size(), 1);
    }

    void refusesMetadataTraversalWithoutOverwritingIndex()
    {
        ProfileStore initial;
        const QString index = temporary->filePath(QStringLiteral("omawin365/profiles.json"));
        const QByteArray malicious = QJsonDocument(QJsonArray{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("../outside")},
                        {QStringLiteral("name"), QStringLiteral("Cloud PC")}}
        }).toJson();
        QVERIFY(writeFile(index, malicious));
        const QString source = temporary->filePath(QStringLiteral("valid.rdpw"));
        QVERIFY(writeFile(source, validProfile));
        ProfileStore reopened;
        QString error;
        QVERIFY(reopened.importFile(source, &error).id.isEmpty());
        QCOMPARE(contents(index), malicious);
    }

    void refusesWorldReadableStorage()
    {
        ProfileStore initial;
        const QString root = temporary->filePath(QStringLiteral("omawin365"));
        QVERIFY(::chmod(QFile::encodeName(root).constData(), 0755) == 0);
        const QString source = temporary->filePath(QStringLiteral("valid.rdpw"));
        QVERIFY(writeFile(source, validProfile));
        ProfileStore reopened;
        QString error;
        QVERIFY(reopened.importFile(source, &error).id.isEmpty());
        QVERIFY(error.contains(QStringLiteral("0700")));
    }

    void staleStoreCannotOverwriteNewerIndex()
    {
        // Instances with different runtime locks can still share XDG_DATA_HOME.
        const QString source = temporary->filePath(QStringLiteral("Cloud PC.rdpw"));
        QVERIFY(writeFile(source, validProfile));
        ProfileStore first;
        ProfileStore second;
        QString error;
        const auto kept = first.importFile(source, &error);
        QVERIFY2(!kept.id.isEmpty(), qPrintable(error));
        const QDir root(QFileInfo(kept.path).absolutePath());
        const QString index = root.filePath(QStringLiteral("profiles.json"));
        const auto files = root.entryList(QDir::Files | QDir::Hidden);
        const auto saved = contents(index);
        QVERIFY(second.importFile(source, &error).id.isEmpty());
        QVERIFY(!error.isEmpty());
        QCOMPARE(root.entryList(QDir::Files | QDir::Hidden), files);
        QCOMPARE(contents(index), saved);
        ProfileStore third;
        QCOMPARE(third.profiles().size(), 1);
        QCOMPARE(third.profiles().first().id, kept.id);
        // A stale import must not restore an entry whose file another instance
        // removed; that index would make every later start fail to load.
        QVERIFY2(first.removeProfile(kept.id, &error), qPrintable(error));
        QVERIFY(third.importFile(source, &error).id.isEmpty());
        QVERIFY(!error.isEmpty());
        ProfileStore reopened;
        QVERIFY(reopened.profiles().isEmpty());
        QVERIFY2(!reopened.importFile(source, &error).id.isEmpty(), qPrintable(error));
    }

    void fifoCannotBlockProfileRead()
    {
        // Import and launch check the pathname first; a FIFO swapped in later must not block open().
        const QString fifo = temporary->filePath(QStringLiteral("swapped.rdpw"));
        QVERIFY(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0);
        auto read = std::async(std::launch::async, [fifo] {
            QString error;
            return RdpProfile::readAndValidate(fifo, nullptr, &error) || error.isEmpty();
        });
        const bool returned = read.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        if (!returned) {
            // Release the blocked reader so the failure is reported instead of hanging.
            const int writer = ::open(QFile::encodeName(fifo).constData(), O_WRONLY | O_NONBLOCK);
            if (writer >= 0)
                ::close(writer);
        }
        QVERIFY2(returned, "Opening a FIFO blocked the caller.");
        QVERIFY(!read.get());
    }

private:
    std::unique_ptr<QTemporaryDir> temporary;
    QByteArray oldDataHome;
};

QTEST_GUILESS_MAIN(ProfileTests)
#include "tst_profiles.moc"
