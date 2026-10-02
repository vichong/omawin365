// Synthetic external process adapter. Only launch configuration uses test access;
// stimuli and assertions below cross public calls/signals and CDP/filesystem seams.
#include <QThread>

namespace {
const QString CallbackEndpoint = "https://login.microsoftonline.com/common/oauth2/nativeclient";
const QString PortalEndpoint = "https://client.wvd.microsoft.com/arm/webclient/index.html";

QUrl syntheticAuthorization(const QString& state = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
{
    QUrl url("https://login.microsoftonline.com/common/oauth2/v2.0/authorize");
    QUrlQuery query;
    query.addQueryItem("client_id", "a85cf173-4192-42f8-81fa-777a763e6e2c");
    query.addQueryItem("response_type", "code");
            query.addQueryItem("scope", "https://www.wvd.microsoft.com/.default openid profile offline_access");
            query.addQueryItem("code_challenge", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
            query.addQueryItem("code_challenge_method", "S256");
    query.addQueryItem("redirect_uri", CallbackEndpoint);
    query.addQueryItem("state", state);
    url.setQuery(query);
    return url;
}

bool eventually(const std::function<bool()>& predicate, int timeout = 4000)
{
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (predicate()) return true;
        QThread::msleep(2);
    } while (timer.elapsed() < timeout);
    return predicate();
}

QList<QJsonObject> transcript(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QList<QJsonObject> result;
    for (const auto& line : file.readAll().split('\n')) {
        if (!line.isEmpty()) result.append(QJsonDocument::fromJson(line).object());
    }
    return result;
}

int commandCount(const QString& trace, const QString& method)
{
    int count = 0;
    for (const auto& item : transcript(trace))
        if (item.value("method").toString() == method) ++count;
    return count;
}

int cancellationsFor(const QString& trace, const QString& guid)
{
    int count = 0;
    for (const auto& item : transcript(trace))
        if (item.value("method") == "Browser.cancelDownload"
            && item.value("params").toObject().value("guid") == guid) ++count;
    return count;
}

QString downloadDirectory(const QString& trace)
{
    for (const auto& item : transcript(trace)) {
        const auto params = item.value("params").toObject();
        if (item.value("method") == "Browser.setDownloadBehavior" && params.value("behavior") == "allowAndName")
            return params.value("downloadPath").toString();
    }
    return {};
}

bool emitted(const QString& trace, const QString& event)
{
    for (const auto& item : transcript(trace))
        if (item.value("event").toString() == event) return true;
    return false;
}

bool directoryFilesystem(const QString& path, long* type)
{
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct statfs info{};
    const bool valid = ::fstatfs(fd, &info) == 0;
    ::close(fd);
    if (valid) *type = info.f_type;
    return valid;
}

bool knownDiskFilesystem(long type)
{
    // Do not mistake ramfs or an unknown overlay backing store for actual disk evidence.
    return type == BTRFS_SUPER_MAGIC || type == EXT4_SUPER_MAGIC || type == XFS_SUPER_MAGIC;
}

QString fixtureParent(bool memoryBacked)
{
    const QStringList candidates = memoryBacked
        ? QStringList{QStringLiteral("/tmp"), QStringLiteral("/dev/shm")}
        : QStringList{QStringLiteral(BROWSER_TEST_SOURCE_DIR), QStringLiteral("/var/tmp")};
    for (const QString& path : candidates) {
        long type = 0;
        if (directoryFilesystem(path, &type)
            && (memoryBacked ? type == TMPFS_MAGIC : knownDiskFilesystem(type))
            && QFileInfo(path).isWritable())
            return path;
    }
    return {}; // Preflight fails explicitly; no fallback to the wrong storage type.
}

bool privateTmpfsFixture(const QString& path)
{
    struct stat info{};
    long type = 0;
    return !path.isEmpty() && ::lstat(QFile::encodeName(path).constData(), &info) == 0
        && S_ISDIR(info.st_mode) && info.st_uid == ::getuid() && (info.st_mode & 0777) == 0700
        && directoryFilesystem(path, &type) && type == TMPFS_MAGIC;
}

// Each fixture owns an absolute 0700 tmpfs runtime root. No real runtime/browser
// state is read. Environment restoration happens after BrowserAuth destruction.
struct SyntheticRuntime {
    QTemporaryDir root{fixtureParent(true).isEmpty()
        ? QStringLiteral("/nonexistent-omawin365-fixture/runtime-XXXXXX")
        : fixtureParent(true) + QStringLiteral("/omawin365-browser-test-XXXXXX")};
    QByteArray previous = qgetenv("XDG_RUNTIME_DIR");
    bool previouslySet = qEnvironmentVariableIsSet("XDG_RUNTIME_DIR");
    SyntheticRuntime() { qputenv("XDG_RUNTIME_DIR", QFile::encodeName(root.path())); }
    ~SyntheticRuntime()
    {
        if (previouslySet) qputenv("XDG_RUNTIME_DIR", previous);
        else qunsetenv("XDG_RUNTIME_DIR");
    }
    QString trace() const { return root.path() + "/transcript.jsonl"; }
};

class SyntheticChromium {
    QString scenario;
    QFile trace;
    QByteArray input;
    int targets = 0;
    QString downloadPath;
    const QString guid = "11111111-1111-1111-1111-111111111111";
    QJsonObject heldCreate;
    QJsonObject heldProbe;

    void record(const QJsonObject& object)
    {
        const QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
        if (trace.write(line) != line.size() || !trace.flush()) ::_exit(3);
    }
    void message(const QJsonObject& object)
    {
        QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        bytes.append('\0');
        qsizetype offset = 0;
        while (offset < bytes.size()) {
            const ssize_t n = ::write(4, bytes.constData() + offset, bytes.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) ::_exit(2);
            offset += n;
        }
    }
    void event(const QString& method, const QJsonObject& params, const QString& session = {})
    {
        record({{"event", method}, {"params", params}, {"sessionId", session}});
        QJsonObject object{{"method", method}, {"params", params}};
        if (!session.isEmpty()) object.insert("sessionId", session);
        message(object);
    }
    void respond(const QJsonObject& command, const QJsonObject& result = {})
    {
        message({{"id", command.value("id")}, {"result", result}});
    }
    void callback(const QString& session, bool foreign = false, const QString& code = "synthetic-code")
    {
        QString url = CallbackEndpoint + "?code=" + code + "&state=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        if (foreign) url = CallbackEndpoint + "?code=foreign-code&state=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        if (scenario == "empty-state") url = CallbackEndpoint + "?code=synthetic-code&state=";
        if (scenario == "empty-state-missing" || scenario == "no-state") url = CallbackEndpoint + "?code=synthetic-code";
        if (scenario == "no-state-extra") url = CallbackEndpoint + "?code=synthetic-code&state=extra";
        if (scenario == "form-state") url = CallbackEndpoint + "?code=synthetic-code&state=a%20b%2Bc";
        if (scenario == "wrong-form-state") url = CallbackEndpoint + "?code=synthetic-code&state=a+b+c";
        if (scenario == "wrong-state") url = CallbackEndpoint + "?code=synthetic-code&state=wrong";
        if (scenario == "missing-state") url = CallbackEndpoint + "?code=synthetic-code";
        if (scenario == "duplicate-code") url += "&code=second";
        if (scenario == "duplicate-state") url += "&state=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        if (scenario == "mixed-code-error") url += "&error=denied";
        if (scenario == "fragment") url += "#fragment";
        if (scenario == "oauth-error") url = CallbackEndpoint + "?error=access_denied&state=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        if (scenario == "foreign-origin") url = "https://attacker.invalid/common/oauth2/nativeclient?code=synthetic-code";
        if (scenario == "foreign-path") url = "https://login.microsoftonline.com/other?code=synthetic-code";
        event("Fetch.requestPaused", {{"requestId", foreign ? "foreign-request" : "synthetic-request"},
            {"resourceType", scenario == "not-document" ? "XHR" : "Document"},
            {"frameId", scenario == "subframe" ? "child-frame" : "frame-" + session},
            {"request", QJsonObject{{"method", scenario == "post" ? "POST" : "GET"}, {"url", url}}}},
            foreign ? "unowned-session" : session);
    }
    void download(const QString& session)
    {
        const QString frame = "frame-" + session;
        const auto begin = [&](const QString& id, const QString& sourceFrame, const QString& name) {
            event("Browser.downloadWillBegin", {{"guid", id}, {"frameId", sourceFrame}, {"suggestedFilename", name}});
        };
        if (scenario == "navigate-after-click")
            event("Page.frameNavigated", {{"frame", QJsonObject{{"id", frame}, {"url", PortalEndpoint}}}}, session);
        if (scenario == "wrong-download-frame") begin("22222222-2222-2222-2222-222222222222", "unowned-frame", "Cloud PC.rdpw");
        if (scenario == "wrong-download-name") begin("22222222-2222-2222-2222-222222222222", frame, "payload.exe");
        if (scenario == "invalid-guid") { begin("../invalid", frame, "Cloud PC.rdpw"); return; }
        if (scenario == "empty-progress-clicked")
            event("Browser.downloadProgress", {{"guid", ""}, {"state", "canceled"}});
        begin(guid, frame, "Cloud PC.rdpw");
        if (scenario == "foreign-progress-receiving") {
            event("Browser.downloadProgress", {{"guid", ""}, {"state", "canceled"}});
            event("Browser.downloadProgress", {{"guid", "22222222-2222-2222-2222-222222222222"}, {"state", "canceled"}});
        }
        if (scenario == "second-download") begin("22222222-2222-2222-2222-222222222222", frame, "Second.rdp");
        const QString path = downloadPath + '/' + guid;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) ::_exit(4);
        if (scenario != "empty-file")
            file.write("full address:s:synthetic.invalid\ngatewayhostname:s:synthetic.invalid\narm path:s:synthetic\n");
        if (scenario == "oversize-file" && !file.resize(1024 * 1024 + 1)) ::_exit(4);
        file.close();
        if (scenario == "symlink-file") {
            const QByteArray bytes = QFile::encodeName(path);
            const QString renamed = path + "-target";
            if (!QFile::rename(path, renamed) || ::symlink(QFile::encodeName(renamed).constData(), bytes.constData()) != 0) ::_exit(4);
        }
        if (scenario == "hardlink-file"
            && ::link(QFile::encodeName(path).constData(), QFile::encodeName(path + "-link").constData()) != 0) ::_exit(4);
        if (scenario == "destination-exists") {
            QFile destination(downloadPath + "/Cloud PC.rdpw");
            if (!destination.open(QIODevice::WriteOnly)) ::_exit(4);
            destination.write("existing synthetic data");
        }
        event("Browser.downloadProgress", {{"guid", guid},
            {"receivedBytes", scenario == "oversize-progress" ? 1024 * 1024 + 1 : 88},
            {"state", scenario == "canceled-download" ? "canceled" : "completed"}});
        // A delayed duplicate completion must not publish another file.
        event("Browser.downloadProgress", {{"guid", guid}, {"state", "completed"}});
    }
    void command(const QJsonObject& command)
    {
        record(command);
        const QString method = command.value("method").toString();
        const QString session = command.value("sessionId").toString();
        const QJsonObject params = command.value("params").toObject();
        if (method == "Browser.setDownloadBehavior") {
            if (params.value("behavior") == "allowAndName") downloadPath = params.value("downloadPath").toString();
            if (params.value("behavior") == "deny" && !heldCreate.isEmpty()) {
                respond(heldCreate, {{"targetId", "page-1"}});
                heldCreate = {};
            }
            if (params.value("behavior") == "deny" && !heldProbe.isEmpty()) {
                respond(heldProbe, {{"result", QJsonObject{{"value", QJsonObject{
                    {"state", "resources"}, {"resources", QJsonArray{QJsonObject{{"id", "retired"}, {"name", "Retired PC"}}}}}}}}});
                heldProbe = {};
            }
            respond(command);
        } else if (method == "Target.createTarget") {
            const QString target = "page-" + QString::number(++targets);
            const QString attached = "session-" + QString::number(targets);
            if (scenario == "handoff" && targets > 1) callback("session-1", false, "retired-code");
            event("Target.attachedToTarget", {{"sessionId", attached}, {"targetInfo", QJsonObject{
                {"targetId", target}, {"type", "page"}}}});
            if (scenario == "hold-create" && targets == 1) heldCreate = command;
            else respond(command, {{"targetId", target}});
        } else if (method == "Page.getFrameTree") {
            respond(command, {{"frameTree", QJsonObject{{"frame", QJsonObject{
                {"id", "frame-" + session}, {"url", "about:blank"}}}}}});
        } else if (method == "Page.navigate") {
            respond(command);
            const QString url = params.value("url").toString();
            if (url.startsWith(PortalEndpoint)) {
                QString mainUrl = url;
                if (scenario == "portal-wrong-origin") mainUrl = "https://attacker.invalid/arm/webclient/index.html";
                if (scenario == "portal-wrong-path") mainUrl = "https://client.wvd.microsoft.com/other";
                if (scenario == "portal-subframe" || scenario == "portal-popup") mainUrl = "about:blank";
                event("Page.frameNavigated", {{"frame", QJsonObject{
                    {"id", "frame-" + session}, {"url", mainUrl}}}}, session);
                if (scenario == "portal-subframe")
                    event("Page.frameNavigated", {{"frame", QJsonObject{
                        {"id", "child-frame"}, {"parentId", "frame-" + session}, {"url", url}}}}, session);
                if (scenario == "portal-popup") {
                    event("Target.attachedToTarget", {{"sessionId", "popup-session"}, {"targetInfo", QJsonObject{
                        {"targetId", "popup"}, {"type", "page"}, {"openerId", "page-1"}}}});
                    event("Page.frameNavigated", {{"frame", QJsonObject{{"id", "frame-popup-session"}, {"url", url}}}}, "popup-session");
                }
                if (scenario == "empty-progress-unarmed")
                    event("Browser.downloadProgress", {{"guid", ""}, {"state", "canceled"}});
                if (scenario == "unarmed-download")
                    event("Browser.downloadWillBegin", {{"guid", "22222222-2222-2222-2222-222222222222"},
                        {"frameId", "frame-" + session}, {"suggestedFilename", "Unarmed.rdpw"}});
            } else if (url.startsWith("https://login.microsoftonline.com/")) {
                event("Page.frameNavigated", {{"frame", QJsonObject{
                    {"id", "frame-" + session}, {"url", url}}}}, session);
                if (scenario == "child-exit") ::_exit(0);
                if (scenario == "malformed-json") {
                    const char malformed[] = "{not-json}\0";
                    if (::write(4, malformed, sizeof(malformed) - 1) != sizeof(malformed) - 1) ::_exit(2);
                    return;
                }
                if (scenario == "unowned-first") callback(session, true);
                if (scenario != "silent" && (scenario != "hold-create" || targets > 1)) callback(session);
                if (scenario == "duplicate-event") callback(session);
            }
        } else if (method == "Page.createIsolatedWorld") {
            respond(command, {{"executionContextId", 1}});
        } else if (method == "Runtime.evaluate") {
            if (params.value("expression").toString().contains("[\"select\"")) {
                respond(command, {{"result", QJsonObject{{"value", QJsonObject{{"state", "downloading"}}}}}});
                download(session);
            } else {
                if (scenario == "hold-poll" && session == "session-1") { heldProbe = command; return; }
                const QJsonArray resources{QJsonObject{{"id", "first"}, {"name", "First PC"}},
                                           QJsonObject{{"id", "second"}, {"name", "Second PC"}}};
                respond(command, {{"result", QJsonObject{{"value", QJsonObject{
                    {"state", "resources"}, {"resources", resources}}}}}});
            }
        } else if (method == "Target.closeTarget") {
            respond(command);
            event("Target.targetDestroyed", {{"targetId", params.value("targetId")}});
        } else {
            respond(command);
        }
    }
public:
    explicit SyntheticChromium(const QStringList& args) : scenario(args.at(2)), trace(args.at(3))
    {
        if (!trace.open(QIODevice::WriteOnly | QIODevice::Truncate)) ::_exit(3);
        QString profile;
        for (const auto& arg : args)
            if (arg.startsWith("--user-data-dir=")) profile = arg.mid(16);
        QJsonObject environment;
        for (const auto& key : {"XDG_CACHE_HOME", "TMPDIR", "TMP", "TEMP", "HOME", "XDG_CONFIG_HOME",
                                "XDG_DATA_HOME", "XDG_RUNTIME_DIR", "DISPLAY", "WAYLAND_DISPLAY",
                                "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS"})
            environment.insert(QLatin1String(key), qEnvironmentVariable(key));
        bool storageWrites = true;
        if (scenario == "storage") {
            QString diskCache;
            for (const auto& arg : args)
                if (arg.startsWith("--disk-cache-dir=")) diskCache = arg.mid(17);
            for (const QString& path : {diskCache, qEnvironmentVariable("XDG_CACHE_HOME"),
                                       qEnvironmentVariable("TMPDIR")}) {
                // Even a broken launch contract must not write outside the owned profile.
                if (profile.isEmpty() || !path.startsWith(profile + '/')) { storageWrites = false; continue; }
                QFile probe(path + "/synthetic-store");
                storageWrites = probe.open(QIODevice::WriteOnly)
                    && probe.write("synthetic browser bytes") == 23 && storageWrites;
            }
        }
        record({{"pid", qint64(::getpid())}, {"profile", profile},
                {"arguments", QJsonArray::fromStringList(args)}, {"environment", environment},
                {"storageWrites", storageWrites}});
    }
    int run()
    {
        char buffer[8192];
        for (;;) {
            const ssize_t n = ::read(3, buffer, sizeof(buffer));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return n == 0 ? 0 : 2;
            input.append(buffer, n);
            qsizetype end;
            while ((end = input.indexOf('\0')) >= 0) {
                const auto object = QJsonDocument::fromJson(input.left(end)).object();
                input.remove(0, end + 1);
                command(object);
            }
        }
    }
};

int syntheticChromium(const QStringList& args) { return SyntheticChromium(args).run(); }

struct ScopedBrowserEnvironment {
    QProcessEnvironment previous = QProcessEnvironment::systemEnvironment();
    QStringList changed;
    void set(const QString& key, const QString& value)
    {
        if (!changed.contains(key)) changed.append(key);
        qputenv(key.toUtf8().constData(), value.toUtf8());
    }
    ~ScopedBrowserEnvironment()
    {
        for (const QString& key : changed) {
            if (previous.contains(key)) qputenv(key.toUtf8().constData(), previous.value(key).toUtf8());
            else qunsetenv(key.toUtf8().constData());
        }
    }
};

void browserStorageTests(const std::function<void(bool, const char*)>& check)
{
    const QString diskParent = fixtureParent(false);
    QTemporaryDir disk(diskParent.isEmpty()
        ? QStringLiteral("/nonexistent-omawin365-fixture/disk-XXXXXX")
        : diskParent + QStringLiteral("/omawin365-browser-disk-test-XXXXXX"));
    long diskType = 0;
    const bool diskReady = disk.isValid() && directoryFilesystem(disk.path(), &diskType) && knownDiskFilesystem(diskType);
    check(diskReady, "disk-negative preflight requires an actual writable disk-filesystem fixture (no skip)");
    std::printf("Browser storage fixture parents: tmpfs=%s; disk=%s (f_type=0x%lx).\n",
                qPrintable(fixtureParent(true)), qPrintable(diskParent), static_cast<unsigned long>(diskType));
    SyntheticRuntime runtime;
    const QString root = runtime.root.path();
    QTemporaryDir permissive(root + "/permissive-XXXXXX");
    const bool modeReady = permissive.isValid()
        && ::chmod(QFile::encodeName(permissive.path()).constData(), 0755) == 0;
    check(modeReady, "unsafe-permission runtime fixture is prepared");
    const QString link = root + "/runtime-link";
    check(::symlink(QFile::encodeName(root).constData(), QFile::encodeName(link).constData()) == 0,
          "symlink runtime fixture is prepared");
    const QString regular = root + "/runtime-file";
    QFile file(regular);
    const bool fileReady = file.open(QIODevice::WriteOnly);
    file.close();
    check(fileReady, "non-directory runtime fixture is prepared");
    QList<QPair<QString, const char*>> invalid{
        {{}, "empty"}, {"relative-runtime", "relative"}, {root + "/missing", "missing"},
        {regular, "regular-file"}, {link, "symlink"}, {link + '/', "symlink-with-slash"},
        {permissive.path(), "permissive"}};
    if (diskReady) invalid.append({disk.path(), "disk-backed"});
    int rejected = 0;
    for (const auto& test : invalid) {
        for (const bool acquisition : {false, true}) {
            ScopedBrowserEnvironment environment;
            environment.set("XDG_RUNTIME_DIR", test.first);
            const QString trace = root + "/rejected-transcript.jsonl";
            BrowserAuth browser;
            BrowserAuthTestAccess::useSyntheticBrowser(browser, "storage", trace);
            QStringList failures;
            QObject::connect(&browser, &BrowserAuth::failed,
                [&](const QString& message) { failures.append(message); });
            if (acquisition) browser.beginProfileDownload();
            else browser.begin(syntheticAuthorization());
            eventually([] { return false; }, 100); // Settle any incorrectly queued launch before the absence check.
            const QByteArray name = QByteArray("runtime fails closed before launch with fixed error: ") + test.second
                + (acquisition ? " acquisition" : " authentication");
            check(failures == QStringList{
                      "A private, user-owned tmpfs XDG runtime directory is required for browser sign-in."}
                  && !QFileInfo::exists(trace)
                  && QDir(root).entryList({"omawin365-auth-*"}, QDir::Dirs).isEmpty(), name.constData());
            ++rejected;
        }
    }
    // Natural mkdir/open failures and mode/filesystem rejection use real syscalls,
    // not a production override or a mocked filesystem classification.
    check(!createPrivateMemoryDirectory(root)
          && !createPrivateMemoryDirectory(regular)
          && !createPrivateMemoryDirectory(root + "/missing/child")
          && !privateMemoryDirectory(root + "/missing")
          && !privateMemoryDirectory(regular)
          && !privateMemoryDirectory(link), "directory preparation fails closed on real syscall errors");
    if (diskReady)
        check(!createPrivateMemoryDirectory(disk.path() + "/cache"),
              "newly created cache directory on actual disk is rejected independently");
    {
        QTemporaryDir profile(root + "/profile-check-XXXXXX");
        check(profile.isValid() && privateMemoryDirectory(profile.path()), "separate real tmpfs profile descriptor is accepted");
        check(::chmod(QFile::encodeName(profile.path()).constData(), 0710) == 0
              && !privateMemoryDirectory(profile.path()), "separate profile descriptor rejects unsafe mode");
        ::chmod(QFile::encodeName(profile.path()).constData(), 0700);
    }
    for (const bool acquisition : {false, true}) {
        ScopedBrowserEnvironment environment;
        // These inherited locations are synthetic too; no real HOME/profile is inspected.
        QJsonObject preserved;
        for (const auto& key : {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "DISPLAY", "WAYLAND_DISPLAY",
                                "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS"}) {
            const QString value = root + "/preserved-" + key;
            environment.set(key, value);
            preserved.insert(QLatin1String(key), value);
        }
        for (const auto& key : {"XDG_CACHE_HOME", "TMPDIR", "TMP", "TEMP"})
            environment.set(key, root + "/inherited-" + key);
        QString profile, cache, temporary;
        const QString trace = root + (acquisition ? "/storage-acquisition.jsonl" : "/storage-auth.jsonl");
        {
            BrowserAuth browser;
            BrowserAuthTestAccess::useSyntheticBrowser(browser, "storage", trace);
            int failures = 0;
            QObject::connect(&browser, &BrowserAuth::failed, [&] { ++failures; });
            if (acquisition) browser.beginProfileDownload();
            else browser.begin(syntheticAuthorization());
            check(eventually([&] { return !transcript(trace).isEmpty() || failures; }) && failures == 0,
                  "actual writable tmpfs runtime launches synthetic browser");
            const auto metadata = transcript(trace).value(0);
            profile = metadata.value("profile").toString();
            cache = profile + "/cache";
            temporary = profile + "/tmp";
            check(profile.startsWith(root + "/omawin365-auth-") && privateTmpfsFixture(profile)
                  && privateTmpfsFixture(cache) && privateTmpfsFixture(temporary),
                  "runtime/profile/cache/temp are independently private user-owned actual tmpfs directories");
            const auto arguments = metadata.value("arguments").toArray();
            check(arguments.contains("--enable-automation") && arguments.contains("--password-store=basic")
                  && arguments.contains("--user-data-dir=" + profile)
                  && arguments.contains("--disk-cache-dir=" + cache),
                  "synthetic argv pins automation plus private profile and explicit disk-cache path");
            const auto child = metadata.value("environment").toObject();
            check(child.value("XDG_CACHE_HOME") == cache && child.value("TMPDIR") == temporary
                  && child.value("TMP") == temporary && child.value("TEMP") == temporary
                  && child.value("XDG_RUNTIME_DIR") == root,
                  "child cache/temp environment overrides stay beneath the owned profile");
            bool unchanged = true;
            for (auto it = preserved.begin(); it != preserved.end(); ++it)
                unchanged = unchanged && child.value(it.key()) == it.value();
            check(unchanged, "child preserves HOME/config/data and GUI IPC environment locations");
            check(metadata.value("storageWrites").toBool() && QFileInfo::exists(cache + "/synthetic-store")
                  && QFileInfo::exists(temporary + "/synthetic-store"),
                  "synthetic browser writes declared cache/temp stores within owned tmpfs tree");
            browser.cancel();
        }
        check(!profile.isEmpty() && !QFileInfo::exists(profile) && !QFileInfo::exists(cache)
              && !QFileInfo::exists(temporary) && QFileInfo::exists(root),
              "normal teardown removes declared stores but preserves runtime root");
    }
    {
        // Restrictive umask causes naturally unsafe preparation; restore it before
        // any subsequent event processing or fixture teardown.
        BrowserAuth browser;
        const QString trace = root + "/preparation-failure.jsonl";
        BrowserAuthTestAccess::useSyntheticBrowser(browser, "storage", trace);
        QStringList failures;
        QObject::connect(&browser, &BrowserAuth::failed,
            [&](const QString& message) { failures.append(message); });
        const mode_t previous = ::umask(0777);
        browser.begin(syntheticAuthorization());
        ::umask(previous);
        check(failures == QStringList{"A private sign-in browser could not be prepared."}
              && !QFileInfo::exists(trace) && QDir(root).entryList({"omawin365-auth-*"}, QDir::Dirs).isEmpty(),
              "unsafe profile/cache/temp preparation fails before launch with fixed error and removes private state");
    }
    std::printf("Browser storage runtime rejection scenarios: %d; synthetic launch modes: 2.\n", rejected);
}

void publicBrowserTests(const std::function<void(bool, const char*)>& outerCheck)
{
    int cases = 0, assertions = 0;
    const auto check = [&](bool pass, const char* name) { ++assertions; outerCheck(pass, name); };
    {
        SyntheticRuntime runtime;
        const bool ready = runtime.root.isValid() && privateTmpfsFixture(runtime.root.path());
        check(ready, "preflight requires an owned writable real tmpfs fixture (no skip or disk fallback)");
        if (!ready) return;
    }
    browserStorageTests(check);
    {
        SyntheticRuntime runtime;
        BrowserAuth browser;
        BrowserAuthTestAccess::useSyntheticBrowser(browser, "code", runtime.trace());
        int callbacks = 0, failures = 0;
        QUrl captured;
        QObject::connect(&browser, &BrowserAuth::callbackReady, [&](const OAuthContract::Callback& result) { const QUrl& url = result.url;
            ++callbacks; captured = url;
        });
        QObject::connect(&browser, &BrowserAuth::failed, [&] { ++failures; });
        browser.begin(syntheticAuthorization());
        check(eventually([&] { return callbacks || failures; }) && callbacks == 1 && failures == 0
              && captured == QUrl(CallbackEndpoint + "?code=synthetic-code&state=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
              "public sign-in returns the matching callback from an owned main frame");
        check(eventually([&] { return commandCount(runtime.trace(), "Fetch.failRequest") == 1; }),
              "matching callback is aborted at the external CDP seam");
        ++cases;
    }
    struct AuthCase {
        const char* scenario;
        const char* requestQuery;
        const char* callbackQuery;
        bool accepted;
    };
    for (const AuthCase& test : {
        AuthCase{"empty-state", "state=", "code=synthetic-code&state=", false},
        AuthCase{"empty-state-missing", "state=", "code=synthetic-code", false},
        AuthCase{"no-state", "", "code=synthetic-code", false},
        AuthCase{"no-state-extra", "", "code=synthetic-code&state=extra", false},
        AuthCase{"form-state", "state=a+b%2Bc", "code=synthetic-code&state=a%20b%2Bc", false},
        AuthCase{"wrong-form-state", "state=a+b%2Bc", "code=synthetic-code&state=a+b+c", false}}) {
        SyntheticRuntime runtime;
        BrowserAuth browser;
        BrowserAuthTestAccess::useSyntheticBrowser(browser, test.scenario, runtime.trace());
        int callbacks = 0, failures = 0;
        QUrl captured;
        QString failureMessage;
        QObject::connect(&browser, &BrowserAuth::callbackReady, [&](const OAuthContract::Callback& result) { const QUrl& url = result.url; ++callbacks; captured = url; });
        QObject::connect(&browser, &BrowserAuth::failed, [&](const QString& message) { ++failures; failureMessage = message; });
        QUrl request = syntheticAuthorization();
        QUrlQuery query(request);
        query.removeAllQueryItems("state");
        request.setQuery(query.query(QUrl::FullyEncoded)
                         + (QString(test.requestQuery).isEmpty() ? QString() : '&' + QString(test.requestQuery)), QUrl::StrictMode);
        browser.begin(request);
        const QByteArray name = QByteArray("public sign-in preserves optional/form state semantics: ") + test.scenario;
        check(eventually([&] { return callbacks || failures; })
              && (test.accepted
                  ? callbacks == 1 && failures == 0 && captured == QUrl(CallbackEndpoint + '?' + test.callbackQuery)
                  : callbacks == 0 && failures == 1
                    && failureMessage == "The connection supplied an unsupported or unsafe Microsoft sign-in request."), name.constData());
        check(commandCount(runtime.trace(), "Fetch.failRequest") == 0,
              "legacy optional/form-state request rejected before browser launch");
        ++cases;
    }
    for (const auto& scenario : {"wrong-state", "missing-state", "duplicate-code", "duplicate-state",
                                 "mixed-code-error", "fragment", "oauth-error", "post", "subframe", "not-document"}) {
        SyntheticRuntime runtime;
        BrowserAuth browser;
        BrowserAuthTestAccess::useSyntheticBrowser(browser, scenario, runtime.trace());
        int callbacks = 0, failures = 0;
        QString failureMessage;
        QObject::connect(&browser, &BrowserAuth::callbackReady, [&] { ++callbacks; });
        QObject::connect(&browser, &BrowserAuth::failed, [&](const QString& message) { ++failures; failureMessage = message; });
        browser.begin(syntheticAuthorization());
        const QByteArray name = QByteArray("public callback rejects ") + scenario;
        check(eventually([&] { return callbacks || failures; }) && failures == 1 && callbacks == 0
              && eventually([&] { return commandCount(runtime.trace(), "Fetch.failRequest") == 1; }),
              name.constData());
        check(failureMessage == (QString(scenario) == "oauth-error"
              ? "Microsoft sign-in was cancelled or denied. No connection was authorized."
              : "The Microsoft sign-in response did not match this authentication request."),
              "callback rejection preserves its exact mismatch or OAuth-denied message");
        ++cases;
    }
    for (const auto& scenario : {"foreign-origin", "foreign-path"}) {
        SyntheticRuntime runtime;
        BrowserAuth browser;
        BrowserAuthTestAccess::useSyntheticBrowser(browser, scenario, runtime.trace());
        int callbacks = 0, failures = 0;
        QObject::connect(&browser, &BrowserAuth::callbackReady, [&] { ++callbacks; });
        QObject::connect(&browser, &BrowserAuth::failed, [&] { ++failures; });
        browser.begin(syntheticAuthorization());
        const QByteArray name = QByteArray("unrelated navigation is continued, not authorized: ") + scenario;
        check(eventually([&] { return commandCount(runtime.trace(), "Fetch.continueRequest") == 1; })
              && callbacks == 0 && failures == 0 && commandCount(runtime.trace(), "Fetch.failRequest") == 0,
              name.constData());
        browser.cancel();
        ++cases;
    }
    for (const auto& scenario : {"unowned-first", "duplicate-event"}) {
        SyntheticRuntime runtime;
        BrowserAuth browser;
        BrowserAuthTestAccess::useSyntheticBrowser(browser, scenario, runtime.trace());
        int callbacks = 0, failures = 0;
        QUrl captured;
        QObject::connect(&browser, &BrowserAuth::callbackReady, [&](const OAuthContract::Callback& result) { const QUrl& url = result.url; ++callbacks; captured = url; });
        QObject::connect(&browser, &BrowserAuth::failed, [&] { ++failures; });
        browser.begin(syntheticAuthorization());
        check(eventually([&] {
                  return commandCount(runtime.trace(), "Page.navigate") == 2
                      && (QString(scenario) != "duplicate-event"
                          || commandCount(runtime.trace(), "Fetch.failRequest")
                             + commandCount(runtime.trace(), "Fetch.continueRequest") == 2);
              }) && callbacks == 1 && failures == 0
              && captured == QUrl(CallbackEndpoint + "?code=synthetic-code&state=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
              scenario == QString("unowned-first") ? "unowned session cannot satisfy sign-in"
                                                    : "duplicate callback cannot complete twice");
        ++cases;
    }
    {
        SyntheticRuntime runtime;
        BrowserAuth browser;
        BrowserAuthTestAccess::useSyntheticBrowser(browser, "code", runtime.trace());
        QStringList failureMessages;
        QObject::connect(&browser, &BrowserAuth::failed, [&](const QString& message) { failureMessages.append(message); });
        QList<QUrl> invalid;
        auto url = syntheticAuthorization(); url.setHost("attacker.invalid"); invalid.append(url);
        url = syntheticAuthorization(); url.setUserName("untrusted"); invalid.append(url);
        url = syntheticAuthorization(); url.setPort(444); invalid.append(url);
        url = syntheticAuthorization(); url.setScheme("http"); invalid.append(url);
        url = syntheticAuthorization(); url.setPath("/common/oauth2/v2.0/authorize/extra"); invalid.append(url);
        url = syntheticAuthorization(); url.setFragment("fragment"); invalid.append(url);
        url = syntheticAuthorization(); url.setQuery(url.query() + "&state=second"); invalid.append(url);
        QStringList expectedMessages;
        for (const auto& request : invalid) {
            browser.begin(request);
            expectedMessages.append("The connection supplied an unsupported or unsafe Microsoft sign-in request.");
        }
        check(failureMessages == expectedMessages, "unsafe authorization reports its exact failure synchronously");
        eventually([] { return false; }, 300); // Allow mistakenly queued startup work to reach the fixture.
        check(!QFileInfo::exists(runtime.trace())
              && QDir(runtime.root.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty(),
              "unsafe authorization does not launch a child or create browser state after settling");
        cases += invalid.size();
    }
    for (const auto& scenario : {"download", "wrong-download-frame", "wrong-download-name", "second-download", "unarmed-download",
                                 "empty-progress-unarmed", "empty-progress-clicked", "foreign-progress-receiving"}) {
        SyntheticRuntime runtime;
        BrowserAuth browser;
        BrowserAuthTestAccess::useSyntheticBrowser(browser, scenario, runtime.trace());
        int downloads = 0, failures = 0, choices = 0;
        bool privateBytes = false;
        QString deliveredPath;
        QObject::connect(&browser, &BrowserAuth::failed, [&] { ++failures; });
        QObject::connect(&browser, &BrowserAuth::resourcesAvailable,
            [&](const QStringList& ids, const QStringList& names) {
                if (ids.isEmpty()) return;
                ++choices;
                check(ids == QStringList{"first", "second"} && names == QStringList{"First PC", "Second PC"},
                      "acquisition publishes discovered Cloud PC choices");
                browser.selectResource("first");
                browser.selectResource("first");
            });
        QObject::connect(&browser, &BrowserAuth::profileDownloaded,
            [&](const QString& path, const QString& name) {
                ++downloads; deliveredPath = path;
                QFile file(path);
                struct stat info{};
                privateBytes = file.open(QIODevice::ReadOnly)
                    && file.readAll() == "full address:s:synthetic.invalid\ngatewayhostname:s:synthetic.invalid\narm path:s:synthetic\n"
                    && name == "First PC" && ::stat(QFile::encodeName(path).constData(), &info) == 0
                    && (info.st_mode & 0777) == 0600;
            });
        browser.beginProfileDownload();
        const QByteArray name = QByteArray("acquisition accepts exactly one private connection file: ") + scenario;
        check(eventually([&] { return downloads || failures; }) && downloads == 1 && failures == 0
              && choices == 1 && privateBytes && !QFileInfo::exists(deliveredPath), name.constData());
        if (QString(scenario) == "wrong-download-frame" || QString(scenario) == "wrong-download-name"
            || QString(scenario) == "second-download" || QString(scenario) == "unarmed-download")
            check(eventually([&] { return cancellationsFor(runtime.trace(), "22222222-2222-2222-2222-222222222222") == 1; }),
                  "rejected 2222 GUID is canceled independently of the accepted download");
        check(commandCount(runtime.trace(), "Runtime.evaluate") == 2,
              "repeated native selection arms only one download click");
        if (QString(scenario).contains("progress")) {
            bool emptyProgress = false, foreignProgress = false;
            for (const auto& item : transcript(runtime.trace())) {
                if (item.value("event") != "Browser.downloadProgress") continue;
                const QString guid = item.value("params").toObject().value("guid").toString();
                if (guid.isEmpty()) emptyProgress = true;
                if (guid == "22222222-2222-2222-2222-222222222222") foreignProgress = true;
            }
            check(emptyProgress && (QString(scenario) != "foreign-progress-receiving" || foreignProgress),
                  "empty/foreign GUID progress stimuli reached the process seam");
        }
        ++cases;
    }
    for (const auto& scenario : {"empty-file", "oversize-file", "symlink-file", "hardlink-file",
                                 "destination-exists", "oversize-progress", "canceled-download", "invalid-guid", "navigate-after-click"}) {
        SyntheticRuntime runtime;
        BrowserAuth browser;
        BrowserAuthTestAccess::useSyntheticBrowser(browser, scenario, runtime.trace());
        int downloads = 0, failures = 0;
        QString failureMessage;
        QObject::connect(&browser, &BrowserAuth::resourcesAvailable, [&](const QStringList& ids, const QStringList&) {
            if (!ids.isEmpty()) browser.selectResource("first");
        });
        QObject::connect(&browser, &BrowserAuth::profileDownloaded, [&] { ++downloads; });
        QObject::connect(&browser, &BrowserAuth::failed, [&](const QString& message) { ++failures; failureMessage = message; });
        browser.beginProfileDownload();
        const QByteArray name = QByteArray("unsafe or incomplete connection download fails without delivery: ") + scenario;
        check(eventually([&] { return failures || downloads; }) && failures == 1 && downloads == 0
              && emitted(runtime.trace(), QString(scenario) == "invalid-guid"
                         ? "Browser.downloadWillBegin" : "Browser.downloadProgress"), name.constData());
        if (QString(scenario) == "symlink-file")
            check(failureMessage == "The portal did not produce a safe, complete Cloud PC connection file.",
                  "unsafe download preserves its exact failure message");
        ++cases;
    }
    for (const auto& scenario : {"portal-wrong-origin", "portal-wrong-path", "portal-subframe", "portal-popup"}) {
        SyntheticRuntime runtime;
        BrowserAuth browser;
        BrowserAuthTestAccess::useSyntheticBrowser(browser, scenario, runtime.trace());
        int choices = 0, downloads = 0;
        QObject::connect(&browser, &BrowserAuth::resourcesAvailable, [&] { ++choices; });
        QObject::connect(&browser, &BrowserAuth::profileDownloaded, [&] { ++downloads; });
        browser.beginProfileDownload();
        const bool navigated = eventually([&] { return commandCount(runtime.trace(), "Page.navigate") == 1; });
        eventually([] { return false; }, 1700); // More than two 750 ms portal polling intervals.
        const QByteArray name = QByteArray("portal automation never enters a foreign document: ") + scenario;
        check(navigated && choices == 0 && downloads == 0
              && commandCount(runtime.trace(), "Page.createIsolatedWorld") == 0
              && commandCount(runtime.trace(), "Runtime.evaluate") == 0, name.constData());
        browser.cancel();
        ++cases;
    }
    {
        SyntheticRuntime runtime;
        int callbacks = 0, failures = 0;
        QUrl second;
        {
            BrowserAuth browser;
            BrowserAuthTestAccess::useSyntheticBrowser(browser, "handoff", runtime.trace());
            QObject::connect(&browser, &BrowserAuth::callbackReady, [&](const OAuthContract::Callback& result) { const QUrl& url = result.url;
                ++callbacks;
                if (callbacks == 1) browser.begin(syntheticAuthorization());
                else second = url;
            });
            QObject::connect(&browser, &BrowserAuth::failed, [&] { ++failures; });
            browser.begin(syntheticAuthorization());
            check(eventually([&] { return callbacks == 2 || failures; }) && callbacks == 2 && failures == 0
                  && second == QUrl(CallbackEndpoint + "?code=synthetic-code&state=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
                  "reentrant sign-in replacement ignores retained-window callbacks");
            check(eventually([&] { return commandCount(runtime.trace(), "Target.closeTarget") == 1; }),
                  "completed sign-in window is retired after replacement");
            int creations = 0;
            bool orderedClose = false;
            for (const auto& item : transcript(runtime.trace())) {
                if (item.value("method") == "Target.createTarget") ++creations;
                if (item.value("method") == "Target.closeTarget" && item.value("params").toObject().value("targetId") == "page-1") {
                    orderedClose = creations == 2;
                    break;
                }
            }
            check(orderedClose, "external handoff creates a replacement before closing the last old window");
            browser.cancel();
            check(eventually([&] { return commandCount(runtime.trace(), "Target.closeTarget") == 2; }),
                  "cancel after completed sign-in closes the remaining owned window");
        }
        const auto meta = transcript(runtime.trace()).value(0);
        const QString profile = meta.value("profile").toString();
        const pid_t pid = meta.value("pid").toInteger();
        check(!profile.isEmpty() && !QFileInfo::exists(profile) && pid > 0 && ::kill(pid, 0) < 0 && errno == ESRCH,
              "normal destruction reaps the owned browser before removing its private profile");
        check(QFileInfo::exists(runtime.root.path()), "normal cleanup does not remove the supplied runtime root");
        ++cases;
    }
    {
        SyntheticRuntime runtime;
        BrowserAuth browser;
        BrowserAuthTestAccess::useSyntheticBrowser(browser, "hold-create", runtime.trace());
        int callbacks = 0, failures = 0;
        QObject::connect(&browser, &BrowserAuth::callbackReady, [&] { ++callbacks; });
        QObject::connect(&browser, &BrowserAuth::failed, [&] { ++failures; });
        browser.begin(syntheticAuthorization());
        check(eventually([&] { return commandCount(runtime.trace(), "Target.createTarget") == 1; }),
              "cancellation fixture reaches an in-flight window creation");
        browser.cancel();
        browser.begin(syntheticAuthorization());
        check(eventually([&] { return callbacks || failures; }) && callbacks == 1 && failures == 0,
              "late create reply after cancel cannot revive the retired sign-in round");
        bool closedOld = false;
        for (const auto& item : transcript(runtime.trace()))
            if (item.value("method") == "Target.closeTarget" && item.value("params").toObject().value("targetId") == "page-1") closedOld = true;
        check(closedOld, "canceled in-flight window is eventually closed");
        ++cases;
    }
    {
        SyntheticRuntime runtime;
        BrowserAuth browser;
        BrowserAuthTestAccess::useSyntheticBrowser(browser, "hold-poll", runtime.trace());
        int choices = 0, downloads = 0, failures = 0;
        QObject::connect(&browser, &BrowserAuth::resourcesAvailable, [&](const QStringList& ids, const QStringList&) {
            if (ids.isEmpty()) return;
            ++choices;
            check(ids == QStringList{"first", "second"}, "stale portal probe never publishes retired choices");
            browser.selectResource("first");
        });
        QObject::connect(&browser, &BrowserAuth::profileDownloaded, [&] { ++downloads; });
        QObject::connect(&browser, &BrowserAuth::failed, [&] { ++failures; });
        browser.beginProfileDownload();
        check(eventually([&] { return commandCount(runtime.trace(), "Runtime.evaluate") == 1; }),
              "cancellation fixture reaches an in-flight portal probe");
        browser.cancel();
        browser.beginProfileDownload();
        check(eventually([&] { return downloads || failures; }) && choices == 1 && downloads == 1 && failures == 0,
              "canceled probe cannot satisfy replacement acquisition");
        ++cases;
    }
    {
        SyntheticRuntime runtime;
        BrowserAuth browser;
        BrowserAuthTestAccess::useSyntheticBrowser(browser, "download", runtime.trace());
        int choices = 0, downloads = 0, failures = 0;
        QObject::connect(&browser, &BrowserAuth::resourcesAvailable, [&](const QStringList& ids, const QStringList&) {
            if (ids.isEmpty()) return;
            ++choices;
            browser.selectResource("first");
            if (choices == 1) { browser.cancel(); browser.beginProfileDownload(); }
        });
        QObject::connect(&browser, &BrowserAuth::profileDownloaded, [&] { ++downloads; });
        QObject::connect(&browser, &BrowserAuth::failed, [&] { ++failures; });
        browser.beginProfileDownload();
        check(eventually([&] { return downloads || failures; }) && choices == 2 && downloads == 1 && failures == 0,
              "late download after reentrant cancel cannot satisfy replacement acquisition");
        ++cases;
    }
    {
        SyntheticRuntime runtime;
        BrowserAuth browser;
        BrowserAuthTestAccess::useSyntheticBrowser(browser, "download", runtime.trace());
        int downloads = 0, failures = 0;
        bool retainedDuringSignal = true;
        QString firstPath;
        QObject::connect(&browser, &BrowserAuth::resourcesAvailable, [&](const QStringList& ids, const QStringList&) {
            if (!ids.isEmpty()) browser.selectResource("first");
        });
        QObject::connect(&browser, &BrowserAuth::profileDownloaded, [&](const QString& path, const QString&) {
            ++downloads;
            if (downloads == 1) {
                firstPath = path;
                browser.beginProfileDownload();
                retainedDuringSignal = QFileInfo::exists(path);
            }
        });
        QObject::connect(&browser, &BrowserAuth::failed, [&] { ++failures; });
        browser.beginProfileDownload();
        check(eventually([&] { return downloads == 2 || failures; }) && downloads == 2 && failures == 0
              && retainedDuringSignal && !QFileInfo::exists(firstPath),
              "downloaded file survives reentrant acquisition until its synchronous consumer returns");
        ++cases;
    }
    for (const auto& scenario : {"child-exit", "malformed-json"}) {
        SyntheticRuntime runtime;
        BrowserAuth browser;
        BrowserAuthTestAccess::useSyntheticBrowser(browser, scenario, runtime.trace());
        int callbacks = 0, failures = 0;
        QString failureMessage;
        QObject::connect(&browser, &BrowserAuth::callbackReady, [&] { ++callbacks; });
        QObject::connect(&browser, &BrowserAuth::failed, [&](const QString& message) { ++failures; failureMessage = message; });
        browser.begin(syntheticAuthorization());
        const QByteArray name = QByteArray("owned transport failure reports once and cleans private state: ") + scenario;
        check(eventually([&] { return failures; }) && failures == 1 && callbacks == 0
              && eventually([&] {
                  const QString profile = transcript(runtime.trace()).value(0).value("profile").toString();
                  return !profile.isEmpty() && !QFileInfo::exists(profile);
              }), name.constData());
        check(failureMessage == (QString(scenario) == "child-exit"
              ? "The browser was closed before the current request completed."
              : "The sign-in browser sent an invalid protocol message."),
              "owned transport failure preserves its exact closed-browser or malformed-protocol message");
        ++cases;
    }
    {
        SyntheticRuntime runtime;
        QString profile, retiredDirectory;
        {
            BrowserAuth browser;
            BrowserAuthTestAccess::useSyntheticBrowser(browser, "symlink-file", runtime.trace());
            int failures = 0;
            QObject::connect(&browser, &BrowserAuth::resourcesAvailable, [&](const QStringList& ids, const QStringList&) {
                if (!ids.isEmpty()) browser.selectResource("first");
            });
            QObject::connect(&browser, &BrowserAuth::failed, [&] { ++failures; });
            browser.beginProfileDownload();
            check(eventually([&] { return failures; }), "cleanup fixture reaches a canceled unsafe download");
            profile = transcript(runtime.trace()).value(0).value("profile").toString();
            retiredDirectory = downloadDirectory(runtime.trace());
            check(!retiredDirectory.isEmpty() && QFileInfo(retiredDirectory).isDir(),
                  "canceled download keeps its actual download directory until child teardown");
        }
        check(!retiredDirectory.isEmpty() && !QFileInfo::exists(retiredDirectory)
              && !profile.isEmpty() && !QFileInfo::exists(profile) && QFileInfo::exists(runtime.root.path()),
              "normal destruction removes the actual retired download directory without removing the runtime root");
        ++cases;
    }
    {
        SyntheticRuntime runtime;
        {
            BrowserAuth browser;
            BrowserAuthTestAccess::useSyntheticBrowser(browser, "silent", runtime.trace());
            int callbacks = 0, failures = 0;
            QObject::connect(&browser, &BrowserAuth::callbackReady, [&] { ++callbacks; });
            QObject::connect(&browser, &BrowserAuth::failed, [&] { ++failures; });
            browser.begin(syntheticAuthorization());
            browser.cancel(); // QProcess is still Starting; no fixture reply has been read.
            const bool started = eventually([&] { return !transcript(runtime.trace()).isEmpty(); });
            eventually([] { return false; }, 300); // Observe several potential CDP round trips, not just child startup.
            check(started && callbacks == 0 && failures == 0
                  && commandCount(runtime.trace(), "Browser.setDownloadBehavior") == 0
                  && commandCount(runtime.trace(), "Target.createTarget") == 0,
                  "cancel during browser startup sends no flow preparation after settling");
        }
        const QString profile = transcript(runtime.trace()).value(0).value("profile").toString();
        check(!profile.isEmpty() && !QFileInfo::exists(profile), "startup cancellation is cleaned on normal teardown");
        ++cases;
    }
    std::printf("Public BrowserAuth retained synthetic-process scenarios: %d; assertions including storage: %d.\n", cases, assertions);
}
} // namespace
