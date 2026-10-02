#pragma once

#include "oauthcontract.h"
#include <QObject>
#include <QString>
#include <QUrl>
#include <memory>

class Session : public QObject
{
    Q_OBJECT
public:
    explicit Session(QObject* parent = nullptr);
    ~Session() override;
    bool active() const;
    void setDiagnosticsEnabled(bool enabled);

public slots:
    void start(const QString& profilePath);
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
