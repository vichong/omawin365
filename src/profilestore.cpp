#include "profilestore.h"
#include "rdpprofile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QStringView>
#include <QUuid>
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr qint64 maximumProfileBytes = 1024 * 1024;
constexpr auto privateFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
const QRegularExpression profileId(QStringLiteral("^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$"));

bool fail(QString* error, const QString& message)
{
    if (error)
        *error = message;
    return false;
}

bool privateOwnedFile(const QString& path)
{
    struct stat st {};
    const QByteArray encoded = QFile::encodeName(path);
    return ::lstat(encoded.constData(), &st) == 0 && S_ISREG(st.st_mode)
        && st.st_uid == ::getuid() && (st.st_mode & 0077) == 0;
}

bool atomicPrivateWrite(const QString& path, const QByteArray& bytes, QString* error)
{
    if (QFileInfo::exists(path) && !privateOwnedFile(path))
        return fail(error, QStringLiteral("An application data file has unsafe permissions or ownership."));
    if (QFileInfo(path).isSymLink())
        return fail(error, QStringLiteral("Application data must not be a symbolic link."));
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(privateFile)
        || file.write(bytes) != bytes.size() || !file.commit())
        return fail(error, QStringLiteral("Could not save the private connection profile."));
    return true;
}

bool validateDisplayName(const QString& value, QString* error)
{
    for (const QChar character : value) {
        const auto category = character.category();
        if (category == QChar::Other_Control || category == QChar::Separator_Line
            || category == QChar::Separator_Paragraph)
            return fail(error, QStringLiteral("The connection name must not contain control characters or line breaks."));
    }
    const auto trimmed = QStringView(value).trimmed();
    if (trimmed.isEmpty())
        return fail(error, QStringLiteral("Enter a connection name."));
    if (trimmed.size() > 160)
        return fail(error, QStringLiteral("The connection name must be no longer than 160 characters."));
    return true;
}
}

ProfileStore::ProfileStore(QObject* parent) : QObject(parent)
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    root_ = QDir(base).filePath(QStringLiteral("omawin365"));
    if (ensurePrivateRoot(&initializationError_))
        loadIndex();
}

bool ProfileStore::ensurePrivateRoot(QString* error) const
{
    if (QFileInfo(root_).isSymLink())
        return fail(error, QStringLiteral("OMAWIN365's data directory must not be a symbolic link."));
    if (!QFileInfo::exists(root_)) {
        const QString parent = QFileInfo(root_).absolutePath();
        if (!QDir().mkpath(parent))
            return fail(error, QStringLiteral("Could not create the application data parent directory."));
        if (::mkdir(QFile::encodeName(root_).constData(), 0700) != 0 && !QFileInfo::exists(root_))
            return fail(error, QStringLiteral("Could not create private OMAWIN365 storage."));
    }
    struct stat st {};
    if (::lstat(QFile::encodeName(root_).constData(), &st) != 0 || !S_ISDIR(st.st_mode)
        || st.st_uid != ::getuid() || (st.st_mode & 0077) != 0)
        return fail(error, QStringLiteral("OMAWIN365's data directory must be owned by you with permissions 0700."));
    return true;
}

QList<Profile> ProfileStore::profiles() const
{
    return profiles_;
}

void ProfileStore::loadIndex()
{
    const QString index = QDir(root_).filePath(QStringLiteral("profiles.json"));
    if (!QFileInfo::exists(index))
        return;
    QFile file(index);
    if (!privateOwnedFile(index) || !file.open(QIODevice::ReadOnly) || file.size() > maximumProfileBytes) {
        initializationError_ = QStringLiteral("The saved profile list is not a readable private application file.");
        return;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        initializationError_ = QStringLiteral("The saved profile list is damaged; it was left unchanged.");
        return;
    }
    QList<Profile> loaded;
    QSet<QString> seen;
    for (const auto& value : document.array()) {
        const auto object = value.toObject();
        const QString id = object.value(QStringLiteral("id")).toString();
        const QString name = object.value(QStringLiteral("name")).toString();
        if (!profileId.match(id).hasMatch() || seen.contains(id) || name.isEmpty() || name.size() > 160) {
            initializationError_ = QStringLiteral("The saved profile list contains invalid metadata; it was left unchanged.");
            return;
        }
        const QString path = QDir(root_).filePath(id + QStringLiteral(".rdpw"));
        if (!privateOwnedFile(path)) {
            initializationError_ = QStringLiteral("A saved connection is missing or has unsafe ownership or permissions.");
            return;
        }
        seen.insert(id);
        loaded.append({id, name, path});
    }
    profiles_ = std::move(loaded);
}

bool ProfileStore::saveIndex(const QList<Profile>& profiles, QString* error) const
{
    QJsonArray array;
    for (const auto& profile : profiles)
        array.append(QJsonObject{{QStringLiteral("id"), profile.id}, {QStringLiteral("name"), profile.name}});
    return atomicPrivateWrite(QDir(root_).filePath(QStringLiteral("profiles.json")),
                              QJsonDocument(array).toJson(QJsonDocument::Compact), error);
}

Profile ProfileStore::importFile(const QString& sourcePath, QString* error, const QString& displayName)
{
    if (error)
        error->clear();
    if (!initializationError_.isEmpty()) {
        fail(error, initializationError_);
        return {};
    }
    if (!displayName.isNull() && !validateDisplayName(displayName, error))
        return {};
    if (!ensurePrivateRoot(error))
        return {};
    const QFileInfo info(sourcePath);
    const QString suffix = info.suffix().toLower();
    if (!info.isFile() || (suffix != QStringLiteral("rdp") && suffix != QStringLiteral("rdpw"))) {
        fail(error, QStringLiteral("Choose a downloaded .rdp or .rdpw connection file."));
        return {};
    }
    QByteArray bytes;
    if (!RdpProfile::readAndValidate(info.absoluteFilePath(), &bytes, error))
        return {};
    QString name;
    if (displayName.isNull()) {
        name = info.completeBaseName().simplified();
        name.remove(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")));
        name = name.left(160);
        if (name.isEmpty())
            name = QStringLiteral("Cloud PC");
    } else {
        name = displayName.trimmed();
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    Profile profile{id, name, QDir(root_).filePath(id + QStringLiteral(".rdpw"))};
    if (!atomicPrivateWrite(profile.path, bytes, error))
        return {};
    QList<Profile> updated = profiles_;
    updated.append(profile);
    if (!saveIndex(updated, error)) {
        QFile::remove(profile.path);
        return {};
    }
    profiles_ = std::move(updated);
    emit changed();
    return profile;
}

bool ProfileStore::renameProfile(const QString& id, const QString& name, QString* error)
{
    if (error)
        error->clear();
    if (!initializationError_.isEmpty())
        return fail(error, initializationError_);
    if (!ensurePrivateRoot(error))
        return false;
    const auto found = std::find_if(profiles_.cbegin(), profiles_.cend(), [&id](const Profile& profile) { return profile.id == id; });
    if (found == profiles_.cend())
        return fail(error, QStringLiteral("The selected saved connection no longer exists."));
    if (!validateDisplayName(name, error))
        return false;
    if (!privateOwnedFile(found->path))
        return fail(error, QStringLiteral("Refusing to rename a connection with unsafe ownership or permissions."));
    const QString normalized = name.trimmed();
    if (normalized == found->name)
        return true;
    QList<Profile> updated = profiles_;
    updated[std::distance(profiles_.cbegin(), found)].name = normalized;
    if (!saveIndex(updated, error))
        return false;
    profiles_ = std::move(updated);
    emit changed();
    return true;
}

bool ProfileStore::removeProfile(const QString& id, QString* error)
{
    if (error)
        error->clear();
    if (!initializationError_.isEmpty())
        return fail(error, initializationError_);
    if (!ensurePrivateRoot(error))
        return false;
    const auto found = std::find_if(profiles_.cbegin(), profiles_.cend(), [&id](const Profile& profile) { return profile.id == id; });
    if (found == profiles_.cend())
        return fail(error, QStringLiteral("The selected saved connection no longer exists."));
    const QString path = found->path;
    if (!privateOwnedFile(path))
        return fail(error, QStringLiteral("Refusing to remove a connection with unsafe ownership or permissions."));
    QList<Profile> updated = profiles_;
    updated.removeAt(std::distance(profiles_.cbegin(), found));
    if (!saveIndex(updated, error))
        return false;
    profiles_ = std::move(updated);
    const bool removed = QFile::remove(path);
    emit changed();
    if (!removed)
        return fail(error, QStringLiteral("The entry was removed, but its private file could not be deleted."));
    return true;
}
