#include "session.h"
#include "promptparser.h"
#include "rdpprofile.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QProcessEnvironment>
#include <QSocketNotifier>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <cstring>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>
#include <optional>

// Desktop discovery uses X11 only to observe this transport's owned window.
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

namespace {
void clearBytes(QByteArray& bytes)
{
    bytes.fill('\0');
    bytes.clear();
}

QProcessEnvironment freerdpEnvironment()
{
    auto environment = QProcessEnvironment::systemEnvironment();
    // Prevent inherited askpass from receiving PINs and logging settings from writing secrets to files.
    for (const auto& key : environment.keys()) {
        if (key.startsWith(QStringLiteral("WLOG_")) || key == QStringLiteral("FREERDP_ASKPASS") ||
            key == QStringLiteral("SSLKEYLOGFILE"))
            environment.remove(key);
    }
    environment.insert(QStringLiteral("WLOG_APPENDER"), QStringLiteral("CONSOLE"));
    environment.insert(QStringLiteral("WLOG_LEVEL"), QStringLiteral("WARN"));
    return environment;
}

int aboveStdio(int fd)
{
    if (fd < 0 || fd >= 3)
        return fd;
    const int duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    close(fd);
    return duplicate;
}

int staleWindowError(Display*, XErrorEvent*)
{
    return 0; // Windows can disappear between XQueryTree and XGetWindowAttributes.
}

class ScopedXErrors
{
public:
    explicit ScopedXErrors(Display* display) : display_(display)
    {
        XSync(display_, False);
        previous_ = XSetErrorHandler(staleWindowError);
    }
    ~ScopedXErrors()
    {
        XSync(display_, False);
        XSetErrorHandler(previous_);
    }
private:
    Display* display_;
    XErrorHandler previous_;
};

::Window ownedDesktop(Display* display, ::Window window, Atom pidAtom, pid_t pid,
                      const QByteArray& wmClass, int depth, int& remaining)
{
    if (--remaining < 0)
        return 0;
    XWindowAttributes attributes{};
    if (!XGetWindowAttributes(display, window, &attributes))
        return 0;
    if (attributes.map_state == IsViewable && attributes.width > 1 && attributes.height > 1) {
        Atom type = 0;
        int format = 0;
        unsigned long count = 0, extra = 0;
        unsigned char* data = nullptr;
        bool matchingPid = false;
        if (XGetWindowProperty(display, window, pidAtom, 0, 1, False, XA_CARDINAL,
                               &type, &format, &count, &extra, &data) == Success &&
            type == XA_CARDINAL && format == 32 && count == 1 && data) {
            matchingPid = *reinterpret_cast<unsigned long*>(data) == static_cast<unsigned long>(pid);
        }
        if (data)
            XFree(data);
        if (matchingPid) {
            XClassHint hint{};
            if (XGetClassHint(display, window, &hint)) {
                const bool matches = hint.res_class && wmClass == hint.res_class;
                if (hint.res_name)
                    XFree(hint.res_name);
                if (hint.res_class)
                    XFree(hint.res_class);
                if (matches)
                    return window;
            }
        }
    }
    if (depth == 0)
        return 0;
    ::Window root = 0, parent = 0;
    ::Window* children = nullptr;
    unsigned int count = 0;
    if (!XQueryTree(display, window, &root, &parent, &children, &count))
        return 0;
    ::Window found = 0;
    for (unsigned int i = 0; i < count && !found && remaining > 0; ++i)
        found = ownedDesktop(display, children[i], pidAtom, pid, wmClass, depth - 1, remaining);
    if (children)
        XFree(children);
    return found;
}
}

struct Session::State {
    enum class VersionCheck { Idle, Checking, Ending };
    enum class Awaiting { Nothing, Authorization, Pin };
    pid_t child = -1;
    int master = -1;
    int diagnosticsFd = -1;
    QSocketNotifier* diagnosticsReader = nullptr;
    PromptParser diagnosticParser{PromptParser::Mode::DiagnosticsOnly};
    QProcess versionProcess;
    QTimer versionTimer;
    VersionCheck versionCheck = VersionCheck::Idle;
    QString profilePath;
    QString executable;
    std::unique_ptr<QTemporaryDir> config;
    QSocketNotifier* reader = nullptr;
    QSocketNotifier* writer = nullptr;
    QTimer childTimer;
    QTimer windowTimer;
    QTimer touchTimer;
    QElapsedTimer stoppingTime;
    bool stopping = false;
    bool failed = false;
    bool desktopConnected = false;
    bool outputEnded = false;
    bool killSent = false;
    Awaiting awaiting = Awaiting::Nothing;
    quint64 generation = 0;
    // Transport identity is independent of OAuth rounds within that transport.
    quint64 transportEpoch = 0;
    bool terminalNotifications = false;
    bool finalizing = false;
    bool diagnosticsEnabled = false;
    PromptParser parser;
    QByteArray input;
    qsizetype written = 0;
    QString diagnostic;
    std::optional<OAuthContract::Request> authTransaction;
    QTimer authTimer;
    bool buildProbe = false;
    QByteArray probeOutput;
    qsizetype probeErrorBytes = 0;
    Display* display = nullptr;
    Atom pidAtom = 0;
    QByteArray wmClass;

    void clearAttempt()
    {
        config.reset();
    }
};

Session::Session(QObject* parent) : QObject(parent), state_(std::make_unique<State>())
{
    state_->childTimer.setInterval(100);
    state_->windowTimer.setInterval(350);
    state_->versionTimer.setSingleShot(true);
    connect(&state_->versionTimer, &QTimer::timeout, this, [this] {
        if (state_->versionCheck == State::VersionCheck::Checking) {
            state_->versionProcess.kill();
            fail(QStringLiteral("Could not verify the installed stock FreeRDP version within five seconds."));
        }
    });
    connect(&state_->versionProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (state_->versionCheck != State::VersionCheck::Checking) {
            // A failed exec has no finished signal. Complete an in-flight
            // cancellation here; crashes still finish through the normal slot.
            if (state_->versionCheck == State::VersionCheck::Ending && error == QProcess::FailedToStart) {
                state_->versionCheck = State::VersionCheck::Idle;
                emit ended();
            }
            return;
        }
        state_->versionCheck = State::VersionCheck::Idle;
        state_->versionTimer.stop();
        state_->versionCheck = state_->versionProcess.state() != QProcess::NotRunning
            ? State::VersionCheck::Ending : State::VersionCheck::Idle;
        if (state_->versionCheck == State::VersionCheck::Ending)
            state_->versionProcess.kill();
        fail(QStringLiteral("Could not inspect the installed stock FreeRDP executable."));
    });
    connect(&state_->versionProcess, &QProcess::readyReadStandardOutput, this, [this] {
        QByteArray bytes = state_->versionProcess.readAllStandardOutput();
        if (state_->versionCheck == State::VersionCheck::Checking) {
            const qsizetype limit = state_->buildProbe ? 32768 : 4096;
            if (state_->probeOutput.size() + bytes.size() >= limit)
                fail(QStringLiteral("Stock FreeRDP 3.32.1 with unambiguous WITH_SSO_MIB=OFF is required; executable verification failed."));
            else state_->probeOutput.append(bytes);
        }
        clearBytes(bytes);
    });
    connect(&state_->versionProcess, &QProcess::readyReadStandardError, this, [this] {
        QByteArray bytes = state_->versionProcess.readAllStandardError();
        state_->probeErrorBytes += bytes.size();
        clearBytes(bytes);
        if (state_->versionCheck == State::VersionCheck::Checking && state_->probeErrorBytes >= 32768)
            fail(QStringLiteral("Stock FreeRDP 3.32.1 with unambiguous WITH_SSO_MIB=OFF is required; executable verification failed."));
    });
    state_->authTimer.setSingleShot(true);
    connect(&state_->authTimer, &QTimer::timeout, this, [this] {
        if (state_->awaiting == State::Awaiting::Authorization)
            fail(QStringLiteral("Microsoft sign-in timed out; disconnected without submitting a response."));
    });
    connect(&state_->versionProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus exit) {
        if (state_->versionCheck != State::VersionCheck::Checking) {
            if (state_->versionCheck == State::VersionCheck::Ending) {
                state_->versionCheck = State::VersionCheck::Idle;
                emit ended();
            }
            return;
        }
        state_->versionCheck = State::VersionCheck::Idle;
        state_->versionTimer.stop();
        QByteArray version = std::move(state_->probeOutput);
        bool supported = code == 0 && exit == QProcess::NormalExit && state_->probeErrorBytes == 0;
        if (!state_->buildProbe) {
            static const QRegularExpression expected(QStringLiteral(
                "\\AThis is FreeRDP version (?:\\[[^\\]\\r\\n]+\\] )?3\\.32\\.1(?: \\([^\\r\\n]*\\))?\\r?\\n?\\z"));
            supported = supported && version.size() < 4096 && expected.match(QString::fromLatin1(version)).hasMatch();
        } else {
            const QString text = QString::fromLatin1(version);
            static const QRegularExpression flag(QStringLiteral("(?:^|\\s)WITH_SSO_MIB=([^\\s]+)"));
            auto matches = flag.globalMatch(text);
            int count = 0;
            bool off = false;
            while (matches.hasNext()) { ++count; off = matches.next().captured(1) == QStringLiteral("OFF"); }
            // Count every occurrence too: malformed/embedded/conflicting aliases fail closed.
            supported = supported && version.size() < 32768 &&
                text.contains(QStringLiteral("Build configuration:")) && count == 1 && off &&
                text.count(QStringLiteral("WITH_SSO_MIB")) == 1;
        }
        clearBytes(version);
        if (!supported) {
            fail(QStringLiteral("Stock FreeRDP 3.32.1 with unambiguous WITH_SSO_MIB=OFF is required; executable verification failed."));
            return;
        }
        if (!state_->buildProbe) {
            state_->buildProbe = true;
            state_->probeErrorBytes = 0;
            state_->versionCheck = State::VersionCheck::Checking;
            state_->versionProcess.start(state_->executable, {QStringLiteral("/buildconfig")});
            if (state_->versionCheck == State::VersionCheck::Checking) state_->versionTimer.start(5000);
            return;
        }
        startTransport(state_->profilePath);
    });
    state_->touchTimer.setSingleShot(true);
    connect(&state_->touchTimer, &QTimer::timeout, this, [this] {
        if (active() && !state_->stopping && state_->desktopConnected &&
            state_->awaiting == State::Awaiting::Nothing)
            emit statusChanged(QStringLiteral("connected"), QStringLiteral(
                "Desktop connected. Check the remote application for authentication results; PIN submission does not confirm sign-in."));
    });
}

Session::~Session()
{
    // QProcess destruction can emit errorOccurred/finished while waiting for
    // its child. Disconnect before State's reverse member teardown destroys
    // the timers and other data used by those callbacks.
    state_->versionTimer.stop();
    state_->versionProcess.disconnect(this);
    if (state_->versionProcess.state() != QProcess::NotRunning) {
        state_->versionProcess.kill();
        state_->versionProcess.waitForFinished();
    }
    if (state_->child > 0) {
        // Also applied when the event loop is already gone. PDEATHSIG protects an
        // abrupt parent death; the normal path kills the entire owned process group.
        const pid_t child = state_->child;
        kill(-child, SIGTERM);
        kill(child, SIGTERM);
        QElapsedTimer deadline;
        deadline.start();
        int status = 0;
        pid_t result = 0;
        while ((result = waitpid(child, &status, WNOHANG)) == 0 && deadline.elapsed() < 200)
            usleep(10000);
        kill(-child, SIGKILL);
        if (result == 0) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        }
        state_->child = -1;
    }
    releaseTransport();
}

void Session::setDiagnosticsEnabled(bool enabled)
{
    state_->diagnosticsEnabled = enabled;
}

void Session::diagnose(const char* event)
{
    if (state_->diagnosticsEnabled)
        emit diagnosticEvent(QString::fromLatin1(event));
}

bool Session::active() const
{
    return state_->versionCheck != State::VersionCheck::Idle || state_->child > 0;
}

void Session::start(const QString& profilePath)
{
    // Terminal delivery/finalization must finish before a receiver can replace it.
    if (state_->terminalNotifications || state_->finalizing)
        return;
    if (active()) {
        emit error(QStringLiteral("Disconnect the current desktop before starting another session."));
        return;
    }
    diagnose("connection-start");
    state_->failed = false;
    state_->stopping = false;
    state_->clearAttempt();
    state_->profilePath = profilePath;
    state_->executable = QStandardPaths::findExecutable(QStringLiteral("xfreerdp3"));
    if (state_->executable.isEmpty()) {
        fail(QStringLiteral("Stock xfreerdp3 is required but was not found."));
        return;
    }
    verifyExecutable();
}

void Session::verifyExecutable()
{
    state_->buildProbe = false;
    clearBytes(state_->probeOutput);
    state_->probeErrorBytes = 0;
    state_->versionProcess.setProcessEnvironment(freerdpEnvironment());
    state_->versionCheck = State::VersionCheck::Checking;
    state_->versionProcess.start(state_->executable, {QStringLiteral("/version")});
    if (state_->versionCheck != State::VersionCheck::Checking)
        return;
    state_->versionTimer.start(5000);
    emit statusChanged(QStringLiteral("connecting"), QStringLiteral("Checking the installed stock FreeRDP version."));
}

void Session::startTransport(const QString& profilePath)
{
    const QFileInfo profile(profilePath);
    const QString path = profile.canonicalFilePath();
    const QString executable = state_->executable;
    if (path.isEmpty() || !profile.isFile() || !profile.isReadable()) {
        fail(QStringLiteral("The imported connection file is unavailable."));
        return;
    }
    if (executable.isEmpty()) {
        fail(QStringLiteral("Stock xfreerdp3 is required but was not found."));
        return;
    }
    // Revalidate every transport launch. Leave old
    // profiles unchanged. This path check does not close pathname replacement races.
    QString profileError;
    if (!RdpProfile::readAndValidate(path, nullptr, &profileError)) {
        fail(profileError);
        return;
    }
    releaseTransport();
    if (!state_->config) {
        state_->config = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/omawin365-rdp-XXXXXX"));
        if (!state_->config->isValid() || state_->config->path().contains(',')) {
            fail(QStringLiteral("Could not create a private FreeRDP certificate configuration."));
            return;
        }
    }
    state_->parser.reset();
    state_->failed = state_->stopping = state_->desktopConnected = state_->outputEnded = state_->killSent = false;
    state_->awaiting = State::Awaiting::Nothing;
    state_->diagnostic.clear();
    state_->authTransaction.reset();
    state_->authTimer.stop();
    ++state_->generation;
    state_->wmClass = "omawin365-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1();

    // The RDP file is parsed first; these settings override its local-redirection
    // defaults without editing its signature or overriding gateway/tenant policy.
    // Clear channel/device counts before load_addins rebuilds allowed channels.
    QByteArray tune =
        "/tune:FreeRDP_RedirectWebAuthN:TRUE,FreeRDP_AuthenticationLevel:2,"
        "FreeRDP_IgnoreCertificate:FALSE,FreeRDP_AutoAcceptCertificate:FALSE,"
        "FreeRDP_AutoDenyCertificate:FALSE,FreeRDP_ExternalCertificateManagement:FALSE,"
        "FreeRDP_CertificateCallbackPreferPEM:FALSE,"
        "FreeRDP_RedirectDrives:FALSE,FreeRDP_RedirectHomeDrive:FALSE,"
        "FreeRDP_DrivesToRedirect:,FreeRDP_RedirectPrinters:FALSE,"
        "FreeRDP_RedirectSmartCards:FALSE,FreeRDP_RedirectSerialPorts:FALSE,"
        "FreeRDP_RedirectParallelPorts:FALSE,FreeRDP_AudioCapture:FALSE,"
        "FreeRDP_AudioPlayback:FALSE,FreeRDP_SupportSSHAgentChannel:FALSE,"
        "FreeRDP_DeviceRedirection:FALSE,FreeRDP_DeviceCount:0,"
        "FreeRDP_StaticChannelCount:0,FreeRDP_DynamicChannelCount:0,"
        "FreeRDP_AutoReconnectionEnabled:FALSE,FreeRDP_RemoteApplicationMode:FALSE,"
        // Native ConfigureNotify requests remote monitor layouts through disp;
        // load_addins rebuilds disp/drdynvc after the channel-count reset above.
        "FreeRDP_DynamicResolutionUpdate:TRUE,FreeRDP_SupportDisplayControl:TRUE,"
        "FreeRDP_SmartSizing:FALSE,FreeRDP_ParentWindowId:0,FreeRDP_EmbeddedWindow:FALSE,"
        "FreeRDP_Decorations:TRUE,FreeRDP_ToggleFullscreen:TRUE,"
        "FreeRDP_ActionScript:,FreeRDP_ConfigPath:" + QFile::encodeName(state_->config->path());
    std::vector<QByteArray> args = {
        QFile::encodeName(executable), QFile::encodeName(path),
        "+force-console-callbacks", "/gateway:type:arm", "/sec:aad", "/clipboard:files-to:off", "-auto-reconnect",
        "/t:OMAWIN365 - Windows 365", "/wm-class:" + state_->wmClass, "/size:80%",
        "/log-level:WARN", "/log-filters:com.freerdp.client.x11:ERROR", tune
    };
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    const auto environment = freerdpEnvironment();
    std::vector<QByteArray> envStrings;
    for (const auto& entry : environment.toStringList())
        envStrings.push_back(entry.toLocal8Bit());
    std::vector<char*> envp;
    envp.reserve(envStrings.size() + 1);
    for (auto& entry : envStrings)
        envp.push_back(entry.data());
    envp.push_back(nullptr);

    int master = aboveStdio(posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK));
    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0) {
        if (master >= 0)
            close(master);
        fail(QStringLiteral("Could not create a private terminal for FreeRDP."));
        return;
    }
    char slaveName[256]{};
    int slave = -1;
    if (ptsname_r(master, slaveName, sizeof(slaveName)) == 0)
        slave = aboveStdio(open(slaveName, O_RDWR | O_NOCTTY | O_CLOEXEC));
    termios terminal{};
    if (slave < 0 || tcgetattr(slave, &terminal) < 0) {
        if (slave >= 0)
            close(slave);
        close(master);
        fail(QStringLiteral("Could not configure FreeRDP's private terminal."));
        return;
    }
    // Echo is off BEFORE exec, including non-PIN OAuth replies. Noncanonical
    // input avoids the terminal's small canonical-line limit on redirect URLs.
    terminal.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    terminal.c_iflag &= ~(IXON | IXOFF | ICRNL | INLCR | IGNCR);
    terminal.c_oflag &= ~OPOST;
    terminal.c_cc[VMIN] = 1;
    terminal.c_cc[VTIME] = 0;
    if (tcsetattr(slave, TCSANOW, &terminal) < 0) {
        close(slave);
        close(master);
        fail(QStringLiteral("Could not disable echo on FreeRDP's private terminal."));
        return;
    }
    int diagnosticPipe[2] = {-1, -1};
    if (pipe2(diagnosticPipe, O_CLOEXEC | O_NONBLOCK) < 0) {
        close(slave);
        close(master);
        fail(QStringLiteral("Could not isolate FreeRDP diagnostics from authentication prompts."));
        return;
    }
    diagnosticPipe[0] = aboveStdio(diagnosticPipe[0]);
    diagnosticPipe[1] = aboveStdio(diagnosticPipe[1]);
    if (diagnosticPipe[0] < 0 || diagnosticPipe[1] < 0) {
        if (diagnosticPipe[0] >= 0)
            close(diagnosticPipe[0]);
        if (diagnosticPipe[1] >= 0)
            close(diagnosticPipe[1]);
        close(slave);
        close(master);
        fail(QStringLiteral("Could not prepare private FreeRDP process descriptors."));
        return;
    }
    fcntl(diagnosticPipe[1], F_SETFL, fcntl(diagnosticPipe[1], F_GETFL) & ~O_NONBLOCK);
    const pid_t parentPid = getpid();
    const long maximumFd = sysconf(_SC_OPEN_MAX);
    const pid_t child = fork();
    if (child == 0) {
        // Only async-signal-safe/syscall work in the post-fork child of a Qt app.
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != parentPid ||
            setsid() < 0 || ioctl(slave, TIOCSCTTY, 0) < 0 ||
            dup2(slave, STDIN_FILENO) < 0 || dup2(slave, STDOUT_FILENO) < 0 ||
            dup2(diagnosticPipe[1], STDERR_FILENO) < 0)
            _exit(126);
        struct sigaction action{};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        for (int signal : {SIGTERM, SIGINT, SIGHUP, SIGPIPE, SIGCHLD})
            sigaction(signal, &action, nullptr);
        sigset_t mask;
        sigemptyset(&mask);
        sigprocmask(SIG_SETMASK, &mask, nullptr);
        const rlimit noCore{0, 0};
        if (setrlimit(RLIMIT_CORE, &noCore) < 0)
            _exit(126);
#ifdef SYS_close_range
        if (syscall(SYS_close_range, 3U, ~0U, 0U) < 0)
#endif
            for (int fd = 3; fd < maximumFd; ++fd)
                close(fd);
        execve(argv.front(), argv.data(), envp.data());
        _exit(127);
    }
    close(slave);
    close(diagnosticPipe[1]);
    if (child < 0) {
        close(master);
        close(diagnosticPipe[0]);
        fail(QStringLiteral("Could not launch stock FreeRDP."));
        return;
    }
    state_->master = master;
    state_->child = child;
    const quint64 epoch = ++state_->transportEpoch;
    state_->diagnosticsFd = diagnosticPipe[0];
    state_->diagnosticParser.reset();
    state_->diagnosticsReader = new QSocketNotifier(diagnosticPipe[0], QSocketNotifier::Read, this);
    connect(state_->diagnosticsReader, &QSocketNotifier::activated, this,
        [this, epoch, child, master, fd = diagnosticPipe[0]] {
            if (ownsTransport(epoch, child, master) && state_->diagnosticsFd == fd)
                readDiagnostics();
        });
    state_->reader = new QSocketNotifier(master, QSocketNotifier::Read, this);
    state_->writer = new QSocketNotifier(master, QSocketNotifier::Write, this);
    state_->writer->setEnabled(false);
    connect(state_->reader, &QSocketNotifier::activated, this, [this, epoch, child, master] {
        if (ownsTransport(epoch, child, master)) readOutput();
    });
    connect(state_->writer, &QSocketNotifier::activated, this, [this, epoch, child, master] {
        if (ownsTransport(epoch, child, master)) flushInput();
    });
    connect(&state_->childTimer, &QTimer::timeout, this, [this, epoch, child, master] {
        if (ownsTransport(epoch, child, master)) checkChild();
    });
    connect(&state_->windowTimer, &QTimer::timeout, this, [this, epoch, child, master] {
        if (ownsTransport(epoch, child, master)) checkDesktop();
    });
    state_->display = XOpenDisplay(nullptr);
    if (state_->display)
        state_->pidAtom = XInternAtom(state_->display, "_NET_WM_PID", False);
    state_->childTimer.start();
    state_->windowTimer.start();
    diagnose("transport-start");
    if (!ownsTransport(epoch, child, master) || state_->stopping)
        return;
    emit statusChanged(QStringLiteral("connecting"), QStringLiteral("Starting a private Windows 365 connection."));
    if (!ownsTransport(epoch, child, master) || state_->stopping)
        return;
    if (!state_->display)
        fail(QStringLiteral("An XWayland display is required for the stock FreeRDP desktop."));
}

void Session::releaseTransport()
{
    state_->childTimer.stop();
    state_->childTimer.disconnect(this);
    state_->windowTimer.stop();
    state_->windowTimer.disconnect(this);
    state_->touchTimer.stop();
    if (state_->reader) {
        state_->reader->setEnabled(false);
        state_->reader->deleteLater();
        state_->reader = nullptr;
    }
    if (state_->writer) {
        state_->writer->setEnabled(false);
        state_->writer->deleteLater();
        state_->writer = nullptr;
    }
    if (state_->master >= 0) {
        close(state_->master);
        state_->master = -1;
    }
    if (state_->diagnosticsReader) {
        state_->diagnosticsReader->setEnabled(false);
        state_->diagnosticsReader->deleteLater();
        state_->diagnosticsReader = nullptr;
    }
    if (state_->diagnosticsFd >= 0) {
        close(state_->diagnosticsFd);
        state_->diagnosticsFd = -1;
    }
    state_->diagnosticParser.reset();
    if (state_->display) {
        XCloseDisplay(state_->display);
        state_->display = nullptr;
    }
    clearBytes(state_->input);
    state_->written = 0;
    state_->authTransaction.reset();
    state_->authTimer.stop();
    state_->parser.reset();
    state_->awaiting = State::Awaiting::Nothing;
}

void Session::stop()
{
    if (state_->terminalNotifications)
        return; // Certificate termination is already latched, including its deadline.
    const quint64 epoch = state_->transportEpoch;
    const pid_t child = state_->child;
    const int master = state_->master;
    if (active())
        diagnose("connection-stop-request");
    if (child > 0 && !ownsTransport(epoch, child, master))
        return;
    stopTransport();
}

void Session::stopTransport()
{
    if (state_->versionCheck == State::VersionCheck::Checking) {
        state_->versionCheck = State::VersionCheck::Ending;
        state_->versionTimer.stop();
        state_->versionProcess.kill();
        state_->clearAttempt();
        if (!state_->failed)
            emit statusChanged(QStringLiteral("disconnected"), QStringLiteral("Connection cancelled before FreeRDP started."));
        return;
    }
    if (state_->child <= 0 || state_->stopping)
        return;
    const quint64 epoch = state_->transportEpoch;
    const pid_t child = state_->child;
    const int master = state_->master;
    const bool pinCancelled = state_->awaiting == State::Awaiting::Pin;
    diagnose("transport-stop");
    if (!ownsTransport(epoch, child, master) || state_->stopping)
        return;
    state_->stopping = true;
    ++state_->generation;
    state_->touchTimer.stop();
    state_->windowTimer.stop();
    state_->awaiting = State::Awaiting::Nothing;
    state_->authTransaction.reset();
    state_->authTimer.stop();
    clearBytes(state_->input);
    state_->written = 0;
    if (state_->writer)
        state_->writer->setEnabled(false);
    state_->stoppingTime.start();
    kill(-child, SIGTERM);
    kill(child, SIGTERM); // Covers the brief interval before child setsid().
    if (!state_->failed) {
        emit statusChanged(QStringLiteral("disconnecting"), pinCancelled
                ? QStringLiteral("PIN cancelled. FreeRDP cannot cancel this request separately; disconnecting the desktop.")
                : QStringLiteral("Disconnecting the Windows 365 desktop."));
    }
}

void Session::readDiagnostics()
{
    if (state_->diagnosticsFd < 0)
        return;
    for (int round = 0; round < 16; ++round) {
        char buffer[4096];
        const ssize_t count = read(state_->diagnosticsFd, buffer, sizeof(buffer));
        if (count > 0) {
            const auto events = state_->diagnosticParser.feed(QByteArray::fromRawData(buffer, count));
            for (const auto& event : events) {
                if (event.kind == PromptParser::Kind::Diagnostic)
                    state_->diagnostic = event.detail;
            }
            explicit_bzero(buffer, sizeof(buffer));
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        state_->diagnosticsReader->setEnabled(false);
        return;
    }
}

void Session::fail(const QString& message)
{
    if (state_->failed)
        return;
    state_->failed = true;
    diagnose("connection-error");
    emit statusChanged(QStringLiteral("error"), message);
    emit error(message);
    if (active())
        stop();
    else {
        releaseTransport();
        state_->clearAttempt();
        emit ended();
    }
}

bool Session::ownsTransport(quint64 epoch, qint64 child, int master) const
{
    return child > 0 && state_->transportEpoch == epoch &&
        state_->child == child && state_->master == master;
}

void Session::rejectCertificate(const QString& message)
{
    if (state_->child <= 0 || state_->failed)
        return;
    const pid_t child = state_->child;
    const bool newlyStopping = !state_->stopping;
    // No receiver may reap/replace this owned attempt until all terminal
    // notifications finish. Invalidate input authority before the first signal.
    state_->terminalNotifications = true;
    state_->failed = true;
    state_->stopping = true;
    ++state_->generation;
    state_->awaiting = State::Awaiting::Nothing;
    state_->authTransaction.reset();
    state_->authTimer.stop();
    state_->touchTimer.stop();
    state_->windowTimer.stop();
    clearBytes(state_->input);
    state_->written = 0;
    if (state_->writer)
        state_->writer->setEnabled(false);
    if (newlyStopping) {
        state_->stoppingTime.start();
        kill(-child, SIGTERM);
        // In a final drain waitpid has already reaped the leader; its numeric
        // PID is no longer ours. Otherwise also cover the pre-setsid interval.
        if (!state_->finalizing)
            kill(child, SIGTERM);
    }
    diagnose("connection-error");
    if (newlyStopping)
        diagnose("transport-stop");
    emit statusChanged(QStringLiteral("error"), message);
    emit error(message);
    // The child timer owns eventual reaping; keep ConfigPath alive until then.
    // No signals or cleanup continuation may follow release of this fence.
    state_->terminalNotifications = false;
}

void Session::readOutput()
{
    if (state_->master < 0 || state_->outputEnded)
        return;
    const quint64 epoch = state_->transportEpoch;
    const pid_t child = state_->child;
    const int master = state_->master;
    const auto interactive = [this, epoch, child, master] {
        return ownsTransport(epoch, child, master) && !state_->stopping;
    };
    // Bound work per activation so noisy child output cannot starve UI/PIN input.
    for (int round = 0; round < 16; ++round) {
        if (!ownsTransport(epoch, child, master))
            return;
        char buffer[4096];
        const auto wipe = qScopeGuard([&buffer] { explicit_bzero(buffer, sizeof(buffer)); });
        const ssize_t count = read(master, buffer, sizeof(buffer));
        if (count > 0) {
            if (!state_->stopping) {
                const auto events = state_->parser.feed(QByteArray::fromRawData(buffer, count));
                for (const auto& event : events) {
                    if (!interactive())
                        return;
                    if (event.kind == PromptParser::Kind::Diagnostic) {
                        state_->diagnostic = event.detail;
                        continue;
                    }
                    if (state_->awaiting != State::Awaiting::Nothing) {
                        const QString message = QStringLiteral("FreeRDP produced overlapping input requests; disconnected without sending secrets.");
                        if (event.kind == PromptParser::Kind::Certificate)
                            rejectCertificate(message);
                        else
                            fail(message);
                        return;
                    }
                    state_->touchTimer.stop();
                    switch (event.kind) {
                    case PromptParser::Kind::Authorization: {
                        if (!event.authorization) {
                            fail(QStringLiteral("Unsupported Microsoft sign-in request."));
                            return;
                        }
                        state_->authTransaction = event.authorization;
                        state_->authTransaction->generation = ++state_->generation;
                        state_->awaiting = State::Awaiting::Authorization;
                        state_->authTimer.start(180000);
                        const auto request = *state_->authTransaction;
                        diagnose("authorization-request");
                        if (!interactive()) return;
                        emit statusChanged(QStringLiteral("signing-in"), QStringLiteral("Complete Microsoft sign-in in the private browser."));
                        if (!interactive()) return;
                        if (state_->authTransaction && request.generation == state_->generation &&
                            state_->awaiting == State::Awaiting::Authorization)
                            emit authRequested(request);
                        break;
                    }
                    case PromptParser::Kind::Pin:
                        state_->awaiting = State::Awaiting::Pin;
                        diagnose("native-pin-request");
                        if (!interactive()) return;
                        emit statusChanged(QStringLiteral("awaiting-pin"), QStringLiteral("Your security key requires its local FIDO2 PIN."));
                        if (!interactive()) return;
                        if (state_->awaiting == State::Awaiting::Pin)
                            emit pinRequested(QStringLiteral("Enter your security key PIN, then touch the key. Cancelling disconnects the desktop; PINs are never retried automatically."));
                        break;
                    case PromptParser::Kind::Certificate:
                        rejectCertificate(QStringLiteral("FreeRDP requested certificate trust that OMAWIN365 cannot safely grant. The connection is being stopped; no certificate exception was granted. Contact your administrator to review certificate trust and server identity."));
                        return; // Abandon the entire old batch/activation, wiping buffer.
                    case PromptParser::Kind::UnsupportedPrompt:
                        fail(QStringLiteral("FreeRDP requested an unsupported interactive challenge or policy consent. No response was sent; complete any organization requirements with supported Microsoft tooling."));
                        return;
                    case PromptParser::Kind::InvalidPrompt:
                        fail(QStringLiteral("FreeRDP produced an invalid or oversized authentication/certificate request; no input was sent."));
                        return;
                    case PromptParser::Kind::Diagnostic:
                        break;
                    }
                    if (!interactive()) return;
                }
            }
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (count == 0 || (count < 0 && errno == EIO)) {
            state_->outputEnded = true;
            if (state_->reader) state_->reader->setEnabled(false);
            return;
        }
        fail(QStringLiteral("The private FreeRDP terminal could not be read."));
        return;
    }
}

void Session::queueInput(QByteArray bytes)
{
    if (!active() || state_->stopping || state_->master < 0) {
        clearBytes(bytes);
        return;
    }
    if (!state_->input.isEmpty() || bytes.size() > 4095 ||
        bytes.contains('\0') || bytes.contains('\r') || !bytes.endsWith('\n') ||
        bytes.indexOf('\n') != bytes.size() - 1) {
        clearBytes(bytes);
        fail(QStringLiteral("Input could not be safely delivered to FreeRDP; disconnected."));
        return;
    }
    state_->input = std::move(bytes);
    state_->written = 0;
    state_->writer->setEnabled(true);
    flushInput();
}

void Session::flushInput()
{
    if (state_->master < 0 || state_->stopping)
        return;
    while (state_->written < state_->input.size()) {
        const ssize_t count = write(state_->master, state_->input.constData() + state_->written,
                                    state_->input.size() - state_->written);
        if (count > 0) {
            // Wipe only bytes already delivered; preserve the remaining suffix
            // for EAGAIN/partial writes, without duplicating any submitted secret.
            explicit_bzero(state_->input.data() + state_->written, count);
            state_->written += count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        fail(QStringLiteral("Input delivery to the private FreeRDP terminal failed."));
        return;
    }
    clearBytes(state_->input);
    state_->written = 0;
    state_->writer->setEnabled(false);
}

void Session::submitPin(const QString& pin)
{
    if (!active() || state_->stopping || state_->awaiting != State::Awaiting::Pin)
        return; // Cancelled/late/duplicate PINs can never become another request's input.
    const quint64 epoch = state_->transportEpoch;
    const pid_t child = state_->child;
    const int master = state_->master;
    QByteArray bytes = pin.toUtf8();
    bool invalid = pin.size() < 4 || bytes.size() > 63;
    for (char c : bytes)
        invalid = invalid || static_cast<unsigned char>(c) < 0x20 || c == 0x7f;
    if (invalid) {
        clearBytes(bytes);
        emit error(QStringLiteral("The security key PIN must have at least four characters, at most 63 UTF-8 bytes, and no control characters."));
        return;
    }
    state_->awaiting = State::Awaiting::Nothing;
    bytes.append('\n');
    queueInput(std::move(bytes));
    if (!ownsTransport(epoch, child, master) || state_->stopping) return;
    diagnose("native-pin-submitted");
    if (!ownsTransport(epoch, child, master) || state_->stopping) return;
    emit statusChanged(QStringLiteral("awaiting-touch"), QStringLiteral("PIN sent once. Touch your security key now; check the remote application for the result."));
    if (ownsTransport(epoch, child, master) && !state_->stopping)
        state_->touchTimer.start(15000);
}

void Session::submitAuthResult(const OAuthContract::Callback& result)
{
    if (!active() || state_->stopping || state_->awaiting != State::Awaiting::Authorization)
        return;
    if (!state_->authTransaction || state_->authTransaction->generation != state_->generation ||
        result.generation != state_->generation)
        return;
    const quint64 epoch = state_->transportEpoch;
    const pid_t child = state_->child;
    const int master = state_->master;
    const auto reply = OAuthContract::reply(result.url, state_->authTransaction->state);
    QByteArray bytes = OAuthContract::serialize(reply);
    if (bytes.isEmpty()) {
        fail(QStringLiteral("Microsoft returned an invalid or mismatched sign-in callback; disconnected without submitting it."));
        return;
    }
    state_->awaiting = State::Awaiting::Nothing;
    state_->authTransaction.reset();
    state_->authTimer.stop();
    queueInput(std::move(bytes));
    if (!ownsTransport(epoch, child, master) || state_->stopping) return;
    diagnose("authorization-submitted");
    if (ownsTransport(epoch, child, master) && !state_->stopping)
        emit statusChanged(QStringLiteral("connecting"), QStringLiteral("Microsoft sign-in response delivered privately; waiting for the desktop."));
}

void Session::checkDesktop()
{
    if (!active() || state_->stopping || state_->desktopConnected || !state_->display)
        return;
    const quint64 epoch = state_->transportEpoch;
    const pid_t child = state_->child;
    const int master = state_->master;
    ::Window desktop = 0;
    {
        ScopedXErrors errors(state_->display);
        int remaining = 1024;
        desktop = ownedDesktop(state_->display, DefaultRootWindow(state_->display),
                               state_->pidAtom, state_->child, state_->wmClass, 5, remaining);
    }
    if (!desktop)
        return;
    state_->desktopConnected = true;
    diagnose("desktop-connected");
    if (!ownsTransport(epoch, child, master) || state_->stopping) return;
    state_->windowTimer.stop();
    if (state_->awaiting == State::Awaiting::Nothing)
        emit statusChanged(QStringLiteral("connected"), QStringLiteral("The owned Windows 365 desktop window is connected and visible."));
    if (ownsTransport(epoch, child, master) && !state_->stopping)
        emit connected();
}

void Session::checkChild()
{
    if (state_->child <= 0 || state_->terminalNotifications || state_->finalizing)
        return;
    const quint64 epoch = state_->transportEpoch;
    const pid_t child = state_->child;
    const int master = state_->master;
    if (state_->stopping && !state_->killSent && state_->stoppingTime.elapsed() >= 2000) {
        state_->killSent = true;
        kill(-child, SIGKILL);
        kill(child, SIGKILL);
    }
    int status = 0;
    const pid_t result = waitpid(child, &status, WNOHANG);
    if (result == 0 || (result < 0 && errno == EINTR))
        return;
    if (result < 0 && errno != ECHILD) {
        fail(QStringLiteral("Could not monitor the owned FreeRDP process."));
        return;
    }
    // Fence before final drains: these can reject a certificate and deliver
    // signals whose receivers run nested event loops. Reap/cleanup only once.
    state_->finalizing = true;
    auto finalization = qScopeGuard([this] { state_->finalizing = false; });
    if (!state_->stopping)
        readOutput();
    if (!ownsTransport(epoch, child, master)) return;
    readDiagnostics();
    diagnose("transport-exit");
    if (!ownsTransport(epoch, child, master)) return;
    const bool requestedStop = state_->stopping;
    const bool wasConnected = state_->desktopConnected;
    kill(-child, SIGKILL); // No helpers survive the owned session leader.
    state_->child = -1;
    releaseTransport();
    state_->clearAttempt();
    if (!requestedStop && !state_->failed &&
        (!wasConnected || (result > 0 && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)))) {
        state_->failed = true;
        QString message = state_->diagnostic;
        if (message.isEmpty()) {
            if (result > 0 && WIFEXITED(status) && (WEXITSTATUS(status) == 126 || WEXITSTATUS(status) == 127))
                message = QStringLiteral("Stock FreeRDP could not start its controlled terminal or executable.");
            else
                message = wasConnected ? QStringLiteral("The FreeRDP desktop ended unexpectedly.")
                                       : QStringLiteral("FreeRDP ended before a connected desktop was observed.");
        }
        emit statusChanged(QStringLiteral("error"), message);
        emit error(message);
    } else if (!state_->failed) {
        emit statusChanged(QStringLiteral("disconnected"), QStringLiteral("Windows 365 disconnected. You can start a fresh connection."));
    }
    // Cleanup is complete. Ended may synchronously start a fresh epoch;
    // no old-attempt state mutation or signal follows it.
    state_->finalizing = false;
    finalization.dismiss();
    emit ended();
}
