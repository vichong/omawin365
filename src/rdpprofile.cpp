#include "rdpprofile.h"

#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QStringConverter>
#include <QStringView>

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
    static const QRegularExpression ipv4(QStringLiteral("\\A[0-9]+(?:\\.[0-9]+){3}\\z"));
    if (ipv4.matchView(value).hasMatch())
        return false; // IP literals are not in the initial DNS-only policy.
    for (const auto part : value.split(u'.')) {
        if (!label.matchView(part).hasMatch())
            return false;
    }
    return true;
}

// One declaration owns each key's presence, type and complete value grammar.
// Literal entries deliberately do not admit upstream ranges or arbitrary strings.
enum class Value { Dns, Gateway, Arm, Routing, Program, Uuid, Literal, Desktop, Activity, Hub };
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
    {"geo", 's', Value::Literal, "EU"},
    {"alternate full address", 's', Value::Dns}, // Also compared to full address after all lines.
    {"diagnosticserviceurl", 's', Value::Literal, "https://rdweb-g-eu-r1.wvd.microsoft.com/api/arm/DiagnosticEvents/v1"},
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
    {"enablerdsaadauth", 'i', Value::Literal, "0"},
    {"clientrejectinjectedinput", 'i', Value::Literal, "0"},
    {"rdgiskdcproxy", 'i', Value::Literal, "0"},
    {"camerastoredirect", 's', Value::Literal, "*"},
    {"devicestoredirect", 's', Value::Literal, "*"},
    {"drivestoredirect", 's', Value::Literal, "*"},
    {"usbdevicestoredirect", 's', Value::Literal, "*"},
};

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
    static const QRegularExpression desktop(QStringLiteral("\\ACloud PC Enterprise ") + positive +
        QStringLiteral("vCPU/") + positive + QStringLiteral("GB/") + positive + QStringLiteral("GB\\z"));
    // Match encoded punctuation literally: no decoding, alternate JSON or URL forms.
    static const QRegularExpression activity(QStringLiteral("\\Ams-wvd-ep:") + uuid +
        QStringLiteral("\\?ScaleUnitPath=\\{\"Geo\"%3a\"EU\"%2c\"Ring\"%3a[0-9]%2c\"Region\"%3a\"westeurope\"%2c\"ScaleUnit\"%3a") +
        positive + QStringLiteral("\\}\\z"));
    static const QRegularExpression hub(QStringLiteral("\\Ahttps://rdweb-g-eu-r1\\.wvd\\.microsoft\\.com/api/arm/hubdiscovery\\?resourceId=") + uuid + QStringLiteral("\\z"));
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
    case Value::Desktop: return desktop.matchView(value).hasMatch();
    case Value::Activity: return activity.matchView(value).hasMatch();
    case Value::Hub: return hub.matchView(value).hasMatch();
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
    return true;
}

bool RdpProfile::readAndValidate(const QString& path, QByteArray* bytes, QString* error)
{
    if (error)
        error->clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > maximumBytes)
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
