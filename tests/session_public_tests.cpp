#include "../src/browserauth.cpp"
#include "auth_composed.h"
#include "../src/browserauth.h"
// Test-only access to CDP/owned-page scaffolding; all contract validation,
// interception, signals, Session PTY serialization and submission are production.
bool BrowserAuthTestAccess::inertTransport(BrowserAuth& browser) {
        const pid_t parent = ::getpid();
        browser.d->process.setChildProcessModifier([parent] {
            if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() != parent || ::setsid() < 0)
                ::_exit(126);
        });
        browser.d->process.start(QStringLiteral("/usr/bin/sleep"), {QStringLiteral("60")});
        if (!browser.d->process.waitForStarted(3000)) return false;
        browser.d->writeFd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
        return browser.d->writeFd >= 0;
    }
void BrowserAuthTestAccess::adoptPage(BrowserAuth& browser) {
        browser.d->target = "fixture-page";
        browser.d->pages.insert("fixture-page", {browser.d->round, "fixture-session", "fixture-frame", {}, false});
    }
void BrowserAuthTestAccess::callback(BrowserAuth& browser, const QUrl& url, const QString& method) {
        browser.d->handleMessage({{"method", "Fetch.requestPaused"}, {"sessionId", "fixture-session"},
            {"params", QJsonObject{{"requestId", "fixture-request"}, {"resourceType", "Document"},
                {"frameId", "fixture-frame"}, {"request", QJsonObject{{"method", method},
                    {"url", url.toString(QUrl::FullyEncoded)}}}}}});
    }

#include "session_public_cases.h"
#include <X11/Xlib.h>

bool sessionFixtureDisplayAvailable()
{
    Display* display = XOpenDisplay(nullptr);
    if (!display)
        return false;
    XCloseDisplay(display);
    return true;
}

QTEST_GUILESS_MAIN(SessionPublicTest)
