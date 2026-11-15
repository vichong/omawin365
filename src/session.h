#pragma once

#include "oauthcontract.h"
#include "deviceoptions.h"
#include <QObject>
#include <QString>
#include <QUrl>
#include <memory>

// Fixed per connection attempt: captured at Connect/Reconnect, never re-read mid-attempt.
struct LaunchOptions {
    int desktopScale = 100; // Percent, 100..500; anything else is sent as 100.
    DeviceOptions devices;  // The connected Cloud PC's saved choices.
    bool operator==(const LaunchOptions&) const = default;
};

class Session : public QObject
{
    Q_OBJECT
public:
    explicit Session(QObject* parent = nullptr);
    ~Session() override;
    bool active() const;
    // False when the verified FreeRDP is 3.32.2, which refuses Windows'
    // overlapping WebAuthn channels (FreeRDP#13741), so security keys inside
    // the Cloud PC fall back to Windows' own prompt. Valid after a version check.
    bool inSessionKeysSupported() const;
    void setDiagnosticsEnabled(bool enabled);

public slots:
    void start(const QString& profilePath, const LaunchOptions& options = {});
    void stop();
    void submitPin(const QString& pin);
    void submitAuthResult(const OAuthContract::Callback& result);

signals:
    // Fixed event labels only; never pass authentication or endpoint data here.
    void diagnosticEvent(const QString& event);
    void statusChanged(const QString& phase, const QString& detail);
    void pinRequested(const QString& message);
    void authRequested(const OAuthContract::Request& request);
    void connected();
    void ended();
    void error(const QString& message);

private:
    friend struct SessionTestAccess;
    struct State;
    std::unique_ptr<State> state_;
    void verifyExecutable();
    void startTransport(const QString& profilePath);
    void readDiagnostics();
    void readOutput();
    void flushInput();
    void checkChild();
    void checkDesktop();
    void fail(const QString& message);
    void releaseTransport();
    void stopTransport();
    void rejectCertificate(const QString& message);
    bool ownsTransport(quint64 epoch, qint64 child, int master) const;
    void queueInput(QByteArray bytes);
    void diagnose(const char* event);
};
