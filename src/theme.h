#pragma once

#include <QHash>
#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>

class Theme final : public QObject {
    Q_OBJECT
public:
    explicit Theme(QObject* parent = nullptr);
    int spacing(const QString& token, int fallback) const;
    int fontSize(const QString& token) const;
    QString fontFamily() const { return m_fontFamily; }

signals:
    void changed();

private:
    void reload();
    void watchPaths();
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_themePath;
    QString m_configPath;
    QString m_fontPath;
    QHash<QString, QString> m_values;
    double m_spacingScale = 1.0;
    int m_baseSize = 12;
    QString m_fontFamily = QStringLiteral("monospace");
    bool m_fontDirty = true;
};
