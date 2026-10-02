#include "oauthcontract.h"
#include <QMap>
#include <QRegularExpression>
#include <QStringDecoder>

namespace {
const QString native = QStringLiteral("https://login.microsoftonline.com/common/oauth2/nativeclient");
bool origin(const QUrl& u) {
    return u.isValid() && u.scheme() == QStringLiteral("https") &&
        u.host() == QStringLiteral("login.microsoftonline.com") &&
        (u.port(-1) == -1 || u.port() == 443) && !u.authority(QUrl::FullyEncoded).contains('@') && !u.hasFragment();
}
int hex(char c) {
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'a' && c <= 'f') return c-'a'+10;
    if (c >= 'A' && c <= 'F') return c-'A'+10;
    return -1;
}
bool decode(const QByteArray& input, QString& out) {
    QByteArray bytes;
    for (qsizetype i=0; i<input.size(); ++i) {
        char c=input[i];
        if (c=='%') {
            if (i+2>=input.size() || hex(input[i+1])<0 || hex(input[i+2])<0) return false;
            c=char((hex(input[i+1])<<4)|hex(input[i+2])); i+=2;
        } else if (c=='+') c=' ';
        bytes.append(c);
    }
    // Each query field is complete: unfinished UTF-8 must fail, not wait for a next chunk.
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    out=decoder(bytes);
    if (decoder.hasError()) return false;
    for (QChar c:out) if (c.category()==QChar::Other_Control) return false;
    return true;
}
bool fields(const QUrl& u, QMap<QString,QString>& values) {
    if (!u.isValid() || u.toEncoded().size()>32768 || !u.hasQuery()) return false;
    const QByteArray query=u.query(QUrl::FullyEncoded).toUtf8();
    for (const QByteArray& part:query.split('&')) {
        const auto equals=part.indexOf('=');
        QString key,value;
        if (equals<1 || !decode(part.left(equals),key) || key.isEmpty() ||
            !decode(part.mid(equals+1),value) || values.contains(key)) return false;
        values.insert(key,value);
    }
    return true;
}
bool shape(const QString& text,const char* pattern) {
    return QRegularExpression(QString::fromLatin1(pattern)).match(text).hasMatch();
}
QByteArray encode(const QString& text) {
    QByteArray out;
    const char* digits="0123456789ABCDEF";
    for (unsigned char c:text.toUtf8()) {
        if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='.'||c=='_'||c=='~') out.append(char(c));
        else { out.append('%'); out.append(digits[c>>4]); out.append(digits[c&15]); }
    }
    return out;
}
}
namespace OAuthContract {
std::optional<Request> request(const QUrl& u) {
    QMap<QString,QString> f;
    if (!origin(u) || !shape(u.path(QUrl::FullyEncoded),"^/[A-Za-z0-9][A-Za-z0-9.-]{0,254}/oauth2/v2\\.0/authorize$") || !fields(u,f) || f.size()!=7) return {};
    if (f.value("client_id")!="a85cf173-4192-42f8-81fa-777a763e6e2c" || f.value("response_type")!="code" ||
        f.value("redirect_uri")!=native || f.value("code_challenge_method")!="S256" ||
        !shape(f.value("state"),"^[0-9a-fA-F]{64}$") || !shape(f.value("code_challenge"),"^[A-Za-z0-9_-]{43}$")) return {};
    const QString scope=f.value("scope");
    if (scope!="https://www.wvd.microsoft.com/.default openid profile offline_access" &&
        !shape(scope,"^ms-device-service://termsrv\\.wvd\\.microsoft\\.com/name/[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?/user_impersonation$")) return {};
    Request result{u,f.value("state"),f.value("code_challenge"),0};
    result.scope = scope;
    return result;
}
Reply reply(const QUrl& u,const QString& state) {
    if (!u.isValid()) return {};
    QUrl endpoint = u;
    endpoint.setFragment(QString());
    if (!origin(endpoint) || u.path(QUrl::FullyEncoded)!="/common/oauth2/nativeclient") return {Kind::NotCallback,{},{}};
    if (u.hasFragment()) return {};
    QMap<QString,QString> f;
    if (!fields(u,f) || !shape(state,"^[0-9a-fA-F]{64}$") || f.value("state")!=state) return {};
    const bool code=f.contains("code"), error=f.contains("error");
    if (code==error) return {};
    if (error) return f.value("error").isEmpty()?Reply{}:Reply{Kind::Error,{},state};
    if (f.value("code").isEmpty()) return {};
    Reply result{Kind::Code,f.value("code"),state};
    if (serialize(result).isEmpty()) return {};
    return result;
}
QByteArray serialize(const Reply& r) {
    if (r.kind!=Kind::Code || r.code.isEmpty() || !shape(r.state,"^[0-9a-fA-F]{64}$")) return {};
    if (QString::fromUtf8(r.code.toUtf8()) != r.code) return {};
    for (QChar c:r.code) if (c.category()==QChar::Other_Control) return {};
    QByteArray line=native.toUtf8()+"?code="+encode(r.code)+"&state="+encode(r.state)+'\n';
    return line.size()<=4095?line:QByteArray{};
}
bool callback(const QUrl& u) {
    QMap<QString,QString> f;
    return fields(u,f) && reply(u,f.value("state")).kind==Kind::Code;
}
}
