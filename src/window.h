#pragma once

#include "browserauth.h"
#include "session.h"

#include <QPointer>
#include <QWidget>

class BrowserAuth;
class ProfileStore;
class Session;
class Theme;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDialog;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

class Window final : public QWidget {
    Q_OBJECT
public:
    Window(Session* session, BrowserAuth* browser, ProfileStore* profiles, QWidget* parent = nullptr);
    bool importProfile(const QString& sourcePath);

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void refreshProfiles(const QString& preferredId = {});
    void fitProfileList();
    void markConnectedProfile();
    void showIdlePrompt();
    void updateControls();
    void setStatus(const QString& phase, const QString& detail);
    void connectSelected();
    void disconnectSession();
    void reconnectSession();
    void removeSelected();
    void renameSelected();
    void acquireProfile();
    void clearAcquisition();
    void cancelAcquisition();
    void requestPin(const QString& message);
    void showTouch();
    void dismissChallenge();
    void updatePinFocus();
    void showError(const QString& title, const QString& message);
    void showAbout();
    void applySpacing();
    // Session settings: scale is app-wide, devices are per Cloud PC; captured per attempt at Connect/Reconnect.
    LaunchOptions launchOptions(const QString& path) const;
    void refreshAutoScale();
    void updatePendingOptions();
    void forgetSignIn();
    QString selectedId() const;
    QString selectedPath() const;
    DeviceOptions devicesForPath(const QString& path) const;
    void refreshDeviceOptions();
    void saveDeviceOptions();

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
    QComboBox* m_rememberSignIn;
    BrowserAuth::RememberSignIn m_rememberMode = BrowserAuth::RememberSignIn::Off;
    QPushButton* m_forgetSignIn;
    QLabel* m_signInCacheNote;
    bool m_forgettingSignIn = false;
    bool m_forgetFailed = false;
    QComboBox* m_scale;
    QCheckBox* m_clipboard;
    QLabel* m_optionsPending;
    QCheckBox* m_audioPlayback;
    QCheckBox* m_audioCapture;
    QLabel* m_devicesNote;
    QVBoxLayout* m_contentLayout;
    QWidget* m_connectionPanel;
    QVBoxLayout* m_statusLayout;
    struct Challenge {
        QPointer<QDialog> dialog;
        QPointer<QLineEdit> pin;
        QPointer<QLabel> title;
        QPointer<QLabel> message;
        QPointer<QLabel> detail;
        // One fixed-height slot: focus button, PIN field or touch note. Focus
        // follows the mouse on Hyprland, so the dialog must not resize per state.
        QPointer<QStackedWidget> slot;
        QPointer<QWidget> sent;
        QPointer<QPushButton> submit;
        QPointer<QPushButton> focus;
    } m_challenge;
    struct Acquisition {
        QPointer<QWidget> panel;
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
    bool m_signInClosed = false; // The user closed the sign-in window; the stop is a cancel.
    bool m_idlePrompt = true; // The detail text is our own idle prompt, not a status message.
    QString m_activeProfile;
    QString m_restartPath;
    int m_autoScale = 100; // Last Auto result; refreshed only while Auto is selected.
    LaunchOptions m_activeOptions;
    LaunchOptions m_restartOptions;
    bool m_connected = false;
    bool m_stopping = false;
    bool m_closePending = false;
    bool m_errorVisible = false;
    bool m_dismissingChallenge = false;
};
