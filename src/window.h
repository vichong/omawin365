#pragma once

#include <QPointer>
#include <QWidget>

class BrowserAuth;
class ProfileStore;
class Session;
class Theme;
class QCloseEvent;
class QDialog;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QVBoxLayout;

class Window final : public QWidget {
    Q_OBJECT
public:
    Window(Session* session, BrowserAuth* browser, ProfileStore* profiles, QWidget* parent = nullptr);
    bool importProfile(const QString& sourcePath);

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refreshProfiles(const QString& preferredId = {});
    void updateControls();
    void setStatus(const QString& phase, const QString& detail);
    void connectSelected();
    void disconnectSession();
    void reconnectSession();
    void removeSelected();
    void renameSelected();
    void acquireProfile();
    void requestPin(const QString& message);
    void showTouch();
    void dismissChallenge();
    void updatePinFocus();
    void showError(const QString& title, const QString& message);
    void showAbout();
    void applySpacing();
    QString selectedId() const;
    QString selectedPath() const;

    Session* m_session;
    BrowserAuth* m_browser;
    ProfileStore* m_profiles;
    Theme* m_theme;
    QListWidget* m_list;
    QLabel* m_empty;
    QLabel* m_phaseLabel;
    QLabel* m_detailLabel;
    QLabel* m_progressNote;
    QLabel* m_sessionLabel;
    QPushButton* m_import;
    QPushButton* m_acquire;
    QPushButton* m_remove;
    QPushButton* m_rename;
    QPushButton* m_connect;
    QPushButton* m_disconnect;
    QPushButton* m_reconnect;
    QVBoxLayout* m_contentLayout;
    QWidget* m_connectionPanel;
    QVBoxLayout* m_statusLayout;
    struct Challenge {
        QPointer<QDialog> dialog;
        QPointer<QLineEdit> pin;
        QPointer<QLabel> title;
        QPointer<QLabel> detail;
        QPointer<QPushButton> submit;
        QPointer<QPushButton> focus;
    } m_challenge;
    struct Acquisition {
        QPointer<QDialog> guide;
        QPointer<QLabel> status;
        QPointer<QListWidget> resources;
        QPointer<QPushButton> download;
        bool active = false;
        quint64 generation = 0;
    } m_acquisition;
    QPointer<QDialog> m_closeConfirmation;
    QPointer<QDialog> m_renameDialog;
    QPointer<QDialog> m_aboutDialog;
    QString m_phase = QStringLiteral("disconnected");
    QString m_activeProfile;
    QString m_restartPath;
    bool m_connected = false;
    bool m_stopping = false;
    bool m_closePending = false;
    bool m_errorVisible = false;
    bool m_dismissingChallenge = false;
};
