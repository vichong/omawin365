// The worker runs real queue/cancellation code against an in-memory Secret Service
// adapter. These tests must never look up or modify the developer's keyring.
#define secret_password_lookup_sync fixture_lookup
#define secret_password_store_sync fixture_store
#define secret_password_clear_sync fixture_clear
#define secret_password_free fixture_free
#include "../src/signincache.cpp"
#include <QTest>
#include <QSignalSpy>
#include <QMutex>
#include <QMutexLocker>
#include <atomic>
#include <cstring>

namespace {
QMutex fixtureMutex;
QByteArray fixtureSecret;
QStringList fixtureCalls;
std::atomic<bool> holdSave = false;
std::atomic<bool> saveEntered = false;
std::atomic<bool> holdLoad = false;
std::atomic<bool> loadEntered = false;
std::atomic<bool> unavailable = false;
void errorIfUnavailable(GError** error)
{
    if (unavailable) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Synthetic unavailable service");
}
}
extern "C" gchar* fixture_lookup(const SecretSchema*, GCancellable* cancel, GError** error, ...)
{
    loadEntered = true;
    while (holdLoad && !g_cancellable_is_cancelled(cancel)) QThread::msleep(1);
    QMutexLocker lock(&fixtureMutex);
    fixtureCalls.append("load");
    errorIfUnavailable(error);
    return unavailable || fixtureSecret.isEmpty() ? nullptr : g_strdup(fixtureSecret.constData());
}
extern "C" gboolean fixture_store(const SecretSchema*, const gchar*, const gchar*, const gchar* value,
                                   GCancellable* cancel, GError** error, ...)
{
    saveEntered = true;
    while (holdSave && !g_cancellable_is_cancelled(cancel)) QThread::msleep(1);
    QMutexLocker lock(&fixtureMutex);
    fixtureCalls.append("save");
    errorIfUnavailable(error);
    if (unavailable || g_cancellable_is_cancelled(cancel)) return false;
    fixtureSecret = value;
    return true;
}
extern "C" gboolean fixture_clear(const SecretSchema*, GCancellable*, GError** error, ...)
{
    QMutexLocker lock(&fixtureMutex);
    fixtureCalls.append("clear");
    errorIfUnavailable(error);
    if (unavailable) return false;
    fixtureSecret.clear();
    return true;
}
extern "C" void fixture_free(gchar* value) { g_free(value); }

class SignInCacheTest : public QObject {
    Q_OBJECT
    const double now = 2000000000;
    QJsonObject cookie() const
    {
        return {{"name", "ESTSAUTHPERSISTENT"}, {"value", "fictional-value"},
            {"domain", ".login.microsoftonline.com"}, {"path", "/"},
            {"expires", now + 3600}, {"secure", true}, {"httpOnly", true},
            {"session", false}, {"sameSite", "None"}, {"size", 123}};
    }
private slots:
    void init()
    {
        fixtureSecret.clear(); fixtureCalls.clear(); holdSave = false; saveEntered = false; holdLoad = false; loadEntered = false; unavailable = false;
    }
    void persistentCookieRoundTrip()
    {
        auto value = cookie();
        const auto encoded = SignInCookies::encode({value}, now);
        const auto restored = SignInCookies::decode(encoded, now);
        QCOMPARE(restored.size(), 1);
        value.remove("session"); value.remove("size");
        QCOMPARE(restored.first().toObject(), value);
        QVERIFY(SignInCookies::decode(encoded, now + 3601).isEmpty());
    }
    void unsafeCookiesAreExcluded()
    {
        const QList<QPair<QString, QJsonValue>> changes{
            {"domain", ".microsoftonline.com"}, {"domain", "login.microsoftonline.com.evil.invalid"},
            {"domain", "client.wvd.microsoft.com"}, {"domain", "other.login.microsoftonline.com"},
            {"secure", false}, {"session", true}, {"expires", -1}, {"expires", now},
            {"expires", "tomorrow"}, {"name", "bad;name"}, {"value", "bad\nvalue"},
            {"value", "bad;value"}, {"path", "relative"}, {"path", "/bad\npath"},
            {"sameSite", "invalid"}, {"partitionKeyOpaque", true}, {"partitionKey", QJsonObject{{"topLevelSite", "https://evil.invalid"}}}
        };
        for (const auto& change : changes) {
            auto value = cookie(); value.insert(change.first, change.second);
            QVERIFY2(SignInCookies::persistent({value}, now).isEmpty(), qPrintable(change.first));
        }
        auto value = cookie(); value.remove("expires");
        QVERIFY(SignInCookies::persistent({value}, now).isEmpty());
        value = cookie(); value.remove("httpOnly");
        QVERIFY(SignInCookies::persistent({value}, now).isEmpty());
    }
    void malformedAndOversizedCache()
    {
        QVERIFY(SignInCookies::decode("not json", now).isEmpty());
        QVERIFY(SignInCookies::decode("{\"version\":2,\"cookies\":[]}", now).isEmpty());
        QVERIFY(SignInCookies::decode(QByteArray(128 * 1024 + 1, 'x'), now).isEmpty());
        QJsonArray excessive;
        for (int i = 0; i < 129; ++i) excessive.append(cookie());
        QVERIFY(SignInCookies::persistent(excessive, now).isEmpty());
    }
    void serialStorageAndDeletion()
    {
        SignInCache cache;
        QSignalSpy loaded(&cache, &SignInCache::loaded);
        QSignalSpy saved(&cache, &SignInCache::saved);
        QSignalSpy cleared(&cache, &SignInCache::cleared);
        cache.save("fictional-cache"); cache.load();
        QTRY_COMPARE(loaded.count(), 1);
        QCOMPARE(saved.count(), 1);
        QCOMPARE(loaded.first().at(0).toByteArray(), "fictional-cache");
        QVERIFY(loaded.first().at(1).toBool());
        cache.clear();
        QTRY_COMPARE(cleared.count(), 1);
        QVERIFY(cleared.first().first().toBool());
        QVERIFY(fixtureSecret.isEmpty());
        cache.clear(); // Idempotent deletion.
        QTRY_COMPARE(cleared.count(), 2);
    }
    void forgetWinsAgainstPendingSaves()
    {
        SignInCache cache;
        QSignalSpy cleared(&cache, &SignInCache::cleared);
        QSignalSpy loaded(&cache, &SignInCache::loaded);
        holdSave = true;
        cache.save("in-flight");
        QTRY_VERIFY(saveEntered.load());
        cache.save("queued-stale-snapshot");
        cache.load(); // Forget must also discard queued reads, which will not signal loaded.
        cache.clear();
        holdSave = false;
        QTRY_COMPARE(cleared.count(), 1);
        QVERIFY(fixtureSecret.isEmpty());
        QCOMPARE(fixtureCalls, QStringList({"save", "clear"}));
        QCOMPARE(loaded.count(), 0);
    }
    void forgetDuringLoadReportsNothingSaved()
    {
        fixtureSecret = "read-before-forget";
        SignInCache cache;
        QSignalSpy loaded(&cache, &SignInCache::loaded);
        QSignalSpy cleared(&cache, &SignInCache::cleared);
        holdLoad = true;
        cache.load();
        QTRY_VERIFY(loadEntered.load());
        cache.clear();
        holdLoad = false;
        QTRY_COMPARE(cleared.count(), 1);
        QCOMPARE(loaded.count(), 1);
        QVERIFY(loaded.first().at(0).toByteArray().isEmpty()); // Never hands back forgotten sign-in.
        QCOMPARE(fixtureCalls, QStringList({"load", "clear"}));
    }
    void unavailableServiceHasNoFallback()
    {
        unavailable = true;
        SignInCache cache;
        QSignalSpy loaded(&cache, &SignInCache::loaded);
        QSignalSpy saved(&cache, &SignInCache::saved);
        QSignalSpy cleared(&cache, &SignInCache::cleared);
        cache.load(); cache.save("fictional-cache");
        QTRY_COMPARE(saved.count(), 1);
        QVERIFY(!loaded.first().at(1).toBool());
        QVERIFY(loaded.first().at(0).toByteArray().isEmpty());
        QVERIFY(!saved.first().first().toBool());
        cache.clear();
        QTRY_COMPARE(cleared.count(), 1);
        QVERIFY(!cleared.first().first().toBool());
        QVERIFY(fixtureSecret.isEmpty());
    }
    void destructionCancelsInFlightOperation()
    {
        auto cache = std::make_unique<SignInCache>();
        holdSave = true;
        cache->save("cancelled-write");
        QTRY_VERIFY(saveEntered.load());
        cache.reset(); // Cancellation must join the worker without waiting on UI events.
        QVERIFY(fixtureSecret.isEmpty());
        holdSave = false;
    }
};
QTEST_GUILESS_MAIN(SignInCacheTest)
#include "tst_signincache.moc"
