#include "windowpresentation.h"
#include "theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QBuffer>
#include <QBoxLayout>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFile>
#include <QFrame>
#include <QFontMetrics>
#include <QIcon>
#include <QImageReader>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QScrollArea>
#include <QResizeEvent>
#include <QScreen>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

namespace WindowPresentation {
// Adapt only the supplied SVG's wordmark fill; the blue pixel artwork stays
// untouched. QImageReader uses Qt's runtime SVG plugin without a QtSvg link.
class BrandLogo final : public QLabel {
public:
    explicit BrandLogo(QWidget* parent, qreal scale = 1.0) : QLabel(parent), m_scale(scale)
    {
        QSizePolicy policy(QSizePolicy::Maximum, QSizePolicy::Maximum);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
        setAccessibleName(QStringLiteral("OMAWIN365"));
        setAlignment(Qt::AlignLeft | Qt::AlignTop);
        QFile source(QStringLiteral(":/icons/header.svg"));
        if (source.open(QIODevice::ReadOnly)) m_svg = source.readAll();
        // Browser media queries are not supported consistently by Qt SVG.
        const qsizetype start = m_svg.indexOf("<style>");
        const qsizetype end = m_svg.indexOf("</style>", start);
        if (start >= 0 && end >= start) m_svg.remove(start, end + 8 - start);
    }
    int heightForWidth(int width) const override
    {
        const int logoWidth = qMax(1, qMin(width, preferredWidth()));
        return qMax(1, qRound(logoWidth * 217.0 / 1007.0));
    }
    QSize sizeHint() const override
    {
        const int width = preferredWidth();
        return QSize(width, heightForWidth(width));
    }
    QSize minimumSizeHint() const override { return QSize(0, 0); }

    int preferredWidth() const
    {
        return qRound(qMin(240, QFontMetrics(font()).lineSpacing() * 12) * m_scale);
    }
    void refresh()
    {
        const QColor color = palette().color(QPalette::WindowText);
        const qreal dpr = devicePixelRatioF();
        if (m_renderSize == size() && m_renderDpr == dpr && m_textColor == color)
            return;
        if (m_textColor != color) {
            m_coloredSvg = m_svg;
            m_coloredSvg.replace("class=\"t\"",
                QByteArray("fill=\"") + color.name().toUtf8() + "\"");
            m_textColor = color;
        }
        QBuffer buffer(&m_coloredSvg);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "svg");
        const QSize pixels(qMax(1, qRound(width() * dpr)), qMax(1, qRound(height() * dpr)));
        reader.setScaledSize(QSize(1007, 217).scaled(pixels, Qt::KeepAspectRatio));
        QPixmap image = QPixmap::fromImage(reader.read());
        image.setDevicePixelRatio(dpr);
        m_renderSize = size();
        m_renderDpr = dpr;
        setPixmap(image);
    }
protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QLabel::resizeEvent(event);
        refresh();
    }
    bool event(QEvent* event) override
    {
        const bool result = QLabel::event(event);
        if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange
            || event->type() == QEvent::Show) refresh();
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        if (event->type() == QEvent::DevicePixelRatioChange) refresh();
#endif
        return result;
    }
private:
    const qreal m_scale;
    QByteArray m_svg;
    QByteArray m_coloredSvg;
    QColor m_textColor;
    QSize m_renderSize;
    qreal m_renderDpr = 0;
};

BrandingHeader::BrandingHeader(Theme* theme, QWidget* parent, qreal logoScale) : QWidget(parent), m_theme(theme)
{
    QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSizeConstraint(QLayout::SetNoConstraint);
    m_logo = new BrandLogo(this, logoScale);
    m_layout->addWidget(m_logo, 0, Qt::AlignLeft);
    m_layout->addStretch(1);
    connect(theme, &Theme::changed, this, [this] { updateLayout(); });
    updateLayout();
}

void BrandingHeader::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
        updateLayout();
}

void BrandingHeader::updateLayout()
{
    if (!m_logo) return;
    m_logo->setMaximumWidth(m_logo->preferredWidth());
    m_logo->updateGeometry();
    m_logo->refresh();
    m_layout->setSpacing(m_theme->spacing(QStringLiteral("control-gap"), 8));
    updateGeometry();
}

WrappedButton::WrappedButton(const QString& text, QWidget* parent) : QPushButton(text, parent)
{
    QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
}

bool WrappedButton::hasHeightForWidth() const { return true; }

int WrappedButton::heightForWidth(int width) const
{
    QStyleOptionButton option;
    initStyleOption(&option);
    const QSize inset = style()->sizeFromContents(QStyle::CT_PushButton, &option, QSize(0, 0), this);
    const QRect textRect = fontMetrics().boundingRect(
        QRect(0, 0, qMax(1, width - inset.width()), QWIDGETSIZE_MAX),
        Qt::TextWordWrap | Qt::TextShowMnemonic, text());
    return qMax(QPushButton::sizeHint().height(), textRect.height() + inset.height());
}

QSize WrappedButton::minimumSizeHint() const { return QSize(0, QPushButton::sizeHint().height()); }

void WrappedButton::paintEvent(QPaintEvent*)
{
    QStylePainter painter(this);
    QStyleOptionButton option;
    initStyleOption(&option);
    option.text.clear();
    painter.drawControl(QStyle::CE_PushButton, option);
    QRect textRect = style()->subElementRect(QStyle::SE_PushButtonContents, &option, this);
    if (isDown()) textRect.translate(style()->pixelMetric(QStyle::PM_ButtonShiftHorizontal, &option, this),
        style()->pixelMetric(QStyle::PM_ButtonShiftVertical, &option, this));
    const int mnemonic = style()->styleHint(QStyle::SH_UnderlineShortcut, &option, this)
        ? Qt::TextShowMnemonic : Qt::TextHideMnemonic;
    style()->drawItemText(&painter, textRect, Qt::AlignCenter | Qt::TextWordWrap | mnemonic,
        option.palette, isEnabled(), text(), QPalette::ButtonText);
}

AdaptiveActions::AdaptiveActions(QWidget* parent) : QWidget(parent)
{
    QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    actions = new QBoxLayout(QBoxLayout::LeftToRight, this);
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSizeConstraint(QLayout::SetNoConstraint);
}

void AdaptiveActions::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateDirection();
}

bool AdaptiveActions::event(QEvent* event)
{
    const bool result = QWidget::event(event);
    if (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Show
        || event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
        updateDirection();
    return result;
}

void AdaptiveActions::updateDirection()
{
    if (!actions) return;
    int preferred = 0;
    int count = 0;
    for (int index = 0; index < actions->count(); ++index) {
        QLayoutItem* item = actions->itemAt(index);
        if (item->isEmpty()) continue;
        if (count++) preferred += actions->spacing();
        preferred += item->widget()->sizeHint().width();
    }
    actions->setDirection(preferred > width() ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
}

WrappedContent::WrappedContent(QWidget* parent) : QWidget(parent) {}

void WrappedContent::updateWrappedHeight()
{
    if (!layout()) return;
    const int height = layout()->totalHeightForWidth(width());
    if (height >= 0 && minimumHeight() != height) setMinimumHeight(height);
}

void WrappedContent::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateWrappedHeight();
}

bool WrappedContent::event(QEvent* event)
{
    const bool result = QWidget::event(event);
    if (event->type() == QEvent::LayoutRequest) updateWrappedHeight();
    return result;
}

DialogLayout::DialogLayout(QDialog* dialog, Theme* theme)
    : QObject(dialog), m_dialog(dialog), m_theme(theme)
{
    setObjectName(QStringLiteral("adaptiveDialogLayout"));
    m_outer = new QVBoxLayout(dialog);
    m_outer->setSizeConstraint(QLayout::SetNoConstraint);
    m_scroll = new QScrollArea(dialog);
    // fit() sizes this viewport from its wrapped content, not scroll chrome.
    m_scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_content = new WrappedContent(m_scroll);
    details = new QVBoxLayout(m_content);
    details->setContentsMargins(0, 0, 0, 0);
    m_scroll->setWidget(m_content);
    m_outer->addWidget(m_scroll, 1);
    m_controlsScroll = new QScrollArea(dialog);
    m_controlsScroll->setWidgetResizable(true);
    m_controlsScroll->setFrameShape(QFrame::NoFrame);
    m_controlsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_controlContent = new WrappedContent(m_controlsScroll);
    controls = new QVBoxLayout(m_controlContent);
    controls->setContentsMargins(0, 0, 0, 0);
    m_controlsScroll->setWidget(m_controlContent);
    m_outer->addWidget(m_controlsScroll);
    dialog->installEventFilter(this);
    connect(theme, &Theme::changed, this, [this] { applyTheme(); scheduleFit(); });
    applyTheme();
}

QWidget* DialogLayout::content() const { return m_content; }

void DialogLayout::fit(bool preserveWidth)
{
    applyTheme();
    // A top-aligned layout can retain its old size-hint height after a
    // width change. Let it fill the content and put surplus below the text.
    if (details->count() && !details->itemAt(details->count() - 1)->spacerItem())
        details->addStretch();
    for (QLabel* text : m_content->findChildren<QLabel*>()) {
        if (text->property("messageText").toBool()) {
            text->setWordWrap(true);
            QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Minimum);
            policy.setHeightForWidth(true);
            text->setSizePolicy(policy);
            text->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        }
    }
    const QFontMetrics metrics(m_dialog->font());
    const int padding = m_theme->spacing(QStringLiteral("panel-padding"), 18);
    const int gap = m_theme->spacing(QStringLiteral("control-gap"), 8);
    const QSize available = m_dialog->screen()->availableGeometry().size()
        - QSize(2 * m_theme->spacing(QStringLiteral("screen-margin"), 16),
                2 * m_theme->spacing(QStringLiteral("screen-margin"), 16));
    int textWidth = 0;
    for (QLabel* text : m_content->findChildren<QLabel*>())
        textWidth = qMax(textWidth, text->fontMetrics().boundingRect(text->text()).width());
    const int width = qMin(qMax(1, available.width()),
        preserveWidth ? m_dialog->width()
            : qMax(controls->sizeHint().width(), qMin(textWidth, metrics.averageCharWidth() * 52)) + 2 * padding);
    const int innerWidth = qMax(1, width - 2 * padding);
    const int scrollBar = m_scroll->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    const int detailHeight = qMax(0, details->totalHeightForWidth(qMax(1, innerWidth - scrollBar)));
    const int controlsHeight = qMax(controls->sizeHint().height(), controls->totalHeightForWidth(innerWidth));
    const int height = qMin(qMax(1, available.height()), detailHeight + controlsHeight + gap + 2 * padding);
    m_dialog->setMinimumSize(0, 0);
    sizeControls(width, height);
    m_dialog->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    m_dialog->resize(width, height);
    m_outer->activate();
    m_content->updateWrappedHeight();
}

bool DialogLayout::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_dialog && event->type() == QEvent::Resize) {
        const auto* resize = static_cast<QResizeEvent*>(event);
        sizeControls(resize->size().width(), resize->size().height());
    }
    if (watched == m_dialog && event->type() == QEvent::Show) scheduleFit();
    // QMessageBox otherwise replaces content-driven sizing with setFixedSize.
    if (watched == m_dialog && event->type() == QEvent::LayoutRequest
        && qobject_cast<QMessageBox*>(m_dialog)) {
        m_outer->activate();
        m_content->updateWrappedHeight();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

void DialogLayout::sizeControls(int width, int height)
{
    const QMargins margins = m_outer->contentsMargins();
    const int innerWidth = qMax(1, width - margins.left() - margins.right());
    const int available = qMax(0, height - margins.top() - margins.bottom() - m_outer->spacing()
        - QFontMetrics(m_dialog->font()).lineSpacing());
    int wanted = qMax(controls->sizeHint().height(), controls->totalHeightForWidth(innerWidth));
    if (wanted > available) {
        const int scrollBar = m_controlsScroll->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
        wanted = qMax(wanted, controls->totalHeightForWidth(qMax(1, innerWidth - scrollBar)));
    }
    // Actions stay pinned normally; only an exceptionally short host scrolls them.
    m_controlsScroll->setFixedHeight(qMin(wanted, available));
    m_controlContent->updateWrappedHeight();
}

void DialogLayout::applyTheme()
{
    QFont font(m_theme->fontFamily());
    font.setPixelSize(m_theme->fontSize(QStringLiteral("body")));
    m_dialog->setFont(font);
    const int padding = m_theme->spacing(QStringLiteral("panel-padding"), 18);
    m_outer->setContentsMargins(padding, padding, padding, padding);
    layoutSpacing(m_outer, m_theme->spacing(QStringLiteral("control-gap"), 8));
    details->setSpacing(m_theme->spacing(QStringLiteral("control-gap"), 8));
    layoutSpacing(controls, m_theme->spacing(QStringLiteral("control-gap"), 8));
}

void DialogLayout::scheduleFit()
{
    if (m_fitPending) return;
    m_fitPending = true;
    QTimer::singleShot(0, this, [this] { m_fitPending = false; fit(); });
}

QLabel* label(const QString& text, QWidget* parent, const char* role)
{
    auto* result = new QLabel(text, parent);
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    result->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    policy.setHeightForWidth(true);
    result->setSizePolicy(policy);
    if (role) result->setProperty("role", role);
    return result;
}

void layoutSpacing(QLayout* layout, int gap)
{
    layout->setSpacing(gap);
    for (int index = 0; index < layout->count(); ++index) {
        QLayoutItem* item = layout->itemAt(index);
        if (item->layout()) layoutSpacing(item->layout(), gap);
        else if (item->widget() && item->widget()->layout())
            layoutSpacing(item->widget()->layout(), gap);
    }
}

void themeMessageBox(QMessageBox* box, Theme* theme)
{
    box->setOption(QMessageBox::Option::DontUseNativeDialog);
    // Omarchy confirmations are text-led, not platform bitmap/emoji alerts.
    box->setIcon(QMessageBox::NoIcon);
    auto* message = box->findChild<QLabel*>(QStringLiteral("qt_msgbox_label"));
    auto* buttons = box->findChild<QDialogButtonBox*>(QStringLiteral("qt_msgbox_buttonbox"));
    // Retain QMessageBox's buttons, result codes, Escape/default handling and ownership.
    delete box->layout();
    auto* layout = new DialogLayout(box, theme);
    layout->details->addWidget(label(box->windowTitle(), layout->content(), "title"));
    message->setProperty("messageText", true);
    layout->details->addWidget(message);
    buttons->setOrientation(Qt::Vertical);
    const auto clearButtonIcons = [buttons] {
        for (QAbstractButton* button : buttons->buttons()) button->setIcon(QIcon());
    };
    clearButtonIcons();
    QObject::connect(theme, &Theme::changed, box, clearButtonIcons);
    layout->controls->addWidget(buttons);
    layout->fit();
}

void refit(QDialog* prompt)
{
    if (auto* layout = prompt->findChild<QObject*>(QStringLiteral("adaptiveDialogLayout")))
        static_cast<DialogLayout*>(layout)->fit(true);
}

bool confirmProfileAction(QMessageBox& confirm, Theme* theme,
                          QMessageBox::StandardButton action, const QString& caption)
{
    confirm.setTextFormat(Qt::PlainText);
    confirm.setDefaultButton(QMessageBox::Cancel);
    confirm.button(action)->setText(caption);
    themeMessageBox(&confirm, theme);
    return confirm.exec() == action;
}

void attention(QWidget* widget)
{
    widget->show();
    widget->raise();
    widget->activateWindow();
    if (widget->windowHandle()) widget->windowHandle()->requestActivate();
    QApplication::alert(widget, 0);
}

QDialog* dialog(QWidget* owner, const QString& title)
{
    auto* result = new QDialog(owner);
    result->setWindowTitle(title);
    result->setWindowModality(Qt::ApplicationModal);
    result->setAttribute(Qt::WA_DeleteOnClose);
    result->setMinimumSize(0, 0);
    return result;
}
}
