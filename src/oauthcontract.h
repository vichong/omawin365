#pragma once
#include <QUrl>
#include <QString>
#include <QByteArray>
#include <optional>
#include <utility>
#include <QMetaType>

// Pure, version-specific wire contract. No verifier or token belongs here.
namespace OAuthContract {
struct Request {
    QUrl authorization;
    QString state;
    QString challenge;
    QString scope;
    quint64 generation = 0;
    Request() = default;
    Request(const QUrl& url) : authorization(url) {}
    Request(QUrl url, QString s, QString c, quint64 g = 0)
        : authorization(std::move(url)), state(std::move(s)), challenge(std::move(c)), generation(g) {}
};
struct Callback {
    QUrl url;
    quint64 generation = 0;
    Callback() = default;
    Callback(const QUrl& u, quint64 g = 0) : url(u), generation(g) {}
};
enum class Kind { NotCallback, Invalid, Code, Error };
struct Reply { Kind kind = Kind::Invalid; QString code; QString state; };
std::optional<Request> request(const QUrl& url);
Reply reply(const QUrl& url, const QString& expectedState);
QByteArray serialize(const Reply& reply); // Includes newline; empty on failure/overflow.
bool callback(const QUrl& url);
}

Q_DECLARE_METATYPE(OAuthContract::Request)
Q_DECLARE_METATYPE(OAuthContract::Callback)
