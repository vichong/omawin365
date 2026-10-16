#include "signincache.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <cmath>
#include <cstring>
// GLib's signals macro collides with Qt's keyword macro.
#pragma push_macro("signals")
#undef signals
#include <libsecret/secret.h>
#pragma pop_macro("signals")

namespace {
constexpr qsizetype MaxBytes = 128 * 1024;
constexpr qsizetype MaxCookies = 128;
const SecretSchema schema = {
    "org.omawin365.SignIn", SECRET_SCHEMA_NONE,
    {{"purpose", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
    0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
};
bool clean(const QString& value, qsizetype max)
{
    if (value.size() > max) return false;
    for (QChar c : value)
        if (c.unicode() < 0x20 || c.unicode() == 0x7f) return false;
    return true;
}

QJsonArray validated(const QJsonArray& cookies, double now, bool includeSession)
{
    QJsonArray result;
    if (cookies.size() > MaxCookies || !std::isfinite(now)) return result;
    static const QRegularExpression namePattern(QStringLiteral("\\A[!#$%&'*+.^_`|~0-9A-Za-z-]{1,256}\\z"));
    for (const auto& value : cookies) {
        if (!value.isObject()) continue;
        const auto cookie = value.toObject();
        const QString domain = cookie.value("domain").toString();
        const QString name = cookie.value("name").toString();
        const QString contents = cookie.value("value").toString();
        const QString path = cookie.value("path").toString();
        const double expires = cookie.value("expires").toDouble(-1);
        // Canonical session cookies omit expires, as required by Storage.setCookies.
        const bool session = cookie.value("session").toBool(false) || !cookie.contains("expires");
        if ((domain != "login.microsoftonline.com" && domain != ".login.microsoftonline.com")
            || !cookie.value("secure").isBool() || !cookie.value("secure").toBool()
            || (session && !includeSession) || cookie.contains("partitionKey")
            || cookie.value("partitionKeyOpaque").toBool(false)
            || (!session && (!cookie.value("expires").isDouble() || !std::isfinite(expires) || expires <= now))
            || !namePattern.match(name).hasMatch() || !cookie.value("value").isString()
            || !clean(contents, 16384) || contents.contains(';')
            || !path.startsWith('/') || !clean(path, 2048)
            || !cookie.value("httpOnly").isBool()) continue;
        QJsonObject saved{{"name", name}, {"value", contents}, {"domain", domain},
            {"path", path}, {"secure", true}, {"httpOnly", cookie.value("httpOnly")}};
        if (!session) saved.insert("expires", expires);
        if (cookie.contains("sameSite")) {
            const QString sameSite = cookie.value("sameSite").toString();
            if (sameSite != "Strict" && sameSite != "Lax" && sameSite != "None") continue;
            saved.insert("sameSite", sameSite);
        }
        result.append(saved);
    }
    if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() > MaxBytes) return {};
    return result;
}
}

namespace SignInCookies {
QJsonArray persistent(const QJsonArray& cookies, double now) { return validated(cookies, now, false); }
QJsonArray forSession(const QJsonArray& cookies, double now) { return validated(cookies, now, true); }
bool hasPersistentAuthentication(const QJsonArray& cookies)
{
    for (const auto& cookie : cookies) {
        const auto name = cookie.toObject().value("name").toString();
        if (name == "ESTSAUTHPERSISTENT" || name == "estsStatePersistent") return true;
    }
    return false;
}
}

QByteArray SignInCookies::encode(const QJsonArray& cookies, double now)
{
    return QJsonDocument(QJsonObject{{"version", 1}, {"cookies", persistent(cookies, now)}})
        .toJson(QJsonDocument::Compact);
}

QJsonArray SignInCookies::decode(const QByteArray& data, double now)
{
    if (data.size() > MaxBytes) return {};
    const auto object = QJsonDocument::fromJson(data).object();
    if (object.value("version").toInt() != 1 || !object.value("cookies").isArray()) return {};
    return persistent(object.value("cookies").toArray(), now);
}

struct SignInCache::Result {
    GCancellable* cancel = g_cancellable_new();
    QByteArray data;
    bool ok = false;
    bool forgotten = false; // Main thread only.
    ~Result() { g_object_unref(cancel); }
};

SignInCache::SignInCache(QObject* parent) : QObject(parent)
{
    timeout.setSingleShot(true);
    timeout.setInterval(15000);
    connect(&timeout, &QTimer::timeout, this, [this] {
        if (current) g_cancellable_cancel(current->cancel);
    });
}

SignInCache::~SignInCache()
{
    if (thread) {
        disconnect(thread, nullptr, this, nullptr);
        g_cancellable_cancel(current->cancel);
        thread->wait();
        delete thread;
    }
}

void SignInCache::load() { enqueue({Kind::Load, {}}); }
void SignInCache::save(const QByteArray& data)
{
    if (data.size() > MaxBytes) { emit saved(false, false); return; }
    // Several authorization rounds can finish in one connection. Keep the newest
    // pending snapshot, without changing ordering relative to load/clear jobs.
    if (!jobs.isEmpty() && jobs.back().kind == Kind::Save) jobs.back().data = data;
    else enqueue({Kind::Save, data});
}
void SignInCache::clear()
{
    // A load already running may have read the item before it is deleted.
    if (current) current->forgotten = true;
    jobs.clear();
    enqueue({Kind::Clear, {}});
}
void SignInCache::enqueue(Job job)
{
    jobs.enqueue(std::move(job));
    next();
}
void SignInCache::next()
{
    if (thread || jobs.isEmpty()) return;
    const Job job = jobs.dequeue();
    current = std::make_shared<Result>();
    const auto result = current;
    thread = QThread::create([job, result] {
        GError* error = nullptr;
        if (job.kind == Kind::Load) {
            gchar* secret = secret_password_lookup_sync(&schema, result->cancel, &error,
                "purpose", "entra-persistent-cookies-v1", nullptr);
            if (secret) {
                const size_t length = strnlen(secret, MaxBytes + 1);
                if (length <= size_t(MaxBytes)) result->data = QByteArray(secret, qsizetype(length));
                else result->ok = false;
                secret_password_free(secret);
                result->ok = !error && length <= size_t(MaxBytes);
            } else result->ok = !error; // A missing item is a normal first sign-in.
        } else if (job.kind == Kind::Save) {
            result->ok = secret_password_store_sync(&schema, SECRET_COLLECTION_DEFAULT,
                "OMAWIN365 Microsoft sign-in", job.data.constData(), result->cancel, &error,
                "purpose", "entra-persistent-cookies-v1", nullptr);
        } else {
            secret_password_clear_sync(&schema, result->cancel, &error,
                "purpose", "entra-persistent-cookies-v1", nullptr);
            result->ok = !error; // Already absent is also forgotten.
        }
        // Service errors may contain identifying data. Publish only a boolean.
        if (error) g_error_free(error);
    });
    connect(thread, &QThread::finished, this, [this, job, result] {
        timeout.stop();
        auto* finished = thread;
        thread = nullptr;
        current.reset();
        finished->deleteLater();
        if (job.kind == Kind::Load) emit loaded(result->forgotten ? QByteArray() : result->data, result->ok);
        else if (job.kind == Kind::Save) {
            const auto cookies = QJsonDocument::fromJson(job.data).object().value("cookies").toArray();
            emit saved(result->ok, SignInCookies::hasPersistentAuthentication(cookies));
        }
        else emit cleared(result->ok);
        next();
    });
    timeout.start();
    thread->start();
}
