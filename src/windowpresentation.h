#pragma once

#include <QMessageBox>
#include <QObject>
#include <QPushButton>
#include <QWidget>

class Theme;
class QBoxLayout;
class QHBoxLayout;
class QLabel;
class QScrollArea;
class QVBoxLayout;

// Private presentation primitives; connection and dialog flow stays in Window.
namespace WindowPresentation {
class BrandLogo;

class BrandingHeader final : public QWidget {
public:
    BrandingHeader(Theme* theme, QWidget* parent, qreal logoScale = 1.0);
protected:
    void changeEvent(QEvent* event) override;
private:
    void updateLayout();
    Theme* m_theme;
    QHBoxLayout* m_layout = nullptr;
    BrandLogo* m_logo = nullptr;
};

// Keep native button interaction/defaults, but allow captions to wrap in tiles.
class WrappedButton final : public QPushButton {
public:
    WrappedButton(const QString& text, QWidget* parent);
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize minimumSizeHint() const override;
protected:
    void paintEvent(QPaintEvent*) override;
};

// Keep secondary actions compact on roomy panels and readable in narrow tiles.
class AdaptiveActions final : public QWidget {
public:
    explicit AdaptiveActions(QWidget* parent);
    QBoxLayout* actions = nullptr;
protected:
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;
private:
    void updateDirection();
};

// QScrollArea must see the wrapped height, not just each label's one-line minimum.
class WrappedContent final : public QWidget {
public:
    explicit WrappedContent(QWidget* parent);
    void updateWrappedHeight();
protected:
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;
};

class DialogLayout final : public QObject {
public:
    DialogLayout(QDialog* dialog, Theme* theme);
    QVBoxLayout* details;
    QVBoxLayout* controls;
    QWidget* content() const;
    void fit(bool preserveWidth = false);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    void sizeControls(int width, int height);
    void applyTheme();
    void scheduleFit();
    QDialog* m_dialog;
    Theme* m_theme;
    QVBoxLayout* m_outer;
    QScrollArea* m_scroll;
    WrappedContent* m_content;
    QScrollArea* m_controlsScroll;
    WrappedContent* m_controlContent;
    bool m_fitPending = false;
};

QLabel* label(const QString& text, QWidget* parent, const char* role = nullptr);
void layoutSpacing(QLayout* layout, int gap);
void themeMessageBox(QMessageBox* box, Theme* theme);
void refit(QDialog* prompt);
// Caller retains the stack box through the subsequent profile operation.
bool confirmProfileAction(QMessageBox& confirm, Theme* theme,
                          QMessageBox::StandardButton action, const QString& caption);
void attention(QWidget* widget);
QDialog* dialog(QWidget* owner, const QString& title);
}
