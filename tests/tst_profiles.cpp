#include "synthetic_profile.h"
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
        const auto profile = store.importFile(source, &error, QStringLiteral("Browser acquired PC"));
        QVERIFY2(!profile.id.isEmpty(), qPrintable(error));
        QCOMPARE(changes.count(), 1);
        QCOMPARE(contents(source), bytes);
        QCOMPARE(contents(profile.path), bytes);
        struct stat st {};
        QCOMPARE(::stat(QFile::encodeName(profile.path).constData(), &st), 0);
        QCOMPARE(st.st_mode & 0777, mode_t(0600));
    }

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
            for (const auto& value : {".example.test", "example.test.", "a..test", "-a.test", "a-.test", "https://example.test", "user@example.test", "example.test/path", "example.test?token=fixture-secret-marker", "example.test#fixture-secret-marker", "127.0.0.1", "[::1]", "example.test:444", "example.test:0443", "example.test:443:443", "example.test:443/path", "a_b.test"})
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
        for (const auto& geo : {"eu", "US", "EU?token=fixture-secret-marker", "EUX"})
            row(QByteArray("geo-") + geo, setting(fullerProfile, "geo", geo));
        for (const auto& address : {"other.example.test", "cloudpc.example.test:443", "127.0.0.1", "cloudpc.example.test.", "-cloudpc.example.test", "cloudpc..test", "cloudpc_example.test", "https://cloudpc.example.test"})
            row(QByteArray("alternate-") + address, setting(fullerProfile, "alternate full address", address));
        row("alternate-full-changed", setting(fullerProfile, "full address", "other.example.test"));
        row("alternate-label-64", setting(fullerProfile, "alternate full address", QByteArray(64, 'a')));
        row("alternate-total-254", setting(fullerProfile, "alternate full address", QByteArray(63, 'a') + '.' + QByteArray(63, 'b') + '.' + QByteArray(63, 'c') + '.' + QByteArray(62, 'd')));
        for (const auto& key : {QByteArray("diagnosticserviceurl"), QByteArray("hubdiscoverygeourl")}) {
            const auto start = fullerProfile.indexOf(key + ":s:") + key.size() + 3;
            const auto url = fullerProfile.mid(start, fullerProfile.indexOf("\r\n", start) - start);
            for (const auto& pair : QList<QPair<QByteArray, QByteArray>>{{"https://", "http://"}, {"https://", "HTTPS://"}, {"rdweb-g-eu-r1", "rdweb-g-us-r1"}, {".com/", ".com:443/"}, {".com/", ".COM/"}, {"https://", "https://user@"}, {"/api/arm/", "/API/arm/"}, {"/api/arm/", "/api/%61rm/"}}) {
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
        for (const auto& label : {"Cloud PC Enterprise", "Cloud PC Enterprise 2VCPU/8GB/128GB", "Cloud PC Enterprise 2vCPU/8Gb/128GB", "Cloud PC Enterprise 2vCPU/8GB/128GB/1", "Cloud PC Enterprise 2vCPU /8GB/128GB", "Other Enterprise 2vCPU/8GB/128GB", "Cloud PC Enterprise fixture-secret-marker"})
            row(QByteArray("desktop-syntax-") + label, setting(fullerProfile, "remotedesktopname", label));
        const QByteArray activityKey = "activityhint:s:";
        const auto activityStart = fullerProfile.indexOf(activityKey) + activityKey.size();
        const auto activity = fullerProfile.mid(activityStart, fullerProfile.indexOf("\r\n", activityStart) - activityStart);
        for (const auto& pair : QList<QPair<QByteArray, QByteArray>>{
                 {"ms-wvd-ep:", "MS-WVD-EP:"}, {"55555555-6666-7777-8888-999999999999", "not-a-uuid"},
                 {"55555555-6666-7777-8888-999999999999", "{55555555-6666-7777-8888-999999999999}"},
                 {"%3a", "%3A"}, {"%2c", "%2C"}, {"%3a", ":"}, {"%2c", ","}, {"{", "%7b"},
                 {"\"Geo\"", "\"geo\""}, {"\"EU\"", "\"US\""}, {"westeurope", "northeurope"},
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

private:
    std::unique_ptr<QTemporaryDir> temporary;
    QByteArray oldDataHome;
};

QTEST_GUILESS_MAIN(ProfileTests)
#include "tst_profiles.moc"
