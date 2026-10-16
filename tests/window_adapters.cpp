#include "window_fixture.h"
#include "session.h"
#include "browserauth.h"

namespace WindowFixture { Controls controls; }
using WindowFixture::controls;

struct Session::State {};
Session::Session(QObject* parent) : QObject(parent), state_(std::make_unique<State>()) {}
Session::~Session() = default;
bool Session::active() const { return controls.active; }
void Session::setDiagnosticsEnabled(bool) {}
void Session::start(const QString& path, const LaunchOptions& options)
{
    controls.starts.append(path);
    controls.options.append(options);
    controls.active = true;
    emit statusChanged("connecting", "Synthetic transport starting");
}
void Session::stop()
{
    ++controls.stops;
    emit statusChanged("disconnecting", "Synthetic transport stopping");
    if (controls.onStop) controls.onStop();
}
void Session::submitPin(const QString& pin)
{
    controls.pins.append(pin);
    if (controls.onPin) controls.onPin();
}
void Session::submitAuthResult(const OAuthContract::Callback& result) { controls.callbacks.append(result.url); }

struct BrowserAuth::Private {};
BrowserAuth::BrowserAuth(QObject* parent) : QObject(parent), d(std::make_unique<Private>()) {}
BrowserAuth::~BrowserAuth() = default;
void BrowserAuth::setDiagnosticsEnabled(bool) {}
void BrowserAuth::begin(const OAuthContract::Request& request) { controls.authorizations.append(request.authorization); }
void BrowserAuth::beginProfileDownload()
{
    ++controls.acquisitions;
    if (controls.onAcquire) controls.onAcquire();
}
void BrowserAuth::selectResource(const QString& id) { controls.selections.append(id); }
void BrowserAuth::cancel()
{
    ++controls.cancellations;
    if (controls.onCancel) controls.onCancel();
}

void BrowserAuth::setRememberSignIn(RememberSignIn mode) { controls.rememberSignIn = int(mode); }
void BrowserAuth::forgetSignIn() { ++controls.forgets; controls.rememberSignIn = 0; }
void BrowserAuth::forgetSavedSignIn() { ++controls.savedForgets; }
