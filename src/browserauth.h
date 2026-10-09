#pragma once

#include "oauthcontract.h"
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <memory>
#include <optional>

class BrowserAuth final : public QObject
{
    Q_OBJECT
public:
    explicit BrowserAuth(QObject* parent = nullptr);
    ~BrowserAuth() override;
    void setDiagnosticsEnabled(bool enabled);

public slots:
    void begin(const OAuthContract::Request& request);
    void beginProfileDownload();
    void selectResource(const QString& id);
    void cancel();

signals:
    void callbackReady(const OAuthContract::Callback& result);
    void failed(const QString& message);
    // The user closed the sign-in window or browser before the request completed.
    void closed();
    void resourcesAvailable(const QStringList& ids, const QStringList& names);
    void profileDownloaded(const QString& temporaryPath, const QString& displayName);
    void status(const QString& message);
    void diagnosticEvent(const QString& event);

private:
    enum class CallbackKind { NotCallback, Code, OAuthError, Invalid };
    static bool validateAuthorization(const QUrl& url, std::optional<QString>* state);
    static CallbackKind classifyCallback(const QUrl& url, const std::optional<QString>& state);
    friend struct BrowserAuthTestAccess;
    struct Private;
    std::unique_ptr<Private> d;
};
