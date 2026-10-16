// Include the implementation to drive real CDP replies through the private
// acquisition state machine without starting Chromium or exposing a test API.
#include "../src/browserauth.cpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QUrlQuery>
#include <cstdio>
#include <sys/syscall.h>

// Counts process-group signals sent while no process or group holds the number:
// the precondition for a recycled number reaching an unrelated process.
static int unreservedGroupSignals = 0;
extern "C" int kill(pid_t pid, int signal) noexcept
{
    if (pid < -1 && signal != 0 && syscall(SYS_kill, pid, 0) != 0 && errno == ESRCH
        && syscall(SYS_kill, -pid, 0) != 0 && errno == ESRCH)
        ++unreservedGroupSignals;
    return int(syscall(SYS_kill, pid, signal));
}

namespace { QUrl syntheticAuthorization(const QString& state); }

struct BrowserAuthTestAccess {
    static void useSyntheticBrowser(BrowserAuth& browser, const QString& scenario, const QString& trace)
    {
        browser.d->browserExecutable = QCoreApplication::applicationFilePath();
        browser.d->browserArgumentPrefix = {"--synthetic-chromium", scenario, trace};
    }
    static bool authorize(const QUrl& url)
    {
        std::optional<QString> state;
        return BrowserAuth::validateAuthorization(url, &state);
    }
    static bool code(const QUrl& url, const QString& state = {}, bool hasState = false)
    {
        return BrowserAuth::classifyCallback(url,
            hasState ? std::optional<QString>(state) : std::nullopt) == BrowserAuth::CallbackKind::Code;
    }
    static bool error(const QUrl& url)
    {
        return BrowserAuth::classifyCallback(url, std::nullopt) == BrowserAuth::CallbackKind::OAuthError;
    }
    static bool stateDecoded(const QUrl& url, const QString& expected)
    {
        std::optional<QString> state;
        return BrowserAuth::validateAuthorization(url, &state) && state && *state == expected;
    }

    static void signInCache(const std::function<void(bool, const char*)>& check)
    {
        // Any keyring call made here must fail rather than touch the developer's keyring.
        const QByteArray bus = qgetenv("DBUS_SESSION_BUS_ADDRESS");
        qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-omawin365-fixture");
        using Mode = BrowserAuth::RememberSignIn;
        const auto prepare = [](BrowserAuth& browser, Mode mode = Mode::Device) {
            auto& state = *browser.d;
            state.writeFd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
            state.active = true;
            state.rememberSignIn = mode;
        };
        const double now = QDateTime::currentSecsSinceEpoch();
        const QJsonObject persistentCookie{{"name", "ESTSAUTHPERSISTENT"}, {"value", "fictional"},
            {"domain", ".login.microsoftonline.com"}, {"path", "/"},
            {"secure", true}, {"httpOnly", true}, {"expires", now + 3600}};
        const QJsonObject sessionCookie{{"name", "ESTSAUTH"}, {"value", "fictional"},
            {"domain", ".login.microsoftonline.com"}, {"path", "/"},
            {"secure", true}, {"httpOnly", true}, {"session", true}};
        for (const bool callbackSeen : {true, false}) {
            for (const QString error : {QStringLiteral("net::ERR_ABORTED"), QStringLiteral("net::ERR_NAME_NOT_RESOLVED")}) {
                BrowserAuth browser;
                prepare(browser);
                auto& state = *browser.d;
                const auto authorization = syntheticAuthorization(QString(64, 'a'));
                state.auth = {authorization, OAuthContract::request(authorization)};
                state.auth.contract->generation = 17;
                state.target = "owned-target";
                state.pages.insert(state.target, {state.round, "owned-session", "owned-frame", {}, false});
                int callbacks = 0, failures = 0;
                QObject::connect(&browser, &BrowserAuth::callbackReady, [&](const OAuthContract::Callback& result) {
                    ++callbacks;
                    check(result.generation == 17, "cookie snapshot preserves the callback generation");
                });
                QObject::connect(&browser, &BrowserAuth::failed, [&] { ++failures; });
                const int resume = state.nextId;
                state.resumePage(state.target, "owned-session");
                const int navigation = state.nextId;
                state.handleMessage({{"id", resume}, {"result", QJsonObject{}}});
                int snapshot = -1;
                if (callbackSeen) {
                    state.handleMessage({{"method", "Fetch.requestPaused"}, {"sessionId", "owned-session"},
                        {"params", QJsonObject{{"requestId", "callback-request"}, {"resourceType", "Document"},
                            {"frameId", "owned-frame"}, {"request", QJsonObject{{"method", "GET"},
                                {"url", "https://login.microsoftonline.com/common/oauth2/nativeclient?code=fictional-code&state=" + QString(64, 'a')}}}}}});
                    for (auto it = state.pending.cbegin(); it != state.pending.cend(); ++it)
                        if (it->method == "Storage.getCookies") snapshot = it.key();
                    check(snapshot >= 0 && state.finishingAuth && callbacks == 0,
                          "validated callback waits for the optional cookie snapshot");
                }
                // Chromium acknowledges the initial navigation after the Fetch abort,
                // while the asynchronous cookie read still keeps this round active.
                state.handleMessage({{"id", navigation}, {"result", QJsonObject{{"errorText", error}}}});
                if (callbackSeen && error == "net::ERR_ABORTED") {
                    state.handleMessage({{"id", snapshot}, {"error", QJsonObject{{"code", -1}}}});
                    check(callbacks == 1 && failures == 0 && !state.active,
                          "our callback interception abort is not a network failure while snapshotting cookies");
                } else {
                    check(callbacks == 0 && failures == 1 && !state.active,
                          "real navigation failures and aborts without a validated callback still stop sign-in");
                }
            }
        }
        {
            BrowserAuth browser;
            prepare(browser);
            auto& state = *browser.d;
            int completed = 0;
            const int id = state.nextId;
            state.snapshotSignIn([&] { ++completed; });
            check(completed == 0 && state.pending.value(id).method == "Storage.getCookies",
                  "snapshot waits for the browser cookie response");
            state.handleMessage({{"id", id}, {"error", QJsonObject{{"code", -1}}}});
            check(completed == 1 && state.active,
                  "optional cookie-export failure still completes authentication");
        }
        {
            BrowserAuth browser;
            prepare(browser);
            auto& state = *browser.d;
            int completed = 0;
            const int id = state.nextId;
            state.snapshotSignIn([&] { ++completed; });
            browser.cancel();
            state.handleMessage({{"id", id}, {"result", QJsonObject{{"cookies", QJsonArray{}}}}});
            check(completed == 0 && !state.active,
                  "cancellation retires an in-flight cookie snapshot without publishing authentication");
        }
        {
            BrowserAuth browser;
            prepare(browser);
            auto& state = *browser.d;
            int completed = 0;
            QObject::connect(&browser, &BrowserAuth::signInCacheNotice, [&] { browser.cancel(); });
            const int id = state.nextId;
            state.snapshotSignIn([&] { ++completed; });
            state.handleMessage({{"id", id}, {"error", QJsonObject{{"code", -1}}}});
            check(completed == 0 && !state.active,
                  "reentrant cancellation from cache notice cannot publish a stale callback");
        }
        {
            BrowserAuth browser;
            prepare(browser);
            auto& state = *browser.d;
            state.restoreCookies = {QJsonObject{{"name", "fictional"}}};
            const int id = state.nextId;
            state.prepareFlow();
            check(state.pending.value(id).method == "Storage.setCookies" && state.restoreCookies.isEmpty(),
                  "remembered cookies are restored once before preparing any sign-in page");
            state.handleMessage({{"id", id}, {"result", QJsonObject{}}});
            check(state.pending.value(id + 1).method == "Browser.setDownloadBehavior",
                  "sign-in preparation resumes after cookie restoration acknowledgement");
        }
        {
            BrowserAuth browser;
            prepare(browser);
            auto& state = *browser.d;
            int completed = 0;
            const int id = state.nextId;
            state.snapshotSignIn([&] { ++completed; });
            state.pending[id].deadline = -1;
            QMetaObject::invokeMethod(&state.watchdog, "timeout", Qt::DirectConnection);
            check(completed == 1 && state.active && !state.pending.contains(id),
                  "cookie-export timeout falls back without failing authentication");
        }
        {
            BrowserAuth browser;
            auto& state = *browser.d;
            browser.setRememberSignIn(Mode::Device);
            state.loadingCache = true;
            browser.cancel();
            emit state.cache.loaded(SignInCookies::encode({persistentCookie}, now), true);
            check(!state.loadingCache && !state.active && state.process.state() == QProcess::NotRunning,
                  "keyring load completing after cancellation cannot start a browser");
            state.loadingCache = true;
            browser.setRememberSignIn(Mode::Off);
            check(!state.loadingCache && state.restoreCookies.isEmpty(),
                  "disabling persistence retires a load that Forget may drop from the queue");
        }
        {
            const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR");
            qputenv("XDG_RUNTIME_DIR", "/nonexistent-omawin365-fixture");
            BrowserAuth browser;
            auto& state = *browser.d;
            state.active = true;
            state.rememberSignIn = Mode::Device;
            state.startBrowser();
            check(!state.loadingCache && !state.active,
                  "unsafe runtime storage is rejected before accessing remembered credentials");
            if (runtime.isNull()) qunsetenv("XDG_RUNTIME_DIR");
            else qputenv("XDG_RUNTIME_DIR", runtime);
        }
        {
            BrowserAuth browser;
            auto& state = *browser.d;
            state.writeFd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
            state.active = true;
            int completed = 0;
            const int id = state.nextId;
            state.snapshotSignIn([&] { ++completed; });
            check(completed == 1 && state.nextId == id,
                  "default private mode never exports cookies or accesses the keyring");
        }
        // Counts real keyring jobs. D-Bus is unreachable here, so each one fails fast but still reports.
        const auto keyringCalls = [](BrowserAuth::Private& state) {
            int calls = 0;
            const auto count = [&calls] { ++calls; };
            const auto a = QObject::connect(&state.cache, &SignInCache::loaded, count);
            const auto b = QObject::connect(&state.cache, &SignInCache::saved, count);
            QElapsedTimer waited;
            waited.start();
            while (waited.elapsed() < 1000 && !calls) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QObject::disconnect(a);
            QObject::disconnect(b);
            return calls;
        };
        for (const Mode mode : {Mode::AppSession, Mode::Device}) {
            const bool device = mode == Mode::Device;
            BrowserAuth browser;
            prepare(browser, mode);
            auto& state = *browser.d;
            QString notice;
            QObject::connect(&browser, &BrowserAuth::signInCacheNotice, [&](const QString& text) { notice = text; });
            int completed = 0;
            const int id = state.nextId;
            state.snapshotSignIn([&] { ++completed; });
            state.handleMessage({{"id", id}, {"result", QJsonObject{{"cookies", QJsonArray{persistentCookie, sessionCookie}}}}});
            check(completed == 1 && state.liveSignInCookies.size() == 2 && state.restoreCookies.size() == 2,
                  "both remembering choices keep session and persistent sign-in cookies in memory");
            check(keyringCalls(state) == (device ? 1 : 0),
                  device ? "on-this-device saves to the keyring" : "until-app-closes never saves to the keyring");
            if (!device)
                check(notice.contains("until you close OMAWIN365"), "until-app-closes says how long sign-in is kept");
            if (device) {
                // Starting without memory waits on a keyring load. Until-app-closes would
                // launch Chromium here instead, so only the keyring choice is driven.
                state.liveSignInCookies = {};
                state.active = true;
                state.startBrowser();
                state.active = false; // The finished load must not launch anything.
                check(state.loadingCache && keyringCalls(state) == 1, "on-this-device loads from the keyring");
            }
        }
        {
            BrowserAuth browser;
            prepare(browser, Mode::Device);
            auto& state = *browser.d;
            state.liveSignInCookies = {sessionCookie};
            state.restoreCookies = {persistentCookie, sessionCookie}; // As if loaded from the keyring.
            browser.setRememberSignIn(Mode::AppSession);
            check(state.restoreCookies == QJsonArray{sessionCookie} && state.liveSignInCookies.size() == 1,
                  "leaving the keyring setting keeps this app session's sign-in but not keyring-loaded cookies");
            browser.setRememberSignIn(Mode::Off);
            check(state.restoreCookies.isEmpty() && state.liveSignInCookies.isEmpty(),
                  "turning remembering off drops sign-in held in memory");
        }
        {
            BrowserAuth browser;
            browser.setRememberSignIn(Mode::Device);
            auto& state = *browser.d;
            state.loadingCache = true;
            int notices = 0;
            QString notice;
            QObject::connect(&browser, &BrowserAuth::signInCacheNotice, [&](const QString& text) { notice = text; ++notices; });
            emit state.cache.loaded(SignInCookies::encode({persistentCookie}, now), true);
            check(!state.loadingCache && state.restoreCookies.size() == 1 && notice.contains("Restoring"),
                  "a keyring load supplies saved sign-in to restore");
            browser.setRememberSignIn(Mode::AppSession);
            emit state.cache.loaded(SignInCookies::encode({persistentCookie}, now), true);
            check(state.restoreCookies.isEmpty() && notices == 1,
                  "a keyring load finishing after leaving the keyring setting is ignored");
        }
        if (bus.isNull()) qunsetenv("DBUS_SESSION_BUS_ADDRESS");
        else qputenv("DBUS_SESSION_BUS_ADDRESS", bus);
    }

    static void acquisition(const std::function<void(bool, const char*)>& check)
    {
        const QJsonArray resources{
            QJsonObject{{"id", "first"}, {"name", "First PC"}},
            QJsonObject{{"id", "second"}, {"name", "Second PC"}}
        };
        const auto prepare = [](BrowserAuth& browser) {
            auto& state = *browser.d;
            state.cancelRound();
            // send() needs a writable transport, but these synthetic commands
            // must never reach a browser. Replies go through handleMessage().
            if (state.writeFd < 0)
                state.writeFd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
            state.mode = BrowserAuth::Private::Mode::Acquisition;
            state.active = true;
            state.target = "target";
            state.portal.context = 1;
            state.pages.insert(state.target, {state.round, "session", "frame",
                {{"frame", QUrl("https://client.wvd.microsoft.com/arm/webclient/index.html")}}, false});
        };
        const auto probe = [](BrowserAuth& browser) {
            const int id = browser.d->nextId;
            browser.d->evaluatePortal("poll", {}, false);
            return id;
        };
        const auto reply = [](BrowserAuth& browser, int id, const QJsonArray& items) {
            browser.d->handleMessage({{"id", id}, {"result", QJsonObject{
                {"result", QJsonObject{{"value", QJsonObject{
                    {"state", "resources"}, {"resources", items}}}}}}}});
        };
        const auto navigate = [](BrowserAuth& browser) {
            browser.d->handleMessage({{"method", "Page.frameNavigated"}, {"sessionId", "session"},
                {"params", QJsonObject{{"frame", QJsonObject{{"id", "frame"},
                    {"url", "https://client.wvd.microsoft.com/arm/webclient/index.html"}}}}}});
        };
        for (const bool restartOnFailure : {false, true}) {
            BrowserAuth browser;
            prepare(browser);
            int failures = 0, statuses = 0, choices = 0;
            QObject::connect(&browser, &BrowserAuth::failed, [&] {
                ++failures;
                if (restartOnFailure) prepare(browser);
            });
            QObject::connect(&browser, &BrowserAuth::status, [&] { ++statuses; });
            QObject::connect(&browser, &BrowserAuth::resourcesAvailable, [&] { ++choices; });
            const int id = probe(browser);
            browser.selectResource("removed");
            reply(browser, id, resources);
            check(failures == 1 && statuses == 0 && choices == 0 && !browser.d->download.armed(),
                  restartOnFailure ? "stale selection cannot publish into a reentrant new round"
                                   : "stale deferred selection stops after failure");
        }
        {
            BrowserAuth browser;
            prepare(browser);
            int choices = 0;
            QObject::connect(&browser, &BrowserAuth::status, [&] { navigate(browser); });
            QObject::connect(&browser, &BrowserAuth::resourcesAvailable,
                [&](const QStringList& ids, const QStringList&) { if (!ids.isEmpty()) ++choices; });
            reply(browser, probe(browser), resources);
            check(choices == 0 && !browser.d->portal.context,
                  "navigation during status cannot publish old-document choices");
        }
        {
            BrowserAuth browser;
            prepare(browser);
            reply(browser, probe(browser), resources);
            const int oldProbe = probe(browser);
            navigate(browser);
            browser.selectResource("first");
            reply(browser, oldProbe, resources);
            check(!browser.d->download.armed() && browser.d->portal.resources.isEmpty(),
                  "navigation rejects old selection and in-flight probe");
            browser.d->portal.context = 2;
            int choices = 0;
            QObject::connect(&browser, &BrowserAuth::resourcesAvailable,
                [&](const QStringList& ids, const QStringList&) { if (ids.size() == 1) ++choices; });
            reply(browser, probe(browser), {resources.at(1)});
            check(choices == 1 && !browser.d->download.armed(),
                  "explicit choice survives navigation from multiple to one resource");
            browser.selectResource("second");
            const int sent = browser.d->nextId;
            browser.selectResource("second");
            check(browser.d->download.armed() && browser.d->nextId == sent,
                  "refreshed explicit selection sends only one click");
            navigate(browser);
            check(!browser.d->active, "navigation after selection fails closed");
        }
    }

    static void handoff(const std::function<void(bool, const char*)>& check)
    {
        struct Capture {
            int reader = -1;
            BrowserAuth::Private* transport = nullptr;
            QByteArray bytes;
            QList<QJsonObject> frames;
            bool valid = true;
            ~Capture() { closeFd(reader); }
            void collect()
            {
                char buffer[4096];
                ssize_t count;
                do {
                    // Match the production nonblocking transport, and drain queued
                    // frames even when the kernel gives the fixture a small pipe.
                    if (transport) transport->flushWrites();
                    while ((count = ::read(reader, buffer, sizeof(buffer))) > 0)
                        bytes.append(buffer, count);
                } while (transport && !transport->output.isEmpty());
                qsizetype delimiter;
                while ((delimiter = bytes.indexOf('\0')) >= 0) {
                    QJsonParseError error;
                    const auto document = QJsonDocument::fromJson(bytes.left(delimiter), &error);
                    valid = valid && error.error == QJsonParseError::NoError && document.isObject();
                    if (document.isObject()) frames.append(document.object());
                    bytes.remove(0, delimiter + 1);
                }
            }
            int closes(const QString& target)
            {
                collect();
                if (!valid) return -1;
                int count = 0;
                for (const auto& frame : frames)
                    if (frame.value("method").toString() == "Target.closeTarget"
                        && frame.value("params").toObject().value("targetId").toString() == target)
                        ++count;
                return count;
            }
            int latestId(const QString& method)
            {
                collect();
                for (auto it = frames.crbegin(); it != frames.crend(); ++it)
                    if (it->value("method").toString() == method)
                        return it->value("id").toInt(-1);
                return -1;
            }
        };
        const auto ownedRound = [&check](BrowserAuth& browser, Capture& capture) {
            auto& state = *browser.d;
            // A dedicated idle child supplies real QProcess running/teardown
            // semantics; no Chromium or network is involved. CDP goes to our pipe.
            const pid_t parent = ::getpid();
            if (!privatePipe(&state.anchorRead, &state.anchorWrite)) return false;
            const int hold = state.anchorRead;
            QProcess* const child = &state.process;
            state.process.setChildProcessModifier([child, parent, hold] { prepareBrowserChild(child, -1, -1, hold, parent); });
            state.process.start(QCoreApplication::applicationFilePath(), {"--idle-transport"});
            const bool started = state.process.waitForStarted(3000);
            closeFd(state.anchorRead);
            const bool piped = started && privatePipe(&capture.reader, &state.writeFd)
                && nonblocking(capture.reader) && nonblocking(state.writeFd)
                && ::fcntl(state.writeFd, F_SETPIPE_SZ, 4096) > 0;
            capture.transport = &state;
            check(piped, "isolated lifecycle transport started with a small nonblocking pipe");
            if (!piped) return false;
            state.mode = BrowserAuth::Private::Mode::Authentication;
            state.active = true;
            state.target = "old";
            state.auth = {QUrl("https://login.microsoftonline.com/common/oauth2/v2.0/authorize"), OAuthContract::Request{QUrl(), QString("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), QString(), state.round}};
            state.pages.insert("old", {state.round, "old-session", "old-frame", {}, false});
            return true;
        };
        const auto reply = [](BrowserAuth& browser, int id, const QJsonObject& result = {}) {
            browser.d->handleMessage({{"id", id}, {"result", result}});
        };
        const auto created = [&reply](BrowserAuth& browser, int id, const QString& target) {
            reply(browser, id, {{"targetId", target}});
        };
        const auto callback = [](BrowserAuth& browser) {
            browser.d->handleMessage({{"method", "Fetch.requestPaused"}, {"sessionId", "old-session"},
                {"params", QJsonObject{{"requestId", "old-request"}, {"resourceType", "Document"},
                    {"frameId", "old-frame"}, {"request", QJsonObject{{"method", "GET"},
                        {"url", "https://login.microsoftonline.com/common/oauth2/nativeclient?code=synthetic-code&state=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}}}}}});
        };
        {
            Capture capture;
            BrowserAuth browser;
            if (!ownedRound(browser, capture)) return;
            auto& state = *browser.d;
            int callbacks = 0;
            QObject::connect(&browser, &BrowserAuth::callbackReady, [&] { ++callbacks; });
            callback(browser);
            check(callbacks == 1 && !state.active, "successful callback makes the old round inactive");
            QUrl authorization("https://login.microsoftonline.com/common/oauth2/v2.0/authorize");
            QUrlQuery query;
            query.addQueryItem("client_id", "a85cf173-4192-42f8-81fa-777a763e6e2c");
            query.addQueryItem("response_type", "code");
            query.addQueryItem("scope", "https://www.wvd.microsoft.com/.default openid profile offline_access");
            query.addQueryItem("code_challenge", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
            query.addQueryItem("code_challenge_method", "S256");
            query.addQueryItem("redirect_uri", "https://login.microsoftonline.com/common/oauth2/nativeclient");
            query.addQueryItem("state", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
            authorization.setQuery(query);
            browser.begin(authorization);
            reply(browser, capture.latestId("Browser.setDownloadBehavior"));
            reply(browser, capture.latestId("Target.setAutoAttach"));
            reply(browser, capture.latestId("Target.setDiscoverTargets"));
            check(capture.closes("old") == 0 && state.active && state.auth.contract,
                  "public begin after callback does not close the last window before replacement");
            created(browser, capture.latestId("Target.createTarget"), "replacement");
            check(capture.closes("old") == 1 && capture.closes("replacement") == 0
                  && state.pages.contains("replacement") && state.retainedTarget.isEmpty(),
                  "public handoff closes the old window exactly once after adopting its replacement");
            browser.cancel();
            check(capture.closes("replacement") == 1 && !state.active,
                  "explicit cancellation closes the adopted replacement");
        }
        {
            Capture capture;
            BrowserAuth browser;
            if (!ownedRound(browser, capture)) return;
            auto& state = *browser.d;
            state.cancelRound(true);
            check(state.retainedTarget == "old" && state.pages.isEmpty()
                  && state.auth.authorization.isEmpty() && !state.auth.contract,
                  "replacement drops old authentication state while retaining one window");
            state.mode = BrowserAuth::Private::Mode::Acquisition;
            state.active = true;
            const int stale = state.nextId;
            state.createTarget();
            state.cancelRound(true);
            state.mode = BrowserAuth::Private::Mode::Authentication;
            state.active = true;
            const int current = state.nextId;
            state.createTarget();
            created(browser, stale, "stale-created");
            check(capture.closes("old") == 0 && capture.closes("stale-created") == 1
                  && state.target.isEmpty() && state.pages.isEmpty(),
                  "rapid replacement closes stale created targets without closing the anchor");
            int callbacks = 0;
            QObject::connect(&browser, &BrowserAuth::callbackReady, [&] { ++callbacks; });
            callback(browser);
            check(callbacks == 0 && state.active, "retained-window callbacks cannot authorize the new round");
            state.handleMessage({{"method", "Target.detachedFromTarget"},
                {"params", QJsonObject{{"sessionId", "old-session"}}}});
            check(capture.closes("old") == 0 && state.retainedTarget == "old" && state.active,
                  "retained CDP detachment does not close the last window or fail replacement");
            created(browser, current, "replacement");
            check(capture.closes("old") == 1 && capture.closes("replacement") == 0
                  && state.target == "replacement" && state.retainedTarget.isEmpty(),
                  "rapid replacement closes its anchor exactly once after adoption");
            state.handleMessage({{"method", "Target.targetDestroyed"}, {"params", QJsonObject{{"targetId", "old"}}}});
            check(state.active && state.pages.contains("replacement"),
                  "old target destruction cannot cancel the adopted replacement");
        }
        {
            Capture capture;
            BrowserAuth browser;
            if (!ownedRound(browser, capture)) return;
            auto& state = *browser.d;
            state.pages.clear();
            state.target.clear();
            const int earlyReply = state.nextId;
            state.createTarget();
            state.unclaimed.insert("early", "early-session");
            state.cancelRound(true);
            state.active = true;
            created(browser, earlyReply, "early");
            check(capture.closes("early") == 0 && state.retainedTarget == "early" && state.pages.isEmpty(),
                  "late create reply cannot prematurely close its retained attached window");
            const int replacement = state.nextId;
            state.createTarget();
            created(browser, replacement, "next");
            check(capture.closes("early") == 1 && capture.closes("next") == 0 && state.target == "next",
                  "replacement closes its previously unclaimed anchor only after adoption");
        }
        {
            Capture capture;
            BrowserAuth browser;
            if (!ownedRound(browser, capture)) return;
            auto& state = *browser.d;
            state.cancelRound(true);
            state.active = true;
            const int late = state.nextId;
            state.createTarget();
            state.unclaimed.insert("inflight-replacement", "new-session");
            browser.cancel();
            check(capture.closes("old") == 1 && capture.closes("inflight-replacement") == 1
                  && !state.active && state.retainedTarget.isEmpty(),
                  "explicit cancellation closes both retained and attached replacement windows immediately");
            created(browser, late, "late-created");
            check(capture.closes("late-created") == 1 && !state.active && state.pages.isEmpty(),
                  "late create reply after cancellation closes its window without reviving the round");
        }
        {
            Capture capture;
            BrowserAuth browser;
            if (!ownedRound(browser, capture)) return;
            auto& state = *browser.d;
            state.cancelRound(true);
            state.active = true;
            int failures = 0;
            QObject::connect(&browser, &BrowserAuth::failed, [&] { ++failures; });
            const int id = state.nextId;
            state.createTarget();
            created(browser, id, {});
            check(capture.closes("old") == 1 && failures == 1 && !state.active,
                  "failed replacement closes its retained window");
        }
        {
            Capture capture;
            BrowserAuth browser;
            if (!ownedRound(browser, capture)) return;
            browser.d->cancelRound(true);
            int failures = 0;
            QObject::connect(&browser, &BrowserAuth::failed, [&] { ++failures; });
            browser.begin(QUrl("http://127.0.0.1/unsupported"));
            check(capture.closes("old") == 1 && failures == 1 && !browser.d->active,
                  "invalid authorization closes rather than retaining the old window");
        }
    }
};

#include "browserauth_public_tests.h"

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc >= 4 && QByteArray(argv[1]) == "--synthetic-chromium")
        return syntheticChromium(application.arguments());
    if (argc == 2 && QByteArray(argv[1]) == "--idle-transport")
        return application.exec();
    int failures = 0, assertions = 0;
    const auto check = [&failures, &assertions](bool pass, const char* name) {
        ++assertions;
        if (!pass) {
            ++failures;
            std::fprintf(stderr, "FAIL: %s\n", name);
        }
    };
    if (application.arguments().contains(QStringLiteral("--cache-only"))) {
        BrowserAuthTestAccess::signInCache(check);
        std::printf("Browser cache assertions: %d; failures: %d.\n", assertions, failures);
        return failures ? 1 : 0;
    }
    QUrl request(QStringLiteral("https://login.microsoftonline.com/common/oauth2/v2.0/authorize"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("client_id"), QStringLiteral("a85cf173-4192-42f8-81fa-777a763e6e2c"));
    query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    query.addQueryItem(QStringLiteral("redirect_uri"),
                       QStringLiteral("https://login.microsoftonline.com/common/oauth2/nativeclient"));
    query.addQueryItem("scope", "https://www.wvd.microsoft.com/.default openid profile offline_access");
    query.addQueryItem("code_challenge", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    query.addQueryItem("code_challenge_method", "S256");
    query.addQueryItem("state", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    request.setQuery(query);
    check(BrowserAuthTestAccess::authorize(request), "modern seven-field authorization");

    auto altered = request;
    altered.setHost(QStringLiteral("login.microsoftonline.com.attacker.invalid"));
    check(!BrowserAuthTestAccess::authorize(altered), "authorization host suffix rejected");
    altered = request;
    altered.setUserName(QStringLiteral("untrusted"));
    check(!BrowserAuthTestAccess::authorize(altered), "authorization userinfo rejected");
    altered = request;
    altered.setPort(444);
    check(!BrowserAuthTestAccess::authorize(altered), "authorization nondefault port rejected");
    altered = request;
    altered.setScheme(QStringLiteral("http"));
    check(!BrowserAuthTestAccess::authorize(altered), "authorization HTTP rejected");
    altered = request;
    altered.setPath(QStringLiteral("/common/oauth2/v2.0/authorize/extra"));
    check(!BrowserAuthTestAccess::authorize(altered), "authorization path suffix rejected");
    altered = request;
    altered.setFragment(QStringLiteral("code=synthetic"));
    check(!BrowserAuthTestAccess::authorize(altered), "authorization fragment rejected");

    auto changedQuery = query;
    changedQuery.removeAllQueryItems(QStringLiteral("redirect_uri"));
    changedQuery.addQueryItem(QStringLiteral("redirect_uri"),
                             QStringLiteral("https://attacker.invalid/common/oauth2/nativeclient"));
    altered = request;
    altered.setQuery(changedQuery);
    check(!BrowserAuthTestAccess::authorize(altered), "redirect origin rejected");
    changedQuery = query;
    changedQuery.addQueryItem(QStringLiteral("redirect_uri"),
                             QStringLiteral("https://login.microsoftonline.com/common/oauth2/nativeclient"));
    altered.setQuery(changedQuery);
    check(!BrowserAuthTestAccess::authorize(altered), "duplicate redirect rejected");
    changedQuery = query;
    changedQuery.removeAllQueryItems(QStringLiteral("response_type"));
    changedQuery.addQueryItem(QStringLiteral("response_type"), QStringLiteral("token"));
    altered.setQuery(changedQuery);
    check(!BrowserAuthTestAccess::authorize(altered), "implicit-token flow rejected");
    changedQuery = query;
    changedQuery.removeAllQueryItems(QStringLiteral("redirect_uri"));
    changedQuery.addQueryItem(QStringLiteral("redirect_uri"),
                             QStringLiteral("https://login.microsoftonline.com/common/oauth2/nativeclient?extra=1"));
    altered.setQuery(changedQuery);
    check(!BrowserAuthTestAccess::authorize(altered), "redirect query rejected");
    altered.setQuery(request.query(QUrl::FullyEncoded) + QStringLiteral("&state=a+b%2Bc"), QUrl::StrictMode);
    check(!BrowserAuthTestAccess::stateDecoded(altered, QStringLiteral("a b+c")),
          "OAuth form-encoded state decoded once");
    altered.setQuery(altered.query(QUrl::FullyEncoded) + QStringLiteral("&state=second"), QUrl::StrictMode);
    check(!BrowserAuthTestAccess::authorize(altered), "duplicate request state rejected");

    const QString endpoint = QStringLiteral("https://login.microsoftonline.com/common/oauth2/nativeclient");
    check(!BrowserAuthTestAccess::code(QUrl(endpoint + QStringLiteral("?code=synthetic-code&session_state=test"))),
          "nativeclient code recognized");
    check(!BrowserAuthTestAccess::code(QUrl(endpoint + QStringLiteral("?code=synthetic-code&state=a%20b%2Bc")),
                                     QStringLiteral("a b+c"), true), "exact decoded state recognized");
    check(!BrowserAuthTestAccess::code(QUrl(endpoint + QStringLiteral("?code=synthetic-code&state=wrong")),
                                      QStringLiteral("expected"), true), "mismatched state rejected");
    check(!BrowserAuthTestAccess::code(QUrl(endpoint + QStringLiteral("?code=synthetic-code")),
                                      QStringLiteral("expected"), true), "missing required state rejected");
    check(!BrowserAuthTestAccess::code(QUrl(endpoint + QStringLiteral("?code=one&code=two"))),
          "duplicate codes rejected");
    check(!BrowserAuthTestAccess::code(QUrl(endpoint + QStringLiteral("?code=one&state=a&state=a"))),
          "duplicate callback states rejected even without request state");
    check(!BrowserAuthTestAccess::code(QUrl(endpoint + QStringLiteral("?code="))), "empty code rejected");
    check(!BrowserAuthTestAccess::code(QUrl(endpoint + QStringLiteral("?code=one&error=denied"))),
          "mixed error and code rejected");
    check(!BrowserAuthTestAccess::error(QUrl(endpoint + QStringLiteral("?error=access_denied"))),
          "OAuth error recognized");
    check(!BrowserAuthTestAccess::error(QUrl(endpoint + QStringLiteral("?error=one&error=two"))),
          "duplicate OAuth errors rejected");
    check(!BrowserAuthTestAccess::code(QUrl(endpoint + QStringLiteral(".evil?code=synthetic-code"))),
          "callback path suffix rejected");
    check(!BrowserAuthTestAccess::code(QUrl(QStringLiteral(
        "https://login.microsoftonline.com.attacker.invalid/common/oauth2/nativeclient?code=synthetic-code"))),
        "callback host suffix rejected");
    check(!BrowserAuthTestAccess::code(QUrl(QStringLiteral(
        "https://login.microsoftonline.com:444/common/oauth2/nativeclient?code=synthetic-code"))),
        "callback nondefault port rejected");
    check(!BrowserAuthTestAccess::code(QUrl(endpoint + QStringLiteral("?code=synthetic-code#fragment"))),
          "callback fragment rejected");
    check(!BrowserAuthTestAccess::code(QUrl(endpoint + QStringLiteral("?code=") + QString(65536, QLatin1Char('x')))),
          "oversized callback rejected");
    BrowserAuthTestAccess::signInCache(check);
    BrowserAuthTestAccess::acquisition(check);
    BrowserAuthTestAccess::handoff(check);
    publicBrowserTests(check);

    {
        // An anchor that cannot confirm its setup must fail the start, so no group
        // without an anchor is ever published as a started browser.
        QProcess process;
        QProcess* const child = &process;
        const pid_t parent = ::getpid();
        process.setChildProcessModifier([child, parent] { prepareBrowserChild(child, -1, -1, -1, parent); });
        bool started = false;
        QObject::connect(&process, &QProcess::started, [&started] { started = true; });
        process.start(QCoreApplication::applicationFilePath(), {"--idle-transport"});
        check(!process.waitForStarted(3000) && process.error() == QProcess::FailedToStart && !started,
              "browser anchor setup failure is reported as a failed start");
    }
    {
        const QString page = QUrl::fromPercentEncoding(completionPage().toLatin1());
        check(page.startsWith("data:text/html;charset=utf-8,") && page.contains("<div class=logo><svg")
              && page.contains("<title id=\"title\">OMAWIN365</title>")
              && !page.contains("src=") && !page.contains("href=") && !page.contains("url("),
              "sign-in return page shows the inlined OMAWIN365 logo and loads nothing external");
    }
    check(unreservedGroupSignals == 0, "browser process-group signals never reach an unreserved group number");
    std::printf("BrowserAuth assertions: %d; failures: %d.\n", assertions, failures);
    if (failures == 0)
        std::puts("Browser authorization and acquisition boundary checks passed.");
    return failures == 0 ? 0 : 1;
}
