#include "theme.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QHash>
#include <QPalette>
#include <QProcess>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <array>

namespace {
using Values = QHash<QString, QString>;

// The shell's theme format is a flat table of scalar tokens, not application
// configuration. Ignore unsupported TOML constructs rather than interpreting them.
Values readTokens(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text) || file.size() > 256 * 1024)
        return {};
    QTextStream stream(&file);
    Values values;
    QString section;
    while (!stream.atEnd()) {
        QString line = stream.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        if (line.startsWith('[')) {
            const qsizetype end = line.indexOf(']');
            section = end > 1 ? line.mid(1, end - 1).trimmed() : QString();
            continue;
        }
        const qsizetype equals = line.indexOf('=');
        if (equals < 1)
            continue;
        const QString key = line.left(equals).trimmed();
        QString value = line.mid(equals + 1).trimmed();
        if (value.startsWith('"') || value.startsWith('\'')) {
            const QChar quote = value.front();
            const qsizetype end = value.indexOf(quote, 1);
            if (end < 0)
                continue;
            value = value.mid(1, end - 1);
        } else {
            value = value.section('#', 0, 0).trimmed();
        }
        values.insert(section.isEmpty() ? key : section + '.' + key, value);
    }
    return values;
}

double number(const Values& values, const QString& key, double fallback)
{
    bool ok = false;
    const double value = values.value(key).toDouble(&ok);
    return ok && std::isfinite(value) ? value : fallback;
}

QString tokenValue(const Values& values, QString token)
{
    // References in shell.toml are scalar tokens; bound recursion also handles
    // malformed cyclic user overrides without changing the fallback palette.
    for (int depth = 0; depth < 16 && values.contains(token); ++depth)
        token = values.value(token).trimmed();
    return token;
}

QColor resolve(const Values& values, const QString& token, const QColor& fallback)
{
    QString value = tokenValue(values, token).section(' ', 0, 0);
    if (value.compare("text", Qt::CaseInsensitive) == 0)
        value = tokenValue(values, "foreground");
    if (value.compare("transparent", Qt::CaseInsensitive) == 0)
        return QColor(Qt::transparent);
    static const QRegularExpression hyprHex(
        QStringLiteral("^(?:rgb\\(([0-9a-f]{6})\\)|rgba\\(([0-9a-f]{8})\\))$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = hyprHex.match(value);
    if (match.hasMatch())
        value = '#' + (match.captured(1).isEmpty() ? match.captured(2) : match.captured(1));
    if (value.startsWith("0x") && value.size() == 10)
        value = '#' + value.mid(4, 6) + value.mid(2, 2);
    // Omarchy uses #RRGGBBAA; QColor's eight-digit form is #AARRGGBB.
    if (value.startsWith('#') && value.size() == 9)
        value = '#' + value.right(2) + value.mid(1, 6);
    QColor color(value);
    if (!color.isValid()) {
        static const QRegularExpression rgb(
            QStringLiteral("^rgba?\\((\\d+),(\\d+),(\\d+)(?:,([0-9.]+))?\\)$"),
            QRegularExpression::CaseInsensitiveOption);
        const auto channels = rgb.match(value);
        if (channels.hasMatch()) {
            color = QColor(std::clamp(channels.captured(1).toInt(), 0, 255),
                std::clamp(channels.captured(2).toInt(), 0, 255),
                std::clamp(channels.captured(3).toInt(), 0, 255));
            if (!channels.captured(4).isEmpty())
                color.setAlphaF(std::clamp(channels.captured(4).toDouble(), 0.0, 1.0));
        }
    }
    return color.isValid() ? color : fallback;
}

QString css(const QColor& color)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alpha());
}

QColor withAlpha(QColor color, double alpha)
{
    color.setAlphaF(std::clamp(alpha, 0.0, 1.0));
    return color;
}

bool boolean(const Values& values, const QString& key, bool fallback)
{
    const QString value = values.value(key).trimmed().toLower();
    if (value == "true" || value == "1" || value == "yes" || value == "on") return true;
    if (value == "false" || value == "0" || value == "no" || value == "off") return false;
    return fallback;
}

using Widths = std::array<double, 4>;

Widths borderWidths(const Values& values, const QString& prefix, int fallback)
{
    static const QRegularExpression numeric(QStringLiteral("-?\\d+(?:\\.\\d+)?"));
    QList<double> numbers;
    auto matches = numeric.globalMatch(values.value(prefix + "-width"));
    while (matches.hasNext() && numbers.size() < 4)
        numbers.append(std::max(0.0, matches.next().captured().toDouble()));
    if (numbers.isEmpty()) numbers.append(fallback);
    Widths widths {numbers[0], numbers[0], numbers[0], numbers[0]};
    if (numbers.size() > 1) widths[1] = widths[3] = numbers[1];
    if (numbers.size() > 2) widths[2] = numbers[2];
    if (numbers.size() > 3) widths[3] = numbers[3];
    const QStringList sides {"top", "right", "bottom", "left"};
    for (int i = 0; i < 4; ++i) {
        const QString key = prefix + "-width-" + sides[i];
        if (!values.value(key).isEmpty())
            widths[i] = std::max(0.0, number(values, key, 0));
    }
    return widths;
}

QString borderBrush(const Values& values, const QString& token, const QColor& fallback, double alpha)
{
    const QString raw = tokenValue(values, token);
    const QStringList parts = raw.split(' ', Qt::SkipEmptyParts);
    QStringList colors;
    // BorderGeometry multiplies embedded alpha; shell fills/surfaces instead
    // replace it via Util.alpha. Keep that distinction for transparent tokens.
    const auto borderColor = [alpha](const QColor& color) {
        return css(withAlpha(color, color.alphaF() * std::clamp(alpha, 0.0, 1.0)));
    };
    double angle = 0;
    for (const QString& part : parts) {
        if (part.endsWith("deg")) {
            bool ok = false;
            const double parsed = part.chopped(3).toDouble(&ok);
            if (ok && std::isfinite(parsed)) angle = parsed;
        } else {
            colors.append(borderColor(resolve(values, part, fallback)));
        }
    }
    if (colors.isEmpty()) return borderColor(fallback);
    if (colors.size() == 1) return colors.front();
    // QSS gradients use normalized widget coordinates, unlike QML's pixel vector.
    const double radians = angle * (std::acos(-1.0) / 180.0);
    const double x = std::cos(radians) * 0.5;
    const double y = std::sin(radians) * 0.5;
    QString brush = QStringLiteral("qlineargradient(x1:%1, y1:%2, x2:%3, y2:%4")
        .arg(0.5 - x).arg(0.5 - y).arg(0.5 + x).arg(0.5 + y);
    for (qsizetype i = 0; i < colors.size(); ++i)
        brush += QStringLiteral(", stop:%1 %2").arg(double(i) / (colors.size() - 1)).arg(colors[i]);
    return brush + ')';
}

QString borderStyle(const Values& values, const QString& prefix, const QColor& fallback,
    double alpha, int fallbackWidth)
{
    const Widths widths = borderWidths(values, prefix, fallbackWidth);
    return QStringLiteral("border-style: solid; border-width: %1px %2px %3px %4px; border-color: %5;")
        .arg(widths[0]).arg(widths[1]).arg(widths[2]).arg(widths[3])
        .arg(borderBrush(values, values.value(prefix, QString()), fallback, alpha));
}
}

Theme::Theme(QObject* parent) : QObject(parent)
{
    // Omarchy itself uses this state location (not the legacy config theme link).
    m_themePath = QDir::homePath() + QStringLiteral("/.local/state/omarchy/current/theme");
    m_configPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
        + QStringLiteral("/omarchy/shell.toml");
    m_fontPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
        + QStringLiteral("/fontconfig/fonts.conf");
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(100);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString& path) {
        if (path == m_fontPath) m_fontDirty = true;
        m_debounce.start();
    });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString& path) {
        if (path == QFileInfo(m_fontPath).absolutePath()
            || path == QStandardPaths::writableLocation(QStandardPaths::ConfigLocation))
            m_fontDirty = true;
        m_debounce.start();
    });
    connect(&m_debounce, &QTimer::timeout, this, &Theme::reload);
    reload();
}

void Theme::watchPaths()
{
    // Re-arm after atomic file replacements and theme symlink swaps. Watching
    // both the link parent and its current target handles either update style.
    const QStringList oldFiles = m_watcher.files();
    const QStringList oldDirectories = m_watcher.directories();
    if (!oldFiles.isEmpty()) m_watcher.removePaths(oldFiles);
    if (!oldDirectories.isEmpty()) m_watcher.removePaths(oldDirectories);
    QStringList candidates {
        m_themePath, QFileInfo(m_themePath).canonicalFilePath(),
        QFileInfo(m_themePath).absolutePath(),
        QFileInfo(QFileInfo(m_themePath).absolutePath()).absolutePath(),
        m_themePath + "/colors.toml", m_themePath + "/shell.toml",
        m_configPath, QFileInfo(m_configPath).absolutePath(),
        m_fontPath, QFileInfo(m_fontPath).absolutePath(),
        QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
    };
    candidates.removeDuplicates();
    for (const QString& path : candidates)
        if (!path.isEmpty() && QFileInfo::exists(path)) m_watcher.addPath(path);
}

int Theme::spacing(const QString& token, int fallback) const
{
    const double pinned = number(m_values, "spacing." + token, -1);
    if (pinned >= 0) return qRound(pinned);
    const double scaled = fallback > 0 ? fallback * m_spacingScale : 0;
    return scaled > 0 ? std::max(1, qRound(scaled)) : 0;
}

int Theme::fontSize(const QString& token) const
{
    const int pinned = int(number(m_values, "font." + token, 0));
    if (pinned > 0) return pinned;
    if (token == "icon-small") return fontSize("body-small");
    if (token == "icon") return fontSize("title");
    double ratio = 1.0;
    if (token == "caption") ratio = 0.833;
    else if (token == "body-small") ratio = 0.917;
    else if (token == "subtitle") ratio = 1.083;
    else if (token == "title") ratio = 1.167;
    else if (token == "heading") ratio = 1.333;
    else if (token == "display") ratio = 2.0;
    else if (token == "display-large") ratio = 2.333;
    else if (token == "icon-large") ratio = 1.5;
    return std::max(1, qRound(m_baseSize * ratio));
}

void Theme::reload()
{
    Values values = readTokens(m_themePath + "/colors.toml");
    const auto merge = [&values](const Values& extra) {
        // [style] is the shell's legacy spelling; normalize within each layer
        // so user overrides still win regardless of which spelling they use.
        for (auto it = extra.cbegin(); it != extra.cend(); ++it)
            if (it.key().startsWith("style."))
                values.insert("controls." + it.key().mid(6), it.value());
        for (auto it = extra.cbegin(); it != extra.cend(); ++it)
            if (!it.key().startsWith("style.")) values.insert(it.key(), it.value());
    };
    merge(readTokens(m_themePath + "/shell.toml"));
    merge(readTokens(m_configPath));

    const QColor background = resolve(values, "background", resolve(values, "color0", QColor("#101315")));
    const QColor foreground = resolve(values, "foreground", resolve(values, "color7", QColor("#cacccc")));
    const QColor accent = resolve(values, "accent", resolve(values, "color4", foreground));
    const QColor urgent = resolve(values, "red", resolve(values, "color1", QColor("#a55555")));
    values.insert("background", background.name());
    values.insert("foreground", foreground.name());
    values.insert("accent", accent.name());
    values.insert("urgent", urgent.name());
    const QColor surface = withAlpha(resolve(values, "menu.background", background),
        number(values, "menu.background-alpha", 1));
    const QColor text = resolve(values, "menu.text", foreground);
    const QColor muted = resolve(values, "muted", resolve(values, "color8", foreground));
    m_baseSize = std::max(1, int(number(values, "font.base-size", 12)));
    const double scale = number(values, "spacing.scale", 1);
    m_spacingScale = (scale >= 0 ? scale : 1)
        * (boolean(values, "spacing.scale-with-font", true) ? m_baseSize / 12.0 : 1);
    m_values = values;

    const auto stateColor = [&](const QString& state, const QColor& fallback) {
        return resolve(values, "controls." + state + "-color", fallback);
    };
    const QColor normalColor = stateColor("normal", text);
    const QColor hoverColor = stateColor("hover-cursor", text);
    const QColor selectedColor = stateColor("selected", text);
    const QString focusToken = values.value("controls.focus-color").trimmed().toLower();
    const QColor focusColor = (focusToken == "hover" || focusToken == "hover-cursor" || focusToken == "inherit")
        ? hoverColor : stateColor("focus", hoverColor);
    const QColor pressedColor = stateColor("pressed", hoverColor);
    const QColor selectionColor = stateColor("selection", text);
    const auto alpha = [&](const QString& state, const QString& part, double fallback) {
        return number(values, "controls." + state + '-' + part + "-alpha", fallback);
    };
    const auto fill = [&](const QString& state, const QColor& color, double fallback) {
        return css(withAlpha(color, alpha(state, "fill", fallback)));
    };
    const int normalWidth = std::max(0, qRound(number(values, "controls.normal-border-width", 1)));
    const int hoverWidth = std::max(0, qRound(number(values, "controls.hover-cursor-border-width", normalWidth)));
    const int focusWidth = std::max(0, qRound(number(values, "controls.focus-border-width", hoverWidth)));
    const int selectedWidth = std::max(0, qRound(number(values, "controls.selected-border-width", 0)));
    const auto border = [&](const QString& state, const QColor& color, double opacity, int width) {
        return borderStyle(values, "controls." + state + "-border", color,
            alpha(state, "border", opacity), width);
    };
    const QString normalBorder = border("normal", normalColor, 0.4, normalWidth);
    const QString hoverBorder = border("hover-cursor", hoverColor, 0.25, hoverWidth);
    const QString focusBorder = border("focus", focusColor, alpha("hover-cursor", "border", 0.25), focusWidth);
    const QString selectedBorder = border("selected", selectedColor, 1, selectedWidth);
    const QString normalFill = fill("normal", normalColor, 0.04);
    const QString hoverFill = fill("hover-cursor", hoverColor, 0.08);
    const QString focusFill = fill("focus", focusColor, alpha("hover-cursor", "fill", 0.08));
    const QString selectedFill = fill("selected", selectedColor, 0.18);
    const QColor selection = withAlpha(selectionColor, alpha("selection", "fill", 0.35));

    // The fontconfig alias is shared with the shell. Only resolve it on startup
    // or a fontconfig change, not on every theme/layout directory notification.
    if (m_fontDirty) {
        m_fontDirty = false;
        QProcess fontMatch;
        fontMatch.start("fc-match", {"-f", "%{family[0]}", "monospace"});
        m_fontFamily = QStringLiteral("monospace");
        if (fontMatch.waitForFinished(500) && fontMatch.exitStatus() == QProcess::NormalExit
            && fontMatch.exitCode() == 0) {
            const QString resolved = QString::fromUtf8(fontMatch.readAllStandardOutput()).trimmed();
            if (!resolved.isEmpty()) m_fontFamily = resolved;
        } else {
            fontMatch.kill();
            fontMatch.waitForFinished(100);
        }
    }
    QFont font(m_fontFamily);
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(fontSize("body"));
    qApp->setFont(font);
    QPalette palette;
    palette.setColor(QPalette::Window, surface);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, surface);
    palette.setColor(QPalette::AlternateBase, background);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, surface);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::Highlight, selection);
    palette.setColor(QPalette::HighlightedText, text);
    palette.setColor(QPalette::PlaceholderText, muted);
    palette.setColor(QPalette::Disabled, QPalette::Text, muted);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, muted);
    qApp->setPalette(palette);

    // QStyleSheetStyle can retain platform/class fonts despite QApplication's
    // font. Declare family and logical pixel size on actual QWidget instances;
    // role selectors below are more specific, so titles keep their own size.
    QString family = m_fontFamily;
    family.replace('\\', "\\\\").replace('"', "\\\"");
    QString style = QStringLiteral("QWidget { font-family: \"%1\"; font-size: %2px; background: %3; color: %4; }")
        .arg(family).arg(fontSize("body")).arg(css(surface), css(text));
    const QStringList roles {"caption", "body-small", "body", "subtitle", "title", "heading",
        "display", "display-large", "icon-small", "icon", "icon-large"};
    for (const QString& role : roles)
        style += QStringLiteral("QWidget[role=\"%1\"] { font-size: %2px; }").arg(role).arg(fontSize(role));
    style += "QLabel { background: transparent; }";
    style += "QLabel[role=title] { font-weight: bold; }";
    style += "QLabel[role=muted] { color: " + css(muted) + "; }";
    style += "QLabel[role=phase] { color: " + css(accent) + "; font-weight: bold; }";
    style += "QLabel[role=error] { color: " + css(urgent) + "; font-weight: bold; }";
    style += "QFrame#statusPanel { " + borderStyle(values, "menu.border", text,
        number(values, "menu.border-alpha", 1), normalWidth) + " }";
    style += "QPushButton, QLineEdit, QListWidget { border-radius: 0; " + normalBorder
        + " background: " + normalFill + "; }";
    style += "QPushButton { padding: " + QString::number(spacing("control-padding-y", 6)) + "px "
        + QString::number(spacing("control-padding-x", 10)) + "px; }";
    style += "QPushButton:checked { " + selectedBorder + " background: " + selectedFill
        + "; color: " + css(selectedColor) + "; }";
    style += "QPushButton:hover, QLineEdit:hover, QListWidget:hover { " + hoverBorder
        + " background: " + hoverFill + "; }";
    // Include combined selectors: specificity, not just source order, must keep
    // actual keyboard focus ahead of hover and a persistent checked state.
    style += "QPushButton:focus, QPushButton:focus:hover, QPushButton:focus:checked, "
        "QPushButton:focus:hover:checked, QLineEdit:focus, QLineEdit:focus:hover, "
        "QListWidget:focus, QListWidget:focus:hover { " + focusBorder
        + " background: " + focusFill + "; }";
    style += "QPushButton:pressed, QPushButton:pressed:hover, QPushButton:pressed:checked, "
        "QPushButton:pressed:focus, QPushButton:pressed:focus:hover, "
        "QPushButton:pressed:focus:checked, QPushButton:pressed:hover:checked, "
        "QPushButton:pressed:focus:hover:checked { background: "
        + fill("pressed", pressedColor, 0.22) + "; }";
    style += "QPushButton[primary=true] { color: " + css(accent) + "; }";
    style += "QPushButton:disabled { color: " + css(muted) + "; }";
    style += "QLineEdit { padding: " + QString::number(spacing("input-padding-y", 7)) + "px "
        + QString::number(spacing("control-padding-x", 10)) + "px; selection-background-color: "
        + css(selection) + "; selection-color: " + css(text) + "; }";
    style += "QListWidget { padding: " + QString::number(spacing("xs", 3)) + "px; }";
    style += "QListWidget::item { padding: " + QString::number(spacing("row-gap", 8)) + "px; "
        "border: none; background: transparent; }";
    style += "QListWidget::item:selected { " + selectedBorder + " background: " + selectedFill
        + "; color: " + css(selectedColor) + "; }";
    style += "QListWidget::item:hover, QListWidget::item:selected:hover { " + hoverBorder
        + " background: " + hoverFill + "; }";
    style += "QListWidget::item:focus, QListWidget::item:focus:hover, "
        "QListWidget::item:selected:focus, QListWidget::item:selected:focus:hover { "
        + focusBorder + " background: " + focusFill + "; }";
    style += "QScrollArea { border: none; }";
    style += "QToolTip { " + borderStyle(values, "tooltip.border", text,
        number(values, "tooltip.border-alpha", 1), std::max(1, normalWidth))
        + " background: " + css(withAlpha(resolve(values, "tooltip.background", background),
            number(values, "tooltip.background-alpha", 1)))
        + "; color: " + css(resolve(values, "tooltip.text", foreground))
        + "; padding: " + QString::number(spacing("control-padding-y", 6)) + "px "
        + QString::number(spacing("control-padding-x", 10)) + "px; font-size: "
        + QString::number(fontSize("body-small")) + "px; }";
    qApp->setStyleSheet(style);
    watchPaths();
    emit changed();
}
