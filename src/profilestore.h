#pragma once

#include <QObject>
#include <QList>
#include <QString>

struct Profile {
    QString id;
    QString name;
    QString path;
};

class ProfileStore final : public QObject {
    Q_OBJECT
public:
    explicit ProfileStore(QObject* parent = nullptr);
    QList<Profile> profiles() const;
    Profile importFile(const QString& sourcePath, QString* error, const QString& displayName = {});
    bool renameProfile(const QString& id, const QString& name, QString* error);
    bool removeProfile(const QString& id, QString* error);

signals:
    void changed();

private:
    QString root_;
    QString initializationError_;
    QList<Profile> profiles_;
    bool ensurePrivateRoot(QString* error) const;
    bool saveIndex(const QList<Profile>& profiles, QString* error) const;
    void loadIndex();
};
