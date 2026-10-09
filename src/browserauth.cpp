#include "oauthcontract.h"
#include "browserauth.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQueue>
#include <QRegularExpression>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QTimer>

#include <cerrno>
#include <climits>
#include <csignal>
#include <fcntl.h>
#include <functional>
#include <linux/close_range.h>
#include <linux/magic.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace {
constexpr qsizetype MaxFrameBytes = 1024 * 1024;
constexpr qsizetype MaxQueuedBytes = 1024 * 1024;
constexpr qint64 MaxDownloadBytes = 1024 * 1024;
constexpr int MaxPendingCommands = 64;
constexpr int MaxTargets = 16;
constexpr int MaxFrames = 128;
constexpr qint64 CommandTimeoutMs = 30000;
bool acquisitionPage(const QUrl& url)
{
    return url.isValid() && url.scheme() == QStringLiteral("https") && url.userInfo().isEmpty()
        && (url.port(-1) == -1 || url.port() == 443)
        && url.host() == QStringLiteral("client.wvd.microsoft.com")
        && url.path(QUrl::FullyEncoded) == QStringLiteral("/arm/webclient/index.html");
}

// Chromium reports the owned main frame even when a foreign child frame navigates
// it to a download, so the frame cannot attribute the source; the URL can.
bool portalDownloadSource(const QUrl& url)
{
    const QUrl source = url.scheme() == QStringLiteral("blob")
        ? QUrl(url.path(QUrl::FullyEncoded), QUrl::StrictMode) : url;
    return source.isValid() && source.scheme() == QStringLiteral("https") && source.userInfo().isEmpty()
        && (source.port(-1) == -1 || source.port() == 443)
        && source.host() == QStringLiteral("client.wvd.microsoft.com");
}

bool downloadGuidValid(const QString& guid)
{
    static const QRegularExpression pattern(QStringLiteral(
        "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    return pattern.match(guid).hasMatch();
}

bool privateMemoryDirectory(QString path)
{
    if (path.isEmpty() || !QDir::isAbsolutePath(path))
        return false;
    // A trailing slash must not let open() follow a final symlink despite O_NOFOLLOW.
    while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    const QByteArray bytes = QFile::encodeName(path);
    const int fd = ::open(bytes.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat info{};
    struct statfs filesystem{};
    const bool valid = ::fstat(fd, &info) == 0 && S_ISDIR(info.st_mode)
        && info.st_uid == ::getuid() && (info.st_mode & 0777) == 0700
        && ::fstatfs(fd, &filesystem) == 0 && filesystem.f_type == TMPFS_MAGIC;
    ::close(fd);
    return valid;
}

QString privateRuntimePath()
{
    const QString path = qEnvironmentVariable("XDG_RUNTIME_DIR");
    return privateMemoryDirectory(path) ? path : QString();
}

bool createPrivateMemoryDirectory(const QString& path)
{
    return ::mkdir(QFile::encodeName(path).constData(), 0700) == 0
        && privateMemoryDirectory(path);
}

void closeFd(int& fd)
{
    if (fd >= 0)
        ::close(fd);
    fd = -1;
}

// Keep sources away from fd 3/4 so the child's two dup2 calls cannot clobber one another.
bool privatePipe(int* reader, int* writer)
{
    int pipeFds[2];
    if (::pipe2(pipeFds, O_CLOEXEC) != 0)
        return false;
    *reader = ::fcntl(pipeFds[0], F_DUPFD_CLOEXEC, 10);
    *writer = ::fcntl(pipeFds[1], F_DUPFD_CLOEXEC, 10);
    ::close(pipeFds[0]);
    ::close(pipeFds[1]);
    if (*reader < 0 || *writer < 0) {
        closeFd(*reader);
        closeFd(*writer);
        return false;
    }
    return true;
}

bool nonblocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// Runs in the forked browser child before exec, so async-signal-safe calls only
// (_Fork, not fork: no atfork handlers). Starts a new session and an anchor in its
// process group: Qt reaps the browser itself, so without a member we own, the group
// number could be recycled before our final SIGKILL. The anchor keeps every catchable
// signal blocked and ends only on that SIGKILL or when the app closes holdFd's write
// end. Exec happens only after the anchor confirms its setup; failures are reported
// through QProcess so a group without an anchor is never published as started.
// inputFd < 0 skips the CDP descriptors (test transports).
void prepareBrowserChild(QProcess* process, int inputFd, int outputFd, int holdFd, pid_t parentPid)
{
    sigset_t all, previous;
    ::sigfillset(&all);
    if (::sigprocmask(SIG_BLOCK, &all, &previous) != 0 || ::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0
        || ::getppid() != parentPid || ::setsid() < 0)
        process->failChildProcessModifier("browser session setup", errno);
    int ready[2];
    if (::pipe2(ready, O_CLOEXEC) != 0)
        process->failChildProcessModifier("browser anchor setup", errno);
    const pid_t anchor = ::_Fork();
    if (anchor < 0)
        process->failChildProcessModifier("browser anchor setup", errno);
    if (anchor == 0) {
        // Only fd 0 (hold) and fd 1 (ready) survive; no copy of any app or Qt pipe.
        if (::dup2(holdFd, 0) < 0 || ::dup2(ready[1], 1) < 0
            || ::syscall(SYS_close_range, 2u, ~0u, 0u) != 0 || ::write(1, "r", 1) != 1)
            ::_exit(0);
        ::close(1);
        char byte;
        while (::read(0, &byte, 1) < 0 && errno == EINTR) {}
        ::_exit(0);
    }
    ::close(ready[1]);
    char confirmed = 0;
    ssize_t got;
    while ((got = ::read(ready[0], &confirmed, 1)) < 0 && errno == EINTR) {}
    if (got != 1)
        process->failChildProcessModifier("browser anchor setup", 0);
    ::close(ready[0]);
    ::close(holdFd);
    if ((inputFd >= 0 && (::dup2(inputFd, 3) < 0 || ::dup2(outputFd, 4) < 0
                          || ::syscall(SYS_close_range, 5u, ~0u, CLOSE_RANGE_CLOEXEC) < 0))
        || ::sigprocmask(SIG_SETMASK, &previous, nullptr) != 0)
        process->failChildProcessModifier("browser descriptor setup", errno);
}

// Do not change the GUI's signal disposition or consume a pre-existing SIGPIPE.
ssize_t pipeWrite(int fd, const char* bytes, size_t count)
{
    sigset_t blocked, previous, pending;
    ::sigemptyset(&blocked);
    ::sigaddset(&blocked, SIGPIPE);
    if (::pthread_sigmask(SIG_BLOCK, &blocked, &previous) != 0) {
        errno = EIO;
        return -1;
    }
    ::sigpending(&pending);
    const bool alreadyPending = ::sigismember(&pending, SIGPIPE) == 1;
    const ssize_t result = ::write(fd, bytes, count);
    const int savedError = errno;
    if (result < 0 && savedError == EPIPE && !alreadyPending) {
        const timespec immediate{0, 0};
        while (::sigtimedwait(&blocked, nullptr, &immediate) < 0 && errno == EINTR) {}
    }
    ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    errno = savedError;
    return result;
}

QString completionPage()
{
    // The launcher's own logo, inlined from the compiled-in resource: nothing is fetched.
    QString logo;
    QFile header(QStringLiteral(":/icons/header.svg"));
    if (header.open(QIODevice::ReadOnly))
        logo = QStringLiteral("<div class=logo>") + QString::fromUtf8(header.readAll()) + QStringLiteral("</div>");
    return QStringLiteral("data:text/html;charset=utf-8,") + QString::fromLatin1(QUrl::toPercentEncoding(
        QStringLiteral("<!doctype html><meta charset=utf-8><title>OMAWIN365</title>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<style>body{background:#181818;color:#eee;font:16px monospace;"
        "margin:clamp(1em,4vw,3em);max-width:65ch;line-height:1.6}"
        ".logo svg{display:block;width:min(100%,22em);height:auto;margin-bottom:1.5em}"
        ".logo .t{fill:#eee}"
        "h1{font-size:1.5em;line-height:1.3}</style>") + logo + QStringLiteral("<h1>Sign-in returned to OMAWIN365</h1>"
        "<p>This sign-in step is complete. Your desktop connection may still be finishing.</p>"
        "<p><strong>Another Microsoft sign-in window may open.</strong> "
        "A connection can require several sign-in steps. Follow each prompt until your desktop opens.</p>"
        "<p>Keep this window open: OMAWIN365 closes it when your desktop appears, and closing it earlier cancels the connection. "
        "If another sign-in step is needed, OMAWIN365 replaces this window automatically. "
        "You do not need to press Connect again.</p>"
        "<p>If sign-in keeps repeating without progress, check the connection status in OMAWIN365.</p>")));
}
}

bool BrowserAuth::validateAuthorization(const QUrl& url, std::optional<QString>* state)
{
    const auto contract = OAuthContract::request(url);
    if (!contract) return false;
    *state = contract->state;
    return true;
}

BrowserAuth::CallbackKind BrowserAuth::classifyCallback(const QUrl& url, const std::optional<QString>& state)
{
    switch (OAuthContract::reply(url, state.value_or(QString())).kind) {
    case OAuthContract::Kind::NotCallback: return CallbackKind::NotCallback;
    case OAuthContract::Kind::Code: return CallbackKind::Code;
    case OAuthContract::Kind::Error: return CallbackKind::OAuthError;
    case OAuthContract::Kind::Invalid: return CallbackKind::Invalid;
    }
    return CallbackKind::Invalid;
}

struct BrowserAuth::Private {
    enum class Mode { Authentication, Acquisition };
    struct AuthRequest {
        QUrl authorization;
        std::optional<OAuthContract::Request> contract;
    };
    struct Portal {
        QString script; // Browser-lifetime cache; document resets retain it.
        QString requestedId;
        QJsonArray resources;
        quint64 generation = 1;
        int context = 0;
        qint64 deadline = 0;
        bool busy = false;
        bool choiceOffered = false; // Round-lifetime choice requirement, retained across documents.

        bool resetDocument()
        {
            ++generation;
            context = 0;
            deadline = 0;
            busy = false;
            requestedId.clear();
            const bool hadResources = !resources.isEmpty();
            resources = {};
            return hadResources;
        }
    };
    struct Download {
        // Clicked owns a validated resource name; Receiving also owns a validated GUID.
        enum class Phase { Unarmed, Clicked, Receiving };
        Phase phase = Phase::Unarmed;
        std::unique_ptr<QTemporaryDir> directory;
        QString guid;
        QString displayName;

        bool armed() const { return phase != Phase::Unarmed; }
        void resetSelection()
        {
            // Only while unarmed or after retirement; never discard a receiving GUID.
            phase = Phase::Unarmed;
            guid.clear();
            displayName.clear();
        }
    };
    struct Pending {
        quint64 round;
        QString method;
        QString session;
        qint64 deadline;
        std::function<void(const QJsonObject&)> ready;
        quint64 portalGeneration = 0;
    };
    struct Page {
        quint64 round;
        QString session;
        QString mainFrame;
        QHash<QString, QUrl> frames;
        bool preparing = false;
    };

    BrowserAuth* owner;
    // Use Arch's real Chromium binary, never the user-configurable flags launcher.
    QString browserExecutable = QStringLiteral("/usr/lib/chromium/chromium");
    QStringList browserArgumentPrefix;
    QProcess process;
    QTimer watchdog;
    QTimer portalPoll;
    QElapsedTimer clock;
    std::unique_ptr<QTemporaryDir> profile;
    std::unique_ptr<QSocketNotifier> reader;
    std::unique_ptr<QSocketNotifier> writer;
    int readFd = -1;
    int writeFd = -1;
    int childRead = -1;
    int childWrite = -1;
    QList<int> startupGuards;
    pid_t group = -1;
    // Qt reaps Chromium itself, so the group number would be free once its last member
    // exits. An anchor process in the group holds it until our final SIGKILL; it ignores
    // SIGTERM and exits early only when this write end closes (including app death).
    int anchorRead = -1;
    int anchorWrite = -1;
    bool stopping = false;
    bool diagnosticsEnabled = false;
    bool contextCreated = false;
    bool active = false;
    bool creating = false;
    Mode mode = Mode::Authentication;
    quint64 round = 0;
    int nextId = 1;
    qint64 startDeadline = 0;
    QHash<int, Pending> pending;
    QHash<QString, Page> pages;
    QHash<QString, QString> unclaimed;
    QString retainedTarget;
    QString retainedSession;
    QByteArray input;
    QQueue<QByteArray> output;
    qsizetype outputOffset = 0;
    qsizetype queuedBytes = 0;
    AuthRequest auth;
    QString target;
    Portal portal;
    Download download;

    explicit Private(BrowserAuth* q) : owner(q)
    {
        clock.start();
        portalPoll.setInterval(750);
        QObject::connect(&portalPoll, &QTimer::timeout, owner, [this] { pollPortal(); });
        watchdog.setInterval(1000);
        QObject::connect(&watchdog, &QTimer::timeout, owner, [this] {
            if (startDeadline != 0 && clock.elapsed() > startDeadline) {
                transportFailure(QStringLiteral("The sign-in browser did not start in time."));
                return;
            }
            for (auto it = pending.begin(); it != pending.end();) {
                if (it->portalGeneration && it->portalGeneration != portal.generation) {
                    it = pending.erase(it);
                    continue;
                }
                if (clock.elapsed() <= it->deadline) {
                    ++it;
                } else if (active && it->round == round
                           && (it->session.isEmpty() || pageForSession(it->session))) {
                    transportFailure(QStringLiteral("The sign-in browser stopped responding."));
                    return;
                } else {
                    it = pending.erase(it);
                }
            }
        });
        QObject::connect(&process, &QProcess::started, owner, [this] {
            startDeadline = 0;
            group = static_cast<pid_t>(process.processId());
            closeFd(childRead);
            closeFd(childWrite);
            closeFd(anchorRead);
            releaseStartupGuards();
            diagnostic(mode == Mode::Authentication ? QStringLiteral("browser-started")
                                                    : QStringLiteral("browser-acquisition-started"));
            if (active)
                prepareFlow();
        });
        QObject::connect(&process, &QProcess::errorOccurred, owner, [this](QProcess::ProcessError error) {
            if (!stopping && error == QProcess::FailedToStart)
                transportFailure(QStringLiteral("The installed Chromium browser could not be started."));
        });
        QObject::connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), owner,
                         [this](int, QProcess::ExitStatus) {
            if (!stopping) {
                // Release the group before any signal emission can re-enter BrowserAuth.
                if (group > 0)
                    ::kill(-group, SIGKILL);
                group = -1;
                closeFd(anchorWrite);
            }
            diagnostic(mode == Mode::Authentication ? QStringLiteral("browser-exited")
                                                    : QStringLiteral("browser-acquisition-exited"));
            if (stopping)
                return;
            const bool wasActive = active;
            active = false;
            ++round;
            clearRound();
            cleanupTransport();
            disposeContext();
            profile.reset();
            if (wasActive)
                emit owner->closed();
        });
    }

    ~Private() { shutdownBrowser(); }

    void diagnostic(const QString& event)
    {
        if (diagnosticsEnabled)
            emit owner->diagnosticEvent(event);
    }

    void disposeContext()
    {
        if (!contextCreated)
            return;
        contextCreated = false;
        diagnostic(mode == Mode::Authentication ? QStringLiteral("browser-context-disposed")
                                                : QStringLiteral("browser-acquisition-context-disposed"));
    }

    void retireDownload()
    {
        if (download.directory) {
            // Canceled Chromium writes may still be in flight. The enclosing profile
            // owns this directory until browser exit; do not remove beneath a writer.
            download.directory->setAutoRemove(false);
        }
        download = {};
    }

    void clearRound()
    {
        auth = {};
        creating = false;
        target.clear();
        pages.clear();
        unclaimed.clear();
        retireDownload();
        portalPoll.stop();
        portal.choiceOffered = false;
        resetPortal();
    }

    bool reserveDebugDescriptors()
    {
        // QProcess's exec-status pipe must never occupy fd 3/4. Reserving vacant
        // low descriptors in the parent prevents our child dup2 from replacing it.
        for (;;) {
            int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (fd < 0)
                return false;
            if (fd > 4) {
                closeFd(fd);
                return true;
            }
            startupGuards.append(fd);
        }
    }

    void releaseStartupGuards()
    {
        for (int& fd : startupGuards)
            closeFd(fd);
        startupGuards.clear();
    }

    void cleanupTransport()
    {
        reader.reset();
        writer.reset();
        closeFd(readFd);
        closeFd(writeFd);
        closeFd(childRead);
        closeFd(childWrite);
        releaseStartupGuards();
        pending.clear();
        retainedTarget.clear();
        retainedSession.clear();
        input.clear();
        output.clear();
        outputOffset = 0;
        queuedBytes = 0;
        startDeadline = 0;
        watchdog.stop();
    }

    void shutdownBrowser()
    {
        stopping = true;
        active = false;
        clearRound();
        if (process.state() != QProcess::NotRunning) {
            // group is set only once Qt reports a started browser, whose anchor then exists;
            // before that, signal only the unreaped leader via QProcess.
            if (group > 0)
                ::kill(-group, SIGTERM);
            process.terminate();
            if (!process.waitForFinished(1500)) {
                if (group > 0)
                    ::kill(-group, SIGKILL);
                group = -1; // The anchor is gone with that SIGKILL; never signal the number again.
                process.kill();
                process.waitForFinished(-1);
            }
        }
        if (group > 0)
            ::kill(-group, SIGKILL);
        group = -1;
        closeFd(anchorWrite);
        closeFd(anchorRead);
        cleanupTransport();
        // Delete the private profile only after Chromium has exited.
        disposeContext();
        profile.reset();
        stopping = false;
    }

    // The browser exited (normally because the user closed it): a cancellation.
    void transportClosed()
    {
        const bool notify = active;
        ++round;
        shutdownBrowser();
        if (notify)
            emit owner->closed();
    }

    void transportFailure(const QString& message)
    {
        const bool notify = active;
        ++round;
        shutdownBrowser();
        if (notify)
            emit owner->failed(message);
    }

    void closeOneTarget(const QString& id)
    {
        if (!id.isEmpty() && process.state() == QProcess::Running)
            send(QStringLiteral("Target.closeTarget"), {{QStringLiteral("targetId"), id}}, {}, {});
    }
    void closeRetainedTarget()
    {
        const QString id = std::exchange(retainedTarget, QString());
        retainedSession.clear();
        closeOneTarget(id);
    }


    void cancelRound(bool replacing = false)
    {
        if (active)
            diagnostic(mode == Mode::Authentication ? QStringLiteral("browser-auth-round-cancel")
                                                    : QStringLiteral("browser-acquisition-round-cancel"));
        active = false;
        ++round;
        // Snapshot keys: pipe failures can clear the maps while sending cleanup commands.
        const auto ids = pages.keys();
        const auto otherIds = unclaimed.keys();
        const QString guid = download.guid;
        // Retain at most one old window until its replacement exists. Closing
        // Chromium's last window clears session cookies even while CDP stays alive.
        if (replacing && retainedTarget.isEmpty()) {
            if (pages.contains(target)) {
                retainedTarget = target;
                retainedSession = pages.value(target).session;
            } else if (!pages.isEmpty()) {
                retainedTarget = pages.constBegin().key();
                retainedSession = pages.constBegin()->session;
            } else if (!unclaimed.isEmpty()) {
                retainedTarget = unclaimed.constBegin().key();
                retainedSession = unclaimed.constBegin().value();
            }
        }
        const QString keep = replacing ? retainedTarget : QString();
        const QString retired = replacing ? QString() : std::exchange(retainedTarget, QString());
        if (!replacing)
            retainedSession.clear();
        clearRound();
        if (!guid.isEmpty())
            send(QStringLiteral("Browser.cancelDownload"), {{QStringLiteral("guid"), guid}}, {}, {});
        if (process.state() == QProcess::Running)
            send(QStringLiteral("Browser.setDownloadBehavior"), {{QStringLiteral("behavior"), QStringLiteral("deny")}}, {}, {});
        closeOneTarget(retired);
        for (const QString& id : ids)
            if (id != keep)
                closeOneTarget(id);
        for (const QString& id : otherIds)
            if (id != keep)
                closeOneTarget(id);
    }

    // The user closed the sign-in window: a cancellation, not a failure.
    void closeRound()
    {
        if (!active)
            return;
        cancelRound();
        emit owner->closed();
    }

    void failRound(const QString& message)
    {
        if (!active)
            return;
        cancelRound();
        emit owner->failed(message);
    }

    void startBrowser()
    {
        const QString executable = browserExecutable;
        const QString runtime = privateRuntimePath();
        if (runtime.isEmpty()) {
            failRound(QStringLiteral("A private, user-owned tmpfs XDG runtime directory is required for browser sign-in."));
            return;
        }
        if (!QFileInfo(executable).isExecutable()) {
            failRound(QStringLiteral("Chromium is required for Microsoft sign-in. Install the Chromium package."));
            return;
        }
        profile = std::make_unique<QTemporaryDir>(runtime + QStringLiteral("/omawin365-auth-XXXXXX"));
        const QString cache = profile->path() + QStringLiteral("/cache");
        const QString temporary = profile->path() + QStringLiteral("/tmp");
        if (!profile->isValid() || !privateMemoryDirectory(profile->path())
            || !createPrivateMemoryDirectory(cache) || !createPrivateMemoryDirectory(temporary)
            || !privatePipe(&childRead, &writeFd) || !privatePipe(&readFd, &childWrite)
            || !privatePipe(&anchorRead, &anchorWrite)
            || !nonblocking(readFd) || !nonblocking(writeFd)) {
            transportFailure(QStringLiteral("A private sign-in browser could not be prepared."));
            return;
        }
        reader = std::make_unique<QSocketNotifier>(readFd, QSocketNotifier::Read, owner);
        writer = std::make_unique<QSocketNotifier>(writeFd, QSocketNotifier::Write, owner);
        writer->setEnabled(false);
        QObject::connect(reader.get(), &QSocketNotifier::activated, owner, [this] { readFrames(); });
        QObject::connect(writer.get(), &QSocketNotifier::activated, owner, [this] { flushWrites(); });
        const int inputFd = childRead;
        const int outputFd = childWrite;
        const int holdFd = anchorRead;
        const pid_t parentPid = ::getpid();
        QProcess* const child = &process;
        process.setChildProcessModifier([child, inputFd, outputFd, holdFd, parentPid] {
            prepareBrowserChild(child, inputFd, outputFd, holdFd, parentPid);
            ::close(inputFd);
            ::close(outputFd);
        });
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("XDG_CACHE_HOME"), cache);
        environment.insert(QStringLiteral("TMPDIR"), temporary);
        environment.insert(QStringLiteral("TMP"), temporary);
        environment.insert(QStringLiteral("TEMP"), temporary);
        // Crashpad otherwise keeps its database (and any dumps of sign-in memory)
        // in the user's real ~/.config/chromium, outside the private stores.
        environment.insert(QStringLiteral("BREAKPAD_DUMP_LOCATION"), temporary);
        environment.remove(QStringLiteral("CHROMIUM_FLAGS"));
        environment.remove(QStringLiteral("CHROMIUM_USER_FLAGS"));
        environment.remove(QStringLiteral("CHROME_LOG_FILE"));
        environment.remove(QStringLiteral("SSLKEYLOGFILE"));
        environment.remove(QStringLiteral("CHROME_DEBUG_LOG_FILE"));
        environment.remove(QStringLiteral("CHROME_LOG_FD"));
        environment.remove(QStringLiteral("CHROME_ENABLE_LOGGING"));
        environment.remove(QStringLiteral("CHROME_LOG_LEVEL"));
        process.setProcessEnvironment(environment);
        process.setStandardOutputFile(QProcess::nullDevice());
        process.setStandardErrorFile(QProcess::nullDevice());
        if (!reserveDebugDescriptors()) {
            transportFailure(QStringLiteral("Private browser descriptors could not be reserved."));
            return;
        }
        startDeadline = clock.elapsed() + CommandTimeoutMs;
        watchdog.start();
        process.start(executable, browserArgumentPrefix + QStringList{
            QStringLiteral("--remote-debugging-pipe"),
            QStringLiteral("--user-data-dir=") + profile->path(),
            QStringLiteral("--disk-cache-dir=") + cache,
            // Chromium's password-manager saving/filling gate rejects automation.
            // password-store below selects only the backend; it is not that prohibition.
            QStringLiteral("--enable-automation"),
            QStringLiteral("--no-first-run"),
            QStringLiteral("--no-default-browser-check"),
            QStringLiteral("--no-startup-window"),
            QStringLiteral("--disable-background-networking"),
            QStringLiteral("--disable-breakpad"),
            QStringLiteral("--disable-crash-reporter"),
            QStringLiteral("--disable-component-update"),
            QStringLiteral("--disable-sync"),
            QStringLiteral("--no-pings"),
            QStringLiteral("--disable-domain-reliability"),
            // --disable-background-networking no longer stops these Chromium services
            // (push check-in, browser account checks, network time, model and dictionary
            // downloads). These hosts never serve sign-in pages, so make them unresolvable.
            // accounts.google.com and www.google.com stay reachable: Windows 365 external
            // identities and IdPs such as Okta can sign in with Google or use reCAPTCHA.
            QStringLiteral("--host-resolver-rules=MAP android.clients.google.com ~NOTFOUND, "
                           "MAP mtalk.google.com ~NOTFOUND, MAP clients2.google.com ~NOTFOUND, "
                           "MAP *.gvt1.com ~NOTFOUND, "
                           "MAP update.googleapis.com ~NOTFOUND, "
                           "MAP optimizationguide-pa.googleapis.com ~NOTFOUND"),
            QStringLiteral("--password-store=basic")
        });
    }

    void send(const QString& method, const QJsonObject& params, const QString& session,
              std::function<void(const QJsonObject&)> ready, quint64 generation = 0)
    {
        if (writeFd < 0)
            return;
        if (nextId == INT_MAX || (ready && pending.size() >= MaxPendingCommands)) {
            transportFailure(QStringLiteral("The sign-in browser protocol exceeded its safety limit."));
            return;
        }
        const int id = nextId++;
        QJsonObject command{{QStringLiteral("id"), id}, {QStringLiteral("method"), method},
                            {QStringLiteral("params"), params}};
        if (!session.isEmpty())
            command.insert(QStringLiteral("sessionId"), session);
        QByteArray frame = QJsonDocument(command).toJson(QJsonDocument::Compact);
        frame.append('\0');
        if (frame.size() > MaxFrameBytes || queuedBytes + frame.size() > MaxQueuedBytes) {
            transportFailure(QStringLiteral("The sign-in browser protocol exceeded its safety limit."));
            return;
        }
        if (ready)
            pending.insert(id, Pending{round, method, session, clock.elapsed() + CommandTimeoutMs,
                                       std::move(ready), generation});
        queuedBytes += frame.size();
        output.enqueue(std::move(frame));
        flushWrites();
    }

    void flushWrites()
    {
        while (!output.isEmpty() && writeFd >= 0) {
            const QByteArray& frame = output.head();
            const ssize_t count = pipeWrite(writeFd, frame.constData() + outputOffset,
                                            static_cast<size_t>(frame.size() - outputOffset));
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                transportFailure(QStringLiteral("The private connection to the sign-in browser was lost."));
                return;
            }
            if (count == 0)
                break;
            outputOffset += count;
            queuedBytes -= count;
            if (outputOffset == frame.size()) {
                output.dequeue();
                outputOffset = 0;
            }
        }
        if (writer)
            writer->setEnabled(!output.isEmpty());
    }

    void readFrames()
    {
        char bytes[16384];
        qsizetype consumed = 0;
        while (readFd >= 0 && consumed < 256 * 1024) {
            const ssize_t count = ::read(readFd, bytes, sizeof(bytes));
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return;
                transportFailure(QStringLiteral("The private connection to the sign-in browser was lost."));
                return;
            }
            if (count == 0) {
                transportClosed();
                return;
            }
            consumed += count;
            input.append(bytes, count);
            qsizetype delimiter;
            while ((delimiter = input.indexOf('\0')) >= 0) {
                if (delimiter == 0 || delimiter > MaxFrameBytes) {
                    transportFailure(QStringLiteral("The sign-in browser sent an invalid protocol message."));
                    return;
                }
                QJsonParseError error;
                const QJsonDocument document = QJsonDocument::fromJson(input.left(delimiter), &error);
                input.remove(0, delimiter + 1);
                if (error.error != QJsonParseError::NoError || !document.isObject()) {
                    transportFailure(QStringLiteral("The sign-in browser sent an invalid protocol message."));
                    return;
                }
                handleMessage(document.object());
                if (readFd < 0)
                    return;
            }
            if (input.size() > MaxFrameBytes) {
                transportFailure(QStringLiteral("The sign-in browser protocol exceeded its safety limit."));
                return;
            }
        }
    }

    Page* pageForSession(const QString& session)
    {
        if (session.isEmpty())
            return nullptr;
        for (auto it = pages.begin(); it != pages.end(); ++it) {
            if (it->session == session && it->round == round)
                return &it.value();
        }
        return nullptr;
    }

    void handleMessage(const QJsonObject& message)
    {
        if (message.contains(QStringLiteral("id"))) {
            const int id = message.value(QStringLiteral("id")).toInt(-1);
            const auto found = pending.find(id);
            if (found == pending.end())
                return;
            Pending command = std::move(found.value());
            pending.erase(found);
            const QJsonObject result = message.value(QStringLiteral("result")).toObject();
            if (command.round != round || !active) {
                if (command.method == QStringLiteral("Target.createTarget")) {
                    const QString id = result.value(QStringLiteral("targetId")).toString();
                    if (!active || id != retainedTarget)
                        closeOneTarget(id);
                }
                return;
            }
            if (command.portalGeneration && command.portalGeneration != portal.generation)
                return;
            if (!command.session.isEmpty() && !pageForSession(command.session))
                return;
            if (message.contains(QStringLiteral("error"))) {
                failRound(QStringLiteral("The browser could not prepare the current request securely."));
                return;
            }
            command.ready(result);
            return;
        }
        const QString method = message.value(QStringLiteral("method")).toString();
        const QJsonObject params = message.value(QStringLiteral("params")).toObject();
        if (method == QStringLiteral("Target.attachedToTarget")) {
            attachedTarget(params);
            return;
        }
        if (method == QStringLiteral("Target.targetDestroyed")) {
            const QString id = params.value(QStringLiteral("targetId")).toString();
            pages.remove(id);
            unclaimed.remove(id);
            if (id == retainedTarget) {
                retainedTarget.clear();
                retainedSession.clear();
            }
            if (id == target)
                closeRound();
            return;
        }
        if (method == QStringLiteral("Target.detachedFromTarget")) {
            const QString session = params.value(QStringLiteral("sessionId")).toString();
            if (!session.isEmpty() && session == retainedSession) {
                // Detaching CDP does not destroy the owned window. Keep it
                // until replacement so an attachment change cannot end the cookie session.
                retainedSession.clear();
                return;
            }
            if (!session.isEmpty() && pages.value(target).session == session) {
                closeRound();
            } else {
                for (auto it = pages.begin(); it != pages.end();) {
                    if (it->session == session)
                        it = pages.erase(it);
                    else
                        ++it;
                }
            }
            return;
        }
        if (method == QStringLiteral("Browser.downloadWillBegin")) {
            downloadStarted(params);
            return;
        }
        if (method == QStringLiteral("Browser.downloadProgress")) {
            downloadProgress(params);
            return;
        }
        const QString session = message.value(QStringLiteral("sessionId")).toString();
        Page* page = pageForSession(session);
        if (!page)
            return;
        if (method == QStringLiteral("Page.frameNavigated")) {
            const QJsonObject frame = params.value(QStringLiteral("frame")).toObject();
            const QString id = frame.value(QStringLiteral("id")).toString();
            if (!id.isEmpty()) {
                if (page->frames.size() >= MaxFrames && !page->frames.contains(id)) {
                    failRound(QStringLiteral("The browser page exceeded its navigation safety limit."));
                    return;
                }
                page->frames.insert(id, QUrl(frame.value(QStringLiteral("url")).toString(), QUrl::StrictMode));
                if (!frame.contains(QStringLiteral("parentId"))) {
                    page->mainFrame = id;
                    if (mode == Mode::Acquisition && session == pages.value(target).session) {
                        if (download.armed()) {
                            failRound(QStringLiteral("The portal navigated instead of downloading. Import a trusted .rdpw file manually."));
                            return;
                        }
                        resetPortal();
                    }
                }
            }
        } else if (method == QStringLiteral("Page.navigatedWithinDocument")) {
            const QString id = params.value(QStringLiteral("frameId")).toString();
            if (page->frames.contains(id))
                page->frames[id] = QUrl(params.value(QStringLiteral("url")).toString(), QUrl::StrictMode);
            if (mode == Mode::Acquisition && id == page->mainFrame && session == pages.value(target).session) {
                if (download.armed())
                    failRound(QStringLiteral("The portal changed during download. Import a trusted .rdpw file manually."));
                else
                    resetPortal();
            }
        } else if (method == QStringLiteral("Page.frameDetached")) {
            const QString id = params.value(QStringLiteral("frameId")).toString();
            page->frames.remove(id);
            if (mode == Mode::Acquisition && id == page->mainFrame && session == pages.value(target).session) {
                if (download.armed())
                    portalUnavailable();
                else
                    resetPortal();
            }
        } else if (method == QStringLiteral("Fetch.requestPaused")) {
            pausedRequest(params, session, page->mainFrame);
        }
    }

    void prepareFlow()
    {
        if (mode == Mode::Acquisition) {
            download.directory = std::make_unique<QTemporaryDir>(profile->path() + QStringLiteral("/download-XXXXXX"));
            if (!download.directory->isValid()) {
                failRound(QStringLiteral("A private connection-download directory could not be created."));
                return;
            }
        }
        QJsonObject behavior{{QStringLiteral("behavior"), mode == Mode::Acquisition
                             ? QStringLiteral("allowAndName") : QStringLiteral("deny")},
                             {QStringLiteral("eventsEnabled"), true}};
        if (download.directory)
            behavior.insert(QStringLiteral("downloadPath"), download.directory->path());
        send(QStringLiteral("Browser.setDownloadBehavior"), behavior, {}, [this](const QJsonObject&) {
            const QJsonArray filter{QJsonObject{{QStringLiteral("type"), QStringLiteral("page")}},
                                    QJsonObject{{QStringLiteral("exclude"), true}}};
            send(QStringLiteral("Target.setAutoAttach"), {
                {QStringLiteral("autoAttach"), true}, {QStringLiteral("waitForDebuggerOnStart"), true},
                {QStringLiteral("flatten"), true}, {QStringLiteral("filter"), filter}
            }, {}, [this](const QJsonObject&) {
                send(QStringLiteral("Target.setDiscoverTargets"), {{QStringLiteral("discover"), true}}, {},
                     [this](const QJsonObject&) { createTarget(); });
            });
        });
    }

    void createTarget()
    {
        creating = true;
        send(QStringLiteral("Target.createTarget"), {
            {QStringLiteral("url"), QStringLiteral("about:blank")},
            {QStringLiteral("newWindow"), true}, {QStringLiteral("background"), false}
        }, {}, [this](const QJsonObject& created) {
            target = created.value(QStringLiteral("targetId")).toString();
            creating = false;
            if (target.isEmpty()) {
                failRound(QStringLiteral("The browser window could not be created."));
                return;
            }
            // No browserContextId: every round uses Chromium's default private-profile context.
            if (contextCreated) {
                diagnostic(mode == Mode::Authentication ? QStringLiteral("browser-context-reused")
                                                        : QStringLiteral("browser-acquisition-context-reused"));
            } else {
                contextCreated = true;
                diagnostic(mode == Mode::Authentication ? QStringLiteral("browser-context-created")
                                                        : QStringLiteral("browser-acquisition-context-created"));
            }
            Page page;
            page.round = round;
            if (unclaimed.contains(target))
                page.session = unclaimed.take(target);
            pages.insert(target, std::move(page));
            const quint64 preparedRound = round;
            closeRetainedTarget();
            if (!active || round != preparedRound)
                return;
            const auto leftovers = unclaimed.keys();
            unclaimed.clear();
            for (const auto& id : leftovers)
                closeOneTarget(id);
            if (active && round == preparedRound && !pages.value(target).session.isEmpty())
                preparePage(target);
        });
    }

    void attachedTarget(const QJsonObject& params)
    {
        const QJsonObject info = params.value(QStringLiteral("targetInfo")).toObject();
        const QString id = info.value(QStringLiteral("targetId")).toString();
        const QString session = params.value(QStringLiteral("sessionId")).toString();
        const QString opener = info.value(QStringLiteral("openerId")).toString();
        if (id.isEmpty() || session.isEmpty() || info.value(QStringLiteral("type")).toString() != QStringLiteral("page"))
            return;
        if (id == retainedTarget) {
            retainedSession = session;
            return;
        }
        if (!active) {
            closeOneTarget(id);
            return;
        }
        if (pages.size() + unclaimed.size() + !retainedTarget.isEmpty() >= MaxTargets && !pages.contains(id)) {
            closeOneTarget(id);
            failRound(QStringLiteral("The browser opened too many windows for this request."));
            return;
        }
        if (id == target && pages.contains(id)) {
            pages[id].session = session;
            preparePage(id);
        } else if (pages.contains(opener) && pages.value(opener).round == round) {
            Page page;
            page.round = round;
            page.session = session;
            pages.insert(id, std::move(page));
            preparePage(id);
        } else if (creating) {
            unclaimed.insert(id, session);
        } else {
            closeOneTarget(id);
        }
    }

    bool rememberTree(Page& page, const QJsonObject& tree)
    {
        const QJsonObject frame = tree.value(QStringLiteral("frame")).toObject();
        const QString id = frame.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || page.frames.size() >= MaxFrames)
            return false;
        page.frames.insert(id, QUrl(frame.value(QStringLiteral("url")).toString(), QUrl::StrictMode));
        for (const auto& child : tree.value(QStringLiteral("childFrames")).toArray()) {
            if (!rememberTree(page, child.toObject()))
                return false;
        }
        return true;
    }

    void preparePage(const QString& id)
    {
        auto it = pages.find(id);
        if (it == pages.end() || it->preparing || it->session.isEmpty())
            return;
        it->preparing = true;
        const QString session = it->session;
        send(QStringLiteral("Page.enable"), {}, session, [this, id, session](const QJsonObject&) {
            send(QStringLiteral("Page.getFrameTree"), {}, session, [this, id, session](const QJsonObject& tree) {
                auto page = pages.find(id);
                if (page == pages.end())
                    return;
                const QJsonObject frameTree = tree.value(QStringLiteral("frameTree")).toObject();
                page->mainFrame = frameTree.value(QStringLiteral("frame")).toObject()
                    .value(QStringLiteral("id")).toString();
                if (!rememberTree(page.value(), frameTree) || page->mainFrame.isEmpty()) {
                    failRound(QStringLiteral("The browser did not provide a valid navigation frame."));
                    return;
                }
                if (mode == Mode::Authentication) {
                    const QJsonArray patterns{QJsonObject{
                        {QStringLiteral("resourceType"), QStringLiteral("Document")},
                        {QStringLiteral("requestStage"), QStringLiteral("Request")}
                    }};
                    send(QStringLiteral("Fetch.enable"), {{QStringLiteral("patterns"), patterns}}, session,
                         [this, id, session](const QJsonObject&) { resumePage(id, session); });
                } else {
                    resumePage(id, session);
                }
            });
        });
    }

    void resumePage(const QString& id, const QString& session)
    {
        // This acknowledgement follows Fetch.enable for auth, including opener popups.
        send(QStringLiteral("Runtime.runIfWaitingForDebugger"), {}, session, [this, id, session](const QJsonObject&) {
            if (id != target)
                return;
            const QString url = mode == Mode::Acquisition
                ? QStringLiteral("https://client.wvd.microsoft.com/arm/webclient/index.html?useLegacy")
                : auth.authorization.toString(QUrl::FullyEncoded);
            send(QStringLiteral("Page.navigate"), {{QStringLiteral("url"), url}}, session,
                 [this](const QJsonObject& navigation) {
                if (!navigation.value(QStringLiteral("errorText")).toString().isEmpty())
                    failRound(QStringLiteral("The Microsoft page could not be opened. Check the network connection."));
            });
            if (active)
                emit owner->status(mode == Mode::Acquisition
                    ? QStringLiteral("Complete Microsoft sign-in. Available Cloud PCs will be found and downloaded privately; no remote connection will start.")
                    : QStringLiteral("Complete Microsoft sign-in in the private Chromium window."));
            if (active && mode == Mode::Acquisition)
                portalPoll.start();
        });
    }

    void resetPortal()
    {
        const bool hadResources = portal.resetDocument();
        download.resetSelection();
        if (hadResources && active && mode == Mode::Acquisition)
            emit owner->resourcesAvailable({}, {});
    }

    void portalUnavailable()
    {
        failRound(QStringLiteral("Automatic acquisition could not verify the portal's controls or download mode. No further resource was selected. Import a trusted .rdpw file manually."));
    }

    void pollPortal()
    {
        if (!active || mode != Mode::Acquisition)
            return;
        if (portal.deadline && clock.elapsed() > portal.deadline) {
            portalUnavailable();
            return;
        }
        if (portal.busy || download.armed())
            return;
        const auto page = pages.constFind(target);
        if (page == pages.cend() || page->session.isEmpty() || page->mainFrame.isEmpty()
            || !acquisitionPage(page->frames.value(page->mainFrame)))
            return;
        if (portal.context) {
            evaluatePortal(QStringLiteral("poll"), {}, false);
            return;
        }
        if (portal.script.isEmpty()) {
            QFile script(QStringLiteral(":/portal/acquisition.js"));
            if (!script.open(QIODevice::ReadOnly)) {
                portalUnavailable();
                return;
            }
            portal.script = QString::fromUtf8(script.readAll());
        }
        portal.busy = true;
        portal.deadline = clock.elapsed() + 60000;
        send(QStringLiteral("Page.createIsolatedWorld"), {
            {QStringLiteral("frameId"), page->mainFrame},
            {QStringLiteral("worldName"), QStringLiteral("omawin365-acquisition")},
            {QStringLiteral("grantUniveralAccess"), false}
        }, page->session, [this](const QJsonObject& result) {
            portal.busy = false;
            portal.context = result.value(QStringLiteral("executionContextId")).toInt();
            if (portal.context <= 0) {
                portalUnavailable();
                return;
            }
            evaluatePortal(QStringLiteral("poll"), {}, true);
        }, portal.generation);
    }

    void evaluatePortal(const QString& command, const QString& id, bool initialize)
    {
        const auto page = pages.constFind(target);
        if (!active || mode != Mode::Acquisition || portal.busy || !portal.context
            || page == pages.cend() || !acquisitionPage(page->frames.value(page->mainFrame)))
            return;
        const QJsonArray arguments{command, id, QString::number(portal.generation)};
        // Numeric context ids are per renderer process: a replacement document can own
        // this id. Never call a global it could define; the script's own guard rejects it.
        QString expression;
        if (initialize)
            expression = QStringLiteral("delete globalThis.__omawin365Acquisition;");
        expression += QLatin1Char('(') + portal.script + QStringLiteral(")(...")
            + QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact))
            + QLatin1Char(')');
        portal.busy = true;
        send(QStringLiteral("Runtime.evaluate"), {
            {QStringLiteral("expression"), expression},
            {QStringLiteral("contextId"), portal.context},
            {QStringLiteral("returnByValue"), true},
            {QStringLiteral("awaitPromise"), false},
            {QStringLiteral("userGesture"), command == QStringLiteral("select")},
            {QStringLiteral("timeout"), 2000}
        }, page->session, [this, requestRound = round, generation = portal.generation](const QJsonObject& result) {
            portal.busy = false;
            if (result.contains(QStringLiteral("exceptionDetails"))) {
                portalUnavailable();
                return;
            }
            const QJsonObject value = result.value(QStringLiteral("result")).toObject()
                .value(QStringLiteral("value")).toObject();
            const QString state = value.value(QStringLiteral("state")).toString();
            if (state == QStringLiteral("waiting") || state == QStringLiteral("working"))
                return;
            if (state == QStringLiteral("downloading")) {
                if (!download.armed())
                    portalUnavailable();
                return;
            }
            if (state == QStringLiteral("empty")) {
                failRound(QStringLiteral("No downloadable connections could be discovered. The portal feed may be empty or could not load. Import a trusted .rdpw file manually or contact your administrator."));
                return;
            }
            if (state != QStringLiteral("resources")) {
                portalUnavailable();
                return;
            }
            const QJsonArray resources = value.value(QStringLiteral("resources")).toArray();
            if (resources.isEmpty() || resources.size() > 128 || download.armed()) {
                portalUnavailable();
                return;
            }
            QStringList ids;
            QStringList names;
            ids.reserve(resources.size());
            names.reserve(resources.size());
            for (const auto& item : resources) {
                const QJsonObject resource = item.toObject();
                const QString id = resource.value(QStringLiteral("id")).toString();
                const QString name = resource.value(QStringLiteral("name")).toString();
                const QString group = resource.value(QStringLiteral("group")).toString();
                if (id.isEmpty() || id.size() > 80 || ids.contains(id)
                    || name.isEmpty() || name.size() > 160 || group.size() > 160) {
                    portalUnavailable();
                    return;
                }
                ids.append(id);
                names.append(group.isEmpty() ? name : name + QStringLiteral(" — ") + group);
            }
            portal.deadline = 0; // No deadline while the user chooses a resource.
            const bool changed = resources != portal.resources;
            portal.resources = resources;
            if (!portal.requestedId.isEmpty()) {
                const QString requested = std::exchange(portal.requestedId, {});
                choosePortalResource(requested);
                return;
            }
            if (!changed)
                return;
            if (ids.size() == 1 && !portal.choiceOffered) {
                choosePortalResource(ids.constFirst());
            } else {
                portal.choiceOffered = true;
                emit owner->status(QStringLiteral("Choose the Cloud PC to import in OMAWIN365."));
                if (active && mode == Mode::Acquisition && round == requestRound
                    && portal.generation == generation && !download.armed())
                    emit owner->resourcesAvailable(ids, names);
            }
        }, portal.generation);
    }

    void choosePortalResource(const QString& id)
    {
        if (!active || mode != Mode::Acquisition || download.armed() || !portal.context)
            return;
        // Keep one explicit choice while the current DOM probe is in flight.
        if (portal.busy) {
            portal.requestedId = id;
            return;
        }
        for (const auto& item : portal.resources) {
            const QJsonObject resource = item.toObject();
            if (resource.value(QStringLiteral("id")).toString() != id)
                continue;
            download.displayName = resource.value(QStringLiteral("name")).toString();
            download.phase = Download::Phase::Clicked;
            portal.deadline = clock.elapsed() + CommandTimeoutMs;
            evaluatePortal(QStringLiteral("select"), id, false);
            return;
        }
        // Stale native selections cannot select a different document's resource.
        failRound(QStringLiteral("The selected Cloud PC is no longer available. Find your Cloud PCs again or import a trusted .rdpw file manually."));
    }

    void pausedRequest(const QJsonObject& params, const QString& session, const QString& mainFrame)
    {
        const QString requestId = params.value(QStringLiteral("requestId")).toString();
        if (requestId.isEmpty()) {
            transportFailure(QStringLiteral("The sign-in browser sent an invalid interception message."));
            return;
        }
        const QJsonObject request = params.value(QStringLiteral("request")).toObject();
        const QUrl url(request.value(QStringLiteral("url")).toString(), QUrl::StrictMode);
        const CallbackKind kind = BrowserAuth::classifyCallback(url,
            auth.contract.has_value()
                ? std::optional<QString>(auth.contract->state) : std::nullopt);
        if (kind == CallbackKind::NotCallback || !active || mode != Mode::Authentication) {
            send(QStringLiteral("Fetch.continueRequest"), {{QStringLiteral("requestId"), requestId}}, session, {});
            return;
        }
        // Abort before the nativeclient network endpoint can discard the one-use code.
        send(QStringLiteral("Fetch.failRequest"), {
            {QStringLiteral("requestId"), requestId}, {QStringLiteral("errorReason"), QStringLiteral("Aborted")}
        }, session, {});
        if (!active)
            return;
        if (params.value(QStringLiteral("resourceType")).toString() != QStringLiteral("Document")
            || params.value(QStringLiteral("frameId")).toString() != mainFrame
            || request.value(QStringLiteral("method")).toString() != QStringLiteral("GET")
            || kind == CallbackKind::Invalid) {
            failRound(QStringLiteral("The Microsoft sign-in response did not match this authentication request."));
            return;
        }
        if (kind == CallbackKind::OAuthError) {
            failRound(QStringLiteral("Microsoft sign-in was cancelled or denied. No connection was authorized."));
            return;
        }
        QByteArray canonical = OAuthContract::serialize(OAuthContract::reply(url, auth.contract->state));
        canonical.chop(1); // Already validated; serialization includes exactly one newline.
        const OAuthContract::Callback result{QUrl::fromEncoded(canonical, QUrl::StrictMode), auth.contract->generation};
        active = false;
        auth = {};
        // Local data: content, never a fabricated HTTP response from Microsoft's endpoint.
        send(QStringLiteral("Page.navigate"), {{QStringLiteral("url"), completionPage()}}, session, {});
        diagnostic(QStringLiteral("browser-auth-callback-complete"));
        emit owner->callbackReady(result);
    }

    void downloadStarted(const QJsonObject& params)
    {
        const QString guid = params.value(QStringLiteral("guid")).toString();
        if (!downloadGuidValid(guid)) {
            transportFailure(QStringLiteral("The browser supplied an invalid connection-download identifier."));
            return;
        }
        const auto page = pages.constFind(target);
        const QString frame = params.value(QStringLiteral("frameId")).toString();
        const bool ownedPortalFrame = page != pages.cend() && page->round == round
            && frame == page->mainFrame && acquisitionPage(page->frames.value(frame))
            && portalDownloadSource(QUrl(params.value(QStringLiteral("url")).toString(), QUrl::StrictMode));
        const QString name = params.value(QStringLiteral("suggestedFilename")).toString();
        if (!active || mode != Mode::Acquisition || !download.directory || !ownedPortalFrame
            || download.phase != Download::Phase::Clicked
            || (!name.endsWith(QStringLiteral(".rdpw"), Qt::CaseInsensitive)
                && !name.endsWith(QStringLiteral(".rdp"), Qt::CaseInsensitive))) {
            send(QStringLiteral("Browser.cancelDownload"), {{QStringLiteral("guid"), guid}}, {}, {});
            if (active && mode == Mode::Acquisition)
                emit owner->status(QStringLiteral("Only a Cloud PC RDP connection download from the owned official portal is accepted."));
            return;
        }
        download.guid = guid;
        download.phase = Download::Phase::Receiving;
        emit owner->status(QStringLiteral("Receiving the Cloud PC connection file privately…"));
    }

    void downloadProgress(const QJsonObject& params)
    {
        const QString guid = params.value(QStringLiteral("guid")).toString();
        if (!active || mode != Mode::Acquisition || download.phase != Download::Phase::Receiving
            || guid != download.guid || !download.directory)
            return;
        if (params.value(QStringLiteral("receivedBytes")).toDouble() > MaxDownloadBytes
            || params.value(QStringLiteral("totalBytes")).toDouble() > MaxDownloadBytes) {
            failRound(QStringLiteral("The connection download exceeded the 1 MiB safety limit."));
            return;
        }
        const QString state = params.value(QStringLiteral("state")).toString();
        if (state == QStringLiteral("canceled")) {
            failRound(QStringLiteral("The Cloud PC connection download was cancelled or interrupted."));
            return;
        }
        if (state != QStringLiteral("completed"))
            return;
        const QString source = download.directory->path() + QLatin1Char('/') + guid;
        const QByteArray sourceBytes = QFile::encodeName(source);
        const int fd = ::open(sourceBytes.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        struct stat info{};
        const bool valid = fd >= 0 && ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode)
            && info.st_uid == ::getuid() && info.st_nlink == 1
            && info.st_size > 0 && info.st_size <= MaxDownloadBytes
            && ::fchmod(fd, 0600) == 0;
        if (fd >= 0)
            ::close(fd);
        const QString destination = download.directory->path() + QStringLiteral("/Cloud PC.rdpw");
        if (!valid || QFileInfo::exists(destination) || !QFile::rename(source, destination)) {
            failRound(QStringLiteral("The portal did not produce a safe, complete Cloud PC connection file."));
            return;
        }
        // Direct UI connection imports synchronously. Reentrant begin/cancel cannot delete
        // this directory before its consumer returns because ownership is local here.
        auto completedDirectory = std::move(download.directory);
        const QString displayName = download.displayName;
        cancelRound();
        emit owner->profileDownloaded(destination, displayName);
        QFile::remove(destination);
    }
};

BrowserAuth::BrowserAuth(QObject* parent) : QObject(parent), d(std::make_unique<Private>(this)) {}
BrowserAuth::~BrowserAuth() = default;

void BrowserAuth::setDiagnosticsEnabled(bool enabled)
{
    d->diagnosticsEnabled = enabled;
}

void BrowserAuth::begin(const OAuthContract::Request& request)
{
    const QUrl authorizationUrl = request.authorization;
    const auto contract = OAuthContract::request(authorizationUrl);
    if (!contract || (!request.state.isEmpty() && request.state != contract->state) ||
        (!request.challenge.isEmpty() && request.challenge != contract->challenge) ||
        (!request.scope.isEmpty() && request.scope != contract->scope)) {
        d->cancelRound();
        emit failed(QStringLiteral("The connection supplied an unsupported or unsafe Microsoft sign-in request."));
        return;
    }
    d->cancelRound(true);
    d->auth = {authorizationUrl, contract};
    d->auth.contract->generation = request.generation;
    d->mode = Private::Mode::Authentication;
    d->active = true;
    d->diagnostic(QStringLiteral("browser-auth-round-begin"));
    emit status(QStringLiteral("Opening a private Microsoft sign-in window…"));
    if (d->process.state() == QProcess::Running) {
        d->diagnostic(QStringLiteral("browser-reused"));
        d->prepareFlow();
    } else if (d->process.state() == QProcess::NotRunning) {
        d->startBrowser();
    }
}

void BrowserAuth::beginProfileDownload()
{
    d->cancelRound(true);
    d->mode = Private::Mode::Acquisition;
    d->active = true;
    d->diagnostic(QStringLiteral("browser-acquisition-round-begin"));
    emit status(QStringLiteral("Opening Microsoft's official portal to obtain a Cloud PC connection file…"));
    if (d->process.state() == QProcess::Running) {
        d->diagnostic(QStringLiteral("browser-acquisition-reused"));
        d->prepareFlow();
    } else if (d->process.state() == QProcess::NotRunning) {
        d->startBrowser();
    }
}

void BrowserAuth::selectResource(const QString& id)
{
    d->choosePortalResource(id);
}

void BrowserAuth::cancel()
{
    const bool wasActive = d->active;
    d->cancelRound();
    if (wasActive)
        emit status(QStringLiteral("The browser request was cancelled."));
}
