#include "promptparser.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace {
void require(bool condition)
{
    if (!condition)
        std::abort();
}

bool same(const PromptParser::Event& a, const PromptParser::Event& b)
{
    if (a.authorization.has_value() != b.authorization.has_value()) return false;
    if (a.authorization && (a.authorization->authorization != b.authorization->authorization ||
        a.authorization->state != b.authorization->state || a.authorization->challenge != b.authorization->challenge ||
        a.authorization->scope != b.authorization->scope || a.authorization->generation != b.authorization->generation)) return false;
    return a.kind == b.kind && a.url == b.url && a.host == b.host && a.port == b.port
        && a.fingerprint == b.fingerprint && a.changed == b.changed && a.detail == b.detail;
}

void compare(const QList<PromptParser::Event>& a, const QList<PromptParser::Event>& b,
             PromptParser::Mode mode)
{
    require(a.size() == b.size());
    for (qsizetype i = 0; i < a.size(); ++i) {
        require(same(a[i], b[i]));
        const auto& e = a[i];
        if (mode == PromptParser::Mode::DiagnosticsOnly)
            require(e.kind == PromptParser::Kind::Diagnostic || e.kind == PromptParser::Kind::InvalidPrompt);
        if (e.kind == PromptParser::Kind::Authorization)
            require(PromptParser::validAuthorization(e.url) && e.authorization.has_value() &&
                    e.authorization->authorization == e.url && e.authorization->generation == 0);
        if (e.kind == PromptParser::Kind::Certificate)
            require(!e.host.isEmpty() && e.host.size() <= 253 && e.port && e.fingerprint.size() == 32);
        if (e.kind == PromptParser::Kind::Diagnostic)
            require(e.detail == QStringLiteral("FreeRDP reported authentication failure.")
                    || e.detail == QStringLiteral("FreeRDP reported a transport connection failure.")
                    || e.detail == QStringLiteral("FreeRDP reported a security negotiation failure."));
    }
}

QList<PromptParser::Event> chunked(PromptParser& parser, const QByteArray& bytes, unsigned seed)
{
    QList<PromptParser::Event> events;
    for (qsizetype offset = 0; offset < bytes.size();) {
        // An input-derived deterministic schedule, including one-byte chunks.
        seed = seed * 1664525U + 1013904223U;
        const qsizetype count = qMin(bytes.size() - offset, qsizetype(1 + (seed % 512)));
        events.append(parser.feed(bytes.mid(offset, count)));
        offset += count;
    }
    return events;
}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    constexpr size_t maximumInput = 65540;
    if (size < 4 || size > maximumInput)
        return 0;
    const unsigned seed = unsigned(data[0]) | (unsigned(data[1]) << 8);
    const QByteArray bytes(reinterpret_cast<const char*>(data + 4), qsizetype(size - 4));
    const qsizetype cut = bytes.isEmpty() ? 0 :
        (unsigned(data[2]) | (unsigned(data[3]) << 8)) % (bytes.size() + 1);
    for (const auto mode : {PromptParser::Mode::Interactive, PromptParser::Mode::DiagnosticsOnly}) {
        PromptParser whole(mode), pieces(mode);
        compare(whole.feed(bytes), chunked(pieces, bytes, seed), mode);
        // There is no finish API: EOF is an empty feed, not an implicit newline.
        require(whole.feed({}).isEmpty());
        require(pieces.feed({}).isEmpty());
        compare(whole.feed("\n"), pieces.feed("\n"), mode);
        whole.reset();
        pieces.reset();
        whole.feed(bytes.left(cut));
        chunked(pieces, bytes.left(cut), seed);
        whole.reset();
        pieces.reset();
        compare(whole.feed(bytes.mid(cut)), chunked(pieces, bytes.mid(cut), seed), mode);
        whole.reset();
        pieces.reset();
        const auto recovery = whole.feed("FIDO2 PIN: ");
        compare(recovery, pieces.feed("FIDO2 PIN: "), mode);
        require(mode == PromptParser::Mode::Interactive
                    ? recovery.size() == 1 && recovery.first().kind == PromptParser::Kind::Pin
                    : recovery.isEmpty());
    }
    // Reach both real public URL validators with arbitrary encoded input.
    const QUrl url = QUrl::fromEncoded(bytes, QUrl::StrictMode);
    (void)PromptParser::validAuthorization(url);
    (void)PromptParser::validCallback(url);
    return 0;
}
