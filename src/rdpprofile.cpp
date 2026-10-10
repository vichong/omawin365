#include "rdpprofile.h"

#include <QFile>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QStringConverter>
#include <QStringView>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
bool fail(QString* error, const char* message)
{
    if (error)
        *error = QString::fromLatin1(message);
    return false;
}

bool dns(QStringView value)
{
    if (value.isEmpty() || value.size() > 253)
        return false;
    static const QRegularExpression label(QStringLiteral("\\A[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\\z"));
    // IP literals are not in the initial DNS-only policy. libc's getaddrinfo also
    // accepts inet_aton forms (2130706433, 127.1, 0x7f.0.0.1) as IPv4 without a
    // lookup; all of them end in a decimal or 0x-hex label, as WHATWG URL hosts do.
    static const QRegularExpression number(QStringLiteral("\\A(?:[0-9]+|0[Xx][0-9A-Fa-f]*)\\z"));
    if (number.matchView(value.sliced(value.lastIndexOf(u'.') + 1)).hasMatch())
        return false;
    for (const auto part : value.split(u'.')) {
        if (!label.matchView(part).hasMatch())
            return false;
    }
    return true;
}

// One declaration owns each key's presence, type and complete value grammar.
// Literal entries deliberately do not admit upstream ranges or arbitrary strings.
enum class Value { Dns, Gateway, Arm, Routing, Program, Uuid, Literal, BooleanLiteral, Geo, Desktop, Diagnostic, Activity, Hub, OpaqueMetadata };
struct Rule {
    const char* key;
    char type;
    Value value;
    const char* literal = nullptr;
    bool required = false;
};
constexpr Rule rules[] = {
    {"full address", 's', Value::Dns, nullptr, true},
    {"gatewayhostname", 's', Value::Gateway, nullptr, true},
    {"armpath", 's', Value::Arm, nullptr, true},
    {"loadbalanceinfo", 's', Value::Routing, nullptr, true},
    {"remoteapplicationprogram", 's', Value::Program, nullptr, true},
    {"aadtenantid", 's', Value::Uuid},
    {"resourceprovider", 's', Value::Literal, "arm"},
    {"redirectwebauthn", 'i', Value::Literal, "1"},
    {"wvd endpoint pool", 's', Value::Uuid},
    {"workspace id", 's', Value::Uuid},
    {"geo", 's', Value::Geo},
    {"alternate full address", 's', Value::Dns}, // Also compared to full address after all lines.
    {"diagnosticserviceurl", 's', Value::Diagnostic},
    {"hubdiscoverygeourl", 's', Value::Hub},
    {"remotedesktopname", 's', Value::Desktop},
    {"activityhint", 's', Value::Activity},
    {"gatewayusagemethod", 'i', Value::Literal, "1"},
    {"gatewayprofileusagemethod", 'i', Value::Literal, "1"},
    {"authentication level", 'i', Value::Literal, "1"},
    {"gatewaybrokeringtype", 'i', Value::Literal, "1"},
    {"promptcredentialonce", 'i', Value::Literal, "1"},
    {"redirectclipboard", 'i', Value::Literal, "1"},
    {"redirectprinters", 'i', Value::Literal, "1"},
    {"redirectsmartcards", 'i', Value::Literal, "1"},
    {"dynamic resolution", 'i', Value::Literal, "1"},
    {"audiocapturemode", 'i', Value::Literal, "1"},
    {"redirectcomports", 'i', Value::Literal, "1"},
    {"singlemoninwindowedmode", 'i', Value::Literal, "1"},
    {"redirectlocation", 'i', Value::Literal, "1"},
    {"targetisaadjoined", 'i', Value::Literal, "1"},
    {"gatewaycredentialssource", 'i', Value::Literal, "0"},
    {"remoteapplicationmode", 'i', Value::Literal, "0"},
    {"audiomode", 'i', Value::Literal, "0"},
    {"enablerdsaadauth", 'i', Value::BooleanLiteral},
    {"forcehidpioptimizations", 'i', Value::BooleanLiteral},
    {"clientrejectinjectedinput", 'i', Value::Literal, "0"},
    {"rdgiskdcproxy", 'i', Value::Literal, "0"},
    {"camerastoredirect", 's', Value::Literal, "*"},
    {"devicestoredirect", 's', Value::Literal, "*"},
    {"drivestoredirect", 's', Value::Literal, "*"},
    {"usbdevicestoredirect", 's', Value::Literal, "*"},
    {"signature", 's', Value::OpaqueMetadata},
    {"signscope", 's', Value::OpaqueMetadata},
};

bool opaqueMetadata(QStringView value)
{
    // Global control/line parsing and supportedValue's ASCII guard constrain
    // this single-line value to printable ASCII. No decoding or trust verdict;
    // the unchanged whole-file byte limit is the sole size budget.
    return !value.isEmpty() && !value.startsWith(u' ') && !value.endsWith(u' ');
}

const Rule* findRule(const QString& key)
{
    for (const auto& rule : rules) {
        if (key == QLatin1StringView(rule.key))
            return &rule;
    }
    return nullptr;
}

bool supportedValue(const Rule& rule, QStringView value)
{
    // PCRE Unicode caseless equivalents must not extend the ASCII-only rules.
    for (const QChar c : value) {
        if (c.unicode() > 0x7f)
            return false;
    }
    static const QString uuid = QStringLiteral("[A-Fa-f0-9]{8}-[A-Fa-f0-9]{4}-[A-Fa-f0-9]{4}-[A-Fa-f0-9]{4}-[A-Fa-f0-9]{12}");
    static const QString positive = QStringLiteral("[1-9][0-9]{0,3}");
    static const QRegularExpression identifier(QStringLiteral("\\A") + uuid + QStringLiteral("\\z"));
    static const QRegularExpression routing(QStringLiteral("\\Amth://localhost/") + uuid + u'/' + uuid + QStringLiteral("\\z"));
    static const QRegularExpression program(QStringLiteral("\\A\\|\\|") + uuid + QStringLiteral("\\z"));
    // Identifier limits are app policy, not the entire Azure resource-name grammar.
    static const QRegularExpression arm(QStringLiteral("\\A(?i:/subscriptions/)") + uuid +
        QStringLiteral("(?i:/resourcegroups/)[A-Za-z0-9_-]{1,90}(?i:/providers/Microsoft\\.DesktopVirtualization/hostpools/)[A-Za-z0-9_-]{1,64}\\z"));
    // Display label only. The product name varies by Microsoft SKU; an optional size suffix keeps its grammar.
    static const QRegularExpression desktop(QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9 ._-]{0,63}(?: ") + positive +
        QStringLiteral("vCPU/") + positive + QStringLiteral("GB/") + positive + QStringLiteral("GB)?\\z"));
    // Bounded stored-metadata policy, not a geography enum or trusted endpoint registry.
    static const QString token = QStringLiteral("[A-Za-z][A-Za-z0-9_]{0,31}");
    static const QString authority = QStringLiteral("rdweb-g-[a-z]{2,16}-r[0-9]\\.(?:wvd\\.microsoft\\.com|wvd\\.azure\\.us)");
    static const QRegularExpression geo(QStringLiteral("\\A") + token + QStringLiteral("\\z"));
    static const QRegularExpression diagnostic(QStringLiteral("\\Ahttps://") + authority +
        QStringLiteral("/api/arm/DiagnosticEvents/v1\\z"));
    // Match encoded punctuation literally: no decoding, alternate JSON or URL forms.
    static const QRegularExpression activity(QStringLiteral("\\Ams-wvd-ep:") + uuid +
        QStringLiteral("\\?ScaleUnitPath=\\{\"Geo\"%3a\"") + token +
        QStringLiteral("\"%2c\"Ring\"%3a[0-9]%2c\"Region\"%3a\"[a-z](?:[a-z0-9-]{0,30}[a-z0-9])?\"%2c\"ScaleUnit\"%3a") +
        positive + QStringLiteral("\\}\\z"));
    static const QRegularExpression hub(QStringLiteral("\\Ahttps://") + authority +
        QStringLiteral("/api/arm/hubdiscovery\\?resourceId=") + uuid + QStringLiteral("\\z"));
    switch (rule.value) {
    case Value::Dns: return dns(value);
    case Value::Gateway:
        if (value.endsWith(u":443"))
            value.chop(4);
        return dns(value);
    case Value::Arm: return arm.matchView(value).hasMatch();
    case Value::Routing: return routing.matchView(value).hasMatch();
    case Value::Program: return program.matchView(value).hasMatch();
    case Value::Uuid: return identifier.matchView(value).hasMatch();
    case Value::Literal: return value == QLatin1StringView(rule.literal);
    case Value::BooleanLiteral: return value == u"0" || value == u"1";
    case Value::Geo: return geo.matchView(value).hasMatch();
    case Value::Desktop: return desktop.matchView(value).hasMatch();
    case Value::Diagnostic: return diagnostic.matchView(value).hasMatch();
    case Value::Activity: return activity.matchView(value).hasMatch();
    case Value::Hub: return hub.matchView(value).hasMatch();
    case Value::OpaqueMetadata: return opaqueMetadata(value);
    }
    return false;
}
}

bool RdpProfile::validate(const QByteArray& bytes, QString* error)
{
    if (error)
        error->clear();
    if (bytes.isEmpty() || bytes.size() > maximumBytes)
        return fail(error, "The connection file must be readable and no larger than 1 MiB.");
    const bool utf16 = bytes.startsWith(QByteArray::fromHex("fffe"));
    if (bytes.startsWith(QByteArray::fromHex("efbbbf")) || bytes.startsWith(QByteArray::fromHex("feff"))
        || bytes.startsWith(QByteArray::fromHex("fffe0000")) || bytes.startsWith(QByteArray::fromHex("0000feff"))
        || (utf16 && bytes.size() % 2 != 0))
        return fail(error, "The connection file has an unsupported or invalid encoding.");
    // Stateless detects incomplete sequences at EOF; preserve any second BOM so
    // the character check rejects it instead of silently consuming it.
    QStringDecoder decoder(utf16 ? QStringConverter::Utf16LE : QStringConverter::Utf8,
        QStringConverter::Flag::ConvertInitialBom | QStringConverter::Flag::Stateless);
    const QString text = decoder.decode(utf16 ? bytes.sliced(2) : bytes);
    if (decoder.hasError())
        return fail(error, "The connection file has an unsupported or invalid encoding.");
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == u'\r') {
            if (i + 1 >= text.size() || text.at(i + 1) != u'\n')
                return fail(error, "The connection file contains an invalid line separator.");
        } else if (c != u'\n' && (c.category() == QChar::Other_Control
                   || c.category() == QChar::Separator_Line || c.category() == QChar::Separator_Paragraph
                   || c.unicode() == 0xfeff)) {
            return fail(error, "The connection file contains an unsupported character.");
        }
    }
    QSet<QString> required;
    for (const auto& rule : rules) {
        if (rule.required)
            required.insert(QString::fromLatin1(rule.key));
    }
    QSet<QString> seen;
    QStringView fullAddress, alternateAddress;
    for (QStringView line : QStringView(text).split(u'\n')) {
        if (line.endsWith(u'\r'))
            line.chop(1);
        if (line.isEmpty())
            continue;
        const auto colon = line.indexOf(u':');
        if (colon <= 0 || colon + 2 >= line.size() || line.at(colon + 2) != u':')
            return fail(error, "The connection file contains a malformed setting.");
        // Fold ASCII only; Unicode case equivalents cannot introduce supported keys.
        QString key = line.first(colon).toString();
        for (QChar& c : key) {
            if (c >= u'A' && c <= u'Z')
                c = QChar(c.unicode() + ('a' - 'A'));
        }
        const Rule* rule = findRule(key);
        if (!rule)
            return fail(error, "The connection file contains an unsupported setting.");
        if (seen.contains(key))
            return fail(error, "The connection file contains a duplicate setting.");
        seen.insert(key);
        if (line.at(colon + 1) != QLatin1Char(rule->type))
            return fail(error, "The connection file contains an unsupported setting type.");
        const QStringView value = line.sliced(colon + 3);
        if (!supportedValue(*rule, value))
            return fail(error, "The connection file contains an invalid setting value.");
        if (key == QStringLiteral("full address"))
            fullAddress = value;
        else if (key == QStringLiteral("alternate full address"))
            alternateAddress = value;
    }
    // Both values passed ASCII DNS validation; this comparison cannot admit
    // Unicode case folding or a different endpoint, regardless of line order.
    if (!alternateAddress.isNull() && alternateAddress.compare(fullAddress, Qt::CaseInsensitive) != 0)
        return fail(error, "The connection file contains an invalid setting value.");
    if (!required.subtract(seen).isEmpty())
        return fail(error, "The connection file is missing a required supported setting.");
    if (seen.contains(QStringLiteral("signature")) != seen.contains(QStringLiteral("signscope")))
        return fail(error, "The connection file is missing a required supported setting.");
    return true;
}

bool RdpProfile::readAndValidate(const QString& path, QByteArray* bytes, QString* error)
{
    if (error)
        error->clear();
    // Callers check the pathname first. Open without blocking and require the
    // opened object to be a regular file, so a swapped-in FIFO cannot stall.
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (fd < 0)
        return fail(error, "The connection file must be readable and no larger than 1 MiB.");
    const auto closeFd = qScopeGuard([fd] { ::close(fd); });
    struct stat st {};
    QFile file;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || !file.open(fd, QIODevice::ReadOnly)
        || file.size() <= 0 || file.size() > maximumBytes)
        return fail(error, "The connection file must be readable and no larger than 1 MiB.");
    QByteArray captured = file.read(maximumBytes + 1);
    if (file.error() != QFileDevice::NoError || captured.size() > maximumBytes || !file.atEnd())
        return fail(error, "Could not read a complete RDP connection file.");
    if (!validate(captured, error))
        return false;
    if (bytes)
        *bytes = std::move(captured);
    return true;
}
