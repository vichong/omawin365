#include "window.h"
#include "windowpresentation.h"
#include "browserauth.h"
#include "profilestore.h"
#include "session.h"
#include "theme.h"

#include <QApplication>
#include <QBoxLayout>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QFontMetrics>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QStyle>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QValidator>

using namespace WindowPresentation;

namespace {
class PinValidator final : public QValidator {
public:
    explicit PinValidator(QObject* parent) : QValidator(parent) {}
    State validate(QString& input, int&) const override
    {
        for (QChar ch : input)
            if (ch.category() == QChar::Other_Control || ch.category() == QChar::Separator_Line
                || ch.category() == QChar::Separator_Paragraph) return Invalid;
        if (input.toUtf8().size() > 63) return Invalid;
        return input.size() >= 4 ? Acceptable : Intermediate;
    }
};

class ProfileNameValidator final : public QValidator {
public:
    explicit ProfileNameValidator(QObject* parent) : QValidator(parent) {}
    State validate(QString& input, int&) const override
    {
        if (input.size() > 160) return Invalid;
        for (QChar ch : input)
            if (ch.category() == QChar::Other_Control || ch.category() == QChar::Separator_Line
                || ch.category() == QChar::Separator_Paragraph) return Invalid;
        return input.trimmed().isEmpty() ? Intermediate : Acceptable;
    }
};

QString phaseTitle(const QString& phase)
{
    if (phase == "connecting") return QStringLiteral("Connecting");
    if (phase == "signing-in") return QStringLiteral("Microsoft sign-in");
    if (phase == "awaiting-pin") return QStringLiteral("Security key PIN required");
    if (phase == "awaiting-touch") return QStringLiteral("Touch your security key");
    if (phase == "connected") return QStringLiteral("Connected");
    if (phase == "disconnecting") return QStringLiteral("Disconnecting");
    if (phase == "acquiring") return QStringLiteral("Getting Cloud PC connection");
    if (phase == "acquisition-error") return QStringLiteral("Connection download stopped");
    if (phase == "error") return QStringLiteral("Connection stopped");
    return QStringLiteral("Ready");
}
}

Window::Window(Session* session, BrowserAuth* browser, ProfileStore* profiles, QWidget* parent)
    : QWidget(parent), m_session(session), m_browser(browser), m_profiles(profiles), m_theme(new Theme(this))
{
    setWindowTitle(QStringLiteral("OMAWIN365"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/omawin365.svg")));
    setMinimumSize(0, 0);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("connectionScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumSize(0, 0);
    scroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setAlignment(Qt::AlignCenter);
    auto* content = new WrappedContent(scroll);
    content->setObjectName(QStringLiteral("connectionPanel"));
    content->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    m_connectionPanel = content;
    m_contentLayout = new QVBoxLayout(content);
    m_contentLayout->setSizeConstraint(QLayout::SetNoConstraint);
    m_contentLayout->addWidget(new BrandingHeader(m_theme, content));

    auto* profilesLabel = label(QStringLiteral("Cloud &PCs"), content, "title");
    m_contentLayout->addWidget(profilesLabel);
    m_list = new QListWidget(content);
    profilesLabel->setBuddy(m_list);
    m_list->setAccessibleName(QStringLiteral("Cloud PC profiles"));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setTextElideMode(Qt::ElideRight);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_contentLayout->addWidget(m_list);
    m_empty = label(QStringLiteral("Use your work account to get a Microsoft Cloud PC connection, or import a trusted .rdpw file supplied by your organization."), content, "muted");
    m_contentLayout->addWidget(m_empty);

    auto* profileActions = new AdaptiveActions(content);
    m_import = new WrappedButton(QStringLiteral("&Import .rdpw…"), content);
    m_import->setToolTip(QStringLiteral("Import a trusted RDP file (Ctrl+O)"));
    m_remove = new WrappedButton(QStringLiteral("&Remove…"), content);
    m_rename = new WrappedButton(QStringLiteral("Re&name…"), content);
    m_rename->setToolTip(QStringLiteral("Change this app's local Cloud PC label (F2)"));
    profileActions->actions->addWidget(m_import);
    profileActions->actions->addWidget(m_rename);
    profileActions->actions->addWidget(m_remove);

    auto* statusPanel = new QFrame(content);
    statusPanel->setObjectName(QStringLiteral("statusPanel"));
    m_statusLayout = new QVBoxLayout(statusPanel);
    m_phaseLabel = label(QStringLiteral("Ready"), statusPanel, "phase");
    m_phaseLabel->setAccessibleName(QStringLiteral("Connection status"));
    m_sessionLabel = label({}, statusPanel, "muted");
    m_sessionLabel->hide();
    m_detailLabel = label(QStringLiteral("Select a Cloud PC to connect."), statusPanel);
    m_progressNote = label({}, statusPanel, "muted");
    m_progressNote->hide();
    m_statusLayout->addWidget(m_phaseLabel);
    m_statusLayout->addWidget(m_sessionLabel);
    m_statusLayout->addWidget(m_detailLabel);
    m_statusLayout->addWidget(m_progressNote);

    m_connect = new WrappedButton(QStringLiteral("&Connect"), content);
    m_connect->setProperty("primary", true);
    m_connect->setToolTip(QStringLiteral("Connect to the selected Cloud PC (Ctrl+Return)"));
    m_contentLayout->addWidget(m_connect);
    auto* sessionActions = new AdaptiveActions(content);
    m_disconnect = new WrappedButton(QStringLiteral("&Disconnect"), content);
    m_reconnect = new WrappedButton(QStringLiteral("R&econnect"), content);
    sessionActions->actions->addWidget(m_disconnect);
    sessionActions->actions->addWidget(m_reconnect);
    m_contentLayout->addWidget(sessionActions);
    m_contentLayout->addWidget(statusPanel);
    m_contentLayout->addWidget(profileActions);
    m_acquire = new WrappedButton(QStringLiteral("&Get a Cloud PC connection"), content);
    m_contentLayout->addWidget(m_acquire);
    connect(m_acquire, &QPushButton::clicked, this, &Window::acquireProfile);
    m_contentLayout->addWidget(label(QStringLiteral("Your desktop opens in a separate FreeRDP window. Private Microsoft sign-in may appear several times; follow each prompt."), content, "muted"));
    m_contentLayout->addWidget(label(QStringLiteral("Tested with Windows 365 Enterprise. Other editions are not yet verified."), content, "muted"));
    auto* about = new QPushButton(QStringLiteral("&About…"), content);
    about->setObjectName(QStringLiteral("aboutButton"));
    m_contentLayout->addWidget(about, 0, Qt::AlignRight);
    connect(about, &QPushButton::clicked, this, &Window::showAbout);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);

    connect(m_theme, &Theme::changed, this, &Window::applySpacing);
    applySpacing();
    connect(m_profiles, &ProfileStore::changed, this, [this] { refreshProfiles(); });
    connect(m_list, &QListWidget::currentRowChanged, this, [this] {
        QSettings().setValue(QStringLiteral("selectedProfile"), selectedId());
        updateControls();
    });
    connect(m_list, &QListWidget::itemActivated, this, [this] { connectSelected(); });
    connect(m_import, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import Cloud PC connection"), {},
            QStringLiteral("RDP connection files (*.rdp *.rdpw);;All files (*)"));
        if (!path.isEmpty()) importProfile(path);
    });
    connect(m_remove, &QPushButton::clicked, this, &Window::removeSelected);
    connect(m_rename, &QPushButton::clicked, this, &Window::renameSelected);
    connect(m_connect, &QPushButton::clicked, this, &Window::connectSelected);
    connect(m_disconnect, &QPushButton::clicked, this, &Window::disconnectSession);
    connect(m_reconnect, &QPushButton::clicked, this, &Window::reconnectSession);
    auto* importShortcut = new QShortcut(QKeySequence::Open, this);
    connect(importShortcut, &QShortcut::activated, m_import, &QPushButton::click);
    auto* renameShortcut = new QShortcut(QKeySequence(Qt::Key_F2), this);
    connect(renameShortcut, &QShortcut::activated, m_rename, &QPushButton::click);
    auto* connectShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Return")), this);
    connect(connectShortcut, &QShortcut::activated, this, &Window::connectSelected);
    auto* closeShortcut = new QShortcut(QKeySequence::Close, this);
    connect(closeShortcut, &QShortcut::activated, this, &QWidget::close);

    connect(m_session, &Session::statusChanged, this, [this](const QString& phase, const QString& detail) {
        if (m_errorVisible && (phase == "disconnecting" || phase == "disconnected")) {
            updateControls();
            return;
        }
        if (phase == "connected" && m_challenge.dialog
            && (m_phase == "awaiting-pin" || m_phase == "awaiting-touch")) {
            updateControls();
            return;
        }
        setStatus(phase, detail);
        if (phase == "awaiting-touch") showTouch();
    });
    connect(m_session, &Session::connected, this, [this] {
        m_connected = true;
        if (m_phase != "awaiting-pin" && m_phase != "awaiting-touch")
            setStatus("connected", QStringLiteral("Your desktop is connected in its own FreeRDP window."));
        updateControls();
    });
    connect(m_session, &Session::pinRequested, this, &Window::requestPin);
    connect(m_session, &Session::authRequested, this, [this](const OAuthContract::Request& url) {
        if (m_session->active() && !m_stopping) m_browser->begin(url);
    });
    connect(m_browser, &BrowserAuth::callbackReady, this, [this](const OAuthContract::Callback& url) {
        if (m_session->active() && !m_stopping) m_session->submitAuthResult(url);
    });
    connect(m_browser, &BrowserAuth::status, this, [this](const QString& message) {
        if (m_acquisition.active) {
            setStatus("acquiring", message);
            if (m_acquisition.status) m_acquisition.status->setText(message);
        }
        else if (m_session->active() && !m_stopping && m_phase == "signing-in")
            setStatus("signing-in", message);
    });
    connect(m_browser, &BrowserAuth::failed, this, [this](const QString& message) {
        if (m_acquisition.active) {
            m_acquisition.active = false;
            if (m_acquisition.guide) m_acquisition.guide->reject();
            setStatus("acquisition-error", message);
            showError(QStringLiteral("Connection download stopped"), message);
            updateControls();
            return;
        }
        if (!m_session->active() || m_stopping) return;
        m_restartPath.clear();
        m_errorVisible = true;
        disconnectSession();
        setStatus("error", message);
        showError(QStringLiteral("Sign-in stopped"), message);
    });
    connect(m_session, &Session::error, this, [this](const QString& message) {
        m_restartPath.clear();
        m_stopping = true;
        m_errorVisible = true;
        if (m_renameDialog) m_renameDialog->reject();
        dismissChallenge();
        m_browser->cancel();
        setStatus("error", message);
        showError(QStringLiteral("Connection stopped"), message);
    });
    connect(m_session, &Session::ended, this, [this] {
        m_browser->cancel();
        dismissChallenge();
        m_connected = false;
        m_stopping = false;
        m_activeProfile.clear();
        m_sessionLabel->hide();
        if (m_closePending) {
            m_restartPath.clear();
            QTimer::singleShot(0, this, &QWidget::close);
            return;
        }
        if (!m_errorVisible) setStatus("disconnected", QStringLiteral("Session closed. You can connect again."));
        updateControls();
        if (!m_restartPath.isEmpty()) {
            const QString path = m_restartPath;
            m_restartPath.clear();
            m_errorVisible = false;
            m_activeProfile = path;
            for (const Profile& profile : m_profiles->profiles()) {
                if (profile.path == path) {
                    m_sessionLabel->setText(profile.name);
                    m_sessionLabel->show();
                    break;
                }
            }
            setStatus("connecting", QStringLiteral("Starting a new session…"));
            m_session->start(path);
            updateControls();
        }
    });
    connect(m_browser, &BrowserAuth::profileDownloaded, this, [this](const QString& temporaryPath, const QString& displayName) {
        if (!m_acquisition.active || m_session->active()) return;
        // Import synchronously while BrowserAuth still owns its private download.
        // The direct Get action authorized private import, never connection.
        QString error;
        const Profile profile = m_profiles->importFile(temporaryPath, &error, displayName);
        m_acquisition.active = false;
        if (m_acquisition.guide) m_acquisition.guide->accept();
        if (profile.id.isEmpty()) {
            setStatus("acquisition-error", error);
            showError(QStringLiteral("Import failed"), error);
        } else {
            refreshProfiles(profile.id);
            setStatus("disconnected", QStringLiteral("Cloud PC connection imported. Select Connect when ready."));
            m_list->setFocus();
        }
        updateControls();
    });
    connect(m_browser, &BrowserAuth::resourcesAvailable, this,
        [this](const QStringList& ids, const QStringList& names) {
            if (!m_acquisition.active || !m_acquisition.guide || !m_acquisition.resources
                || ids.size() != names.size()) return;
            auto* resources = m_acquisition.resources.data();
            resources->clear();
            resources->setEnabled(true);
            for (qsizetype index = 0; index < ids.size(); ++index) {
                auto* item = new QListWidgetItem(names.at(index), resources);
                item->setData(Qt::UserRole, ids.at(index));
                item->setToolTip(QStringLiteral("<qt>") + names.at(index).toHtmlEscaped() + QStringLiteral("</qt>"));
            }
            resources->setVisible(!ids.isEmpty());
            m_acquisition.download->setVisible(!ids.isEmpty());
            m_acquisition.download->setDefault(!ids.isEmpty());
            m_acquisition.download->setEnabled(false);
            m_acquisition.status->setText(ids.isEmpty()
                ? QStringLiteral("Waiting for the portal to refresh available Cloud PCs…")
                : QStringLiteral("Choose the Cloud PC connection to download. This will not open a desktop."));
            refit(m_acquisition.guide);
            if (!ids.isEmpty()) resources->setFocus();
        });
    refreshProfiles(QSettings().value(QStringLiteral("selectedProfile")).toString());
    const QFontMetrics metrics(font());
    const int padding = m_theme->spacing(QStringLiteral("panel-padding"), 18);
    const QSize available = screen()->availableGeometry().size()
        - QSize(2 * m_theme->spacing(QStringLiteral("screen-margin"), 16),
                2 * m_theme->spacing(QStringLiteral("screen-margin"), 16));
    const int width = qMin(qMax(1, available.width()), m_connectionPanel->maximumWidth());
    resize(width, qMin(qMax(1, available.height()), metrics.lineSpacing() * 38 + 2 * padding));
}

void Window::showAbout()
{
    if (m_aboutDialog) {
        attention(m_aboutDialog);
        return;
    }
    auto* prompt = dialog(this, QStringLiteral("About OMAWIN365"));
    prompt->setObjectName(QStringLiteral("aboutDialog"));
    m_aboutDialog = prompt;
    auto* layout = new DialogLayout(prompt, m_theme);
    auto* content = layout->content();
    layout->details->addWidget(new BrandingHeader(m_theme, content, 1.25));
    layout->details->addWidget(label(QStringLiteral("Version %1 · pre-release")
        .arg(QCoreApplication::applicationVersion()), content, "title"));
    layout->details->addWidget(label(QStringLiteral("Windows 365 Cloud PCs for Omarchy."), content));
    layout->details->addWidget(label(QStringLiteral("© 2026 Vic Hong · MIT licence\nProvided as-is, without warranty."), content));
    layout->details->addWidget(label(QStringLiteral("Wordmark letterforms: David Heinemeier Hansson (MIT).\nWindows 365 mark: Microsoft trademark.\nUses separately installed Qt, FreeRDP and Chromium."), content, "muted"));
    layout->details->addWidget(label(QStringLiteral("Independent community project. Not affiliated with, endorsed by or supported by Microsoft or the Omarchy project."), content, "muted"));
    layout->details->addWidget(label(QStringLiteral("GitHub links require repository access while this project is private."), content, "muted"));
    auto* linkError = label({}, content, "error");
    linkError->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    linkError->hide();
    layout->details->addWidget(linkError);
    auto* notices = label({}, content, "body-small");
    notices->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    notices->hide();
    layout->details->addWidget(notices);

    auto* links = new AdaptiveActions(prompt);
    const auto addLink = [this, links, prompt, layout, linkError](const QString& caption, const QUrl& url) {
        auto* button = new WrappedButton(caption, links);
        button->setToolTip(url.toString());
        links->actions->addWidget(button);
        connect(button, &QPushButton::clicked, prompt, [layout, linkError, url] {
            const bool opened = QDesktopServices::openUrl(url);
            linkError->setText(opened ? QString() : QStringLiteral("Could not open your browser. Copy this address:\n%1").arg(url.toString()));
            linkError->setVisible(!opened);
            layout->fit(true);
        });
    };
    addLink(QStringLiteral("&GitHub"), QUrl(QStringLiteral("https://github.com/vichong/omawin365")));
    addLink(QStringLiteral("&Report an issue"), QUrl(QStringLiteral("https://github.com/vichong/omawin365/issues")));
    layout->controls->addWidget(links);
    auto* licence = new WrappedButton(QStringLiteral("&Licence and notices"), prompt);
    licence->setCheckable(true);
    connect(licence, &QPushButton::toggled, prompt, [layout, notices](bool shown) {
        if (shown && notices->text().isEmpty()) {
            QString text;
            for (const auto* path : {":/about/LICENSE", ":/about/THIRD_PARTY_NOTICES.md", ":/about/omarchy-MIT.txt"}) {
                QFile file(QString::fromLatin1(path));
                if (!file.open(QIODevice::ReadOnly)) {
                    text = QStringLiteral("Could not read the bundled licence notices.");
                    break;
                }
                if (!text.isEmpty()) text += QStringLiteral("\n\n");
                text += QString::fromUtf8(file.readAll());
            }
            notices->setText(text);
        }
        notices->setVisible(shown);
        layout->fit(true);
    });
    layout->controls->addWidget(licence);
    auto* close = new WrappedButton(QStringLiteral("&Close"), prompt);
    close->setDefault(true);
    layout->controls->addWidget(close);
    connect(close, &QPushButton::clicked, prompt, &QDialog::accept);
    layout->fit();
    attention(prompt);
    close->setFocus();
}

void Window::applySpacing()
{
    QFont themedFont(m_theme->fontFamily());
    themedFont.setPixelSize(m_theme->fontSize(QStringLiteral("body")));
    setFont(themedFont);
    const int padding = m_theme->spacing(QStringLiteral("panel-padding"), 18);
    m_contentLayout->setContentsMargins(padding, padding, padding, padding);
    layoutSpacing(m_contentLayout, m_theme->spacing(QStringLiteral("control-gap"), 8));
    m_statusLayout->setContentsMargins(padding, padding, padding, padding);
    const QFontMetrics metrics(m_list->font());
    m_list->setMinimumHeight(metrics.lineSpacing() * 3);
    m_list->setMaximumHeight(metrics.lineSpacing() * 5);
    m_connectionPanel->setMaximumWidth(qBound(480, metrics.averageCharWidth() * 72 + 2 * padding, 720));
    static_cast<WrappedContent*>(m_connectionPanel)->updateWrappedHeight();
}

QString Window::selectedId() const
{
    return m_list->currentItem() ? m_list->currentItem()->data(Qt::UserRole).toString() : QString();
}

QString Window::selectedPath() const
{
    const QString id = selectedId();
    for (const Profile& profile : m_profiles->profiles())
        if (profile.id == id) return profile.path;
    return {};
}

void Window::refreshProfiles(const QString& preferredId)
{
    QString id = preferredId.isEmpty() ? selectedId() : preferredId;
    if (id.isEmpty()) id = QSettings().value(QStringLiteral("selectedProfile")).toString();
    const QSignalBlocker blocker(m_list);
    m_list->clear();
    for (const Profile& profile : m_profiles->profiles()) {
        auto* item = new QListWidgetItem(profile.name, m_list);
        item->setData(Qt::UserRole, profile.id);
        item->setToolTip(QStringLiteral("<qt>") + profile.name.toHtmlEscaped() + QStringLiteral("</qt>"));
        if (profile.id == id) m_list->setCurrentItem(item);
        if (profile.path == m_activeProfile) m_sessionLabel->setText(profile.name);
    }
    if (!m_list->currentItem() && m_list->count() > 0) m_list->setCurrentRow(0);
    QSettings().setValue(QStringLiteral("selectedProfile"), selectedId());
    m_empty->setVisible(m_list->count() == 0);
    m_list->setVisible(m_list->count() > 0);
    updateControls();
}

void Window::acquireProfile()
{
    if (m_session->active() || m_closePending) return;
    if (m_acquisition.guide) {
        attention(m_acquisition.guide);
        return;
    }
    if (m_acquisition.active) return;
    ++m_acquisition.generation;
    auto* guide = dialog(this, QStringLiteral("OMAWIN365 · get a Cloud PC connection"));
    m_acquisition.guide = guide;
    auto* layout = new DialogLayout(guide, m_theme);
    auto* content = layout->content();
    layout->details->addWidget(label(QStringLiteral("Get your Microsoft connection"), content, "title"));
    layout->details->addWidget(label(QStringLiteral("Sign in with your work account in the private browser. OMAWIN365 finds your Cloud PCs and downloads the connection for the one you choose. If there is only one, its connection is downloaded automatically."), content));
    layout->details->addWidget(label(QStringLiteral("Only use an account and connection you trust. OMAWIN365 keeps a private copy of the Microsoft connection. It will not connect until you choose Connect."), content, "muted"));
    m_acquisition.status = label(QStringLiteral("Opening private Microsoft sign-in…"), content);
    m_acquisition.status->setAccessibleName(QStringLiteral("Connection download status"));
    layout->details->addWidget(m_acquisition.status);
    auto* resources = new QListWidget(content);
    m_acquisition.resources = resources;
    resources->setAccessibleName(QStringLiteral("Available Microsoft Cloud PCs"));
    resources->setSelectionMode(QAbstractItemView::SingleSelection);
    resources->setTextElideMode(Qt::ElideRight);
    const QFontMetrics metrics(font());
    resources->setMinimumHeight(metrics.lineSpacing() * 4);
    resources->setMaximumHeight(metrics.lineSpacing() * 9);
    resources->hide();
    layout->details->addWidget(resources);
    auto* download = new WrappedButton(QStringLiteral("&Download selected connection"), guide);
    m_acquisition.download = download;
    download->setProperty("primary", true);
    download->setEnabled(false);
    download->hide();
    layout->controls->addWidget(download);
    auto* manual = new WrappedButton(QStringLiteral("&Import a trusted .rdpw instead…"), guide);
    manual->setAutoDefault(false);
    layout->controls->addWidget(manual);
    auto* cancel = new WrappedButton(QStringLiteral("&Cancel"), guide);
    cancel->setAutoDefault(false);
    layout->controls->addWidget(cancel);
    const auto updateSelection = [this, guide, resources, download] {
        if (m_acquisition.guide == guide && m_acquisition.active) {
            const auto* item = resources->currentItem();
            download->setEnabled(item && item->isSelected());
        }
    };
    connect(resources, &QListWidget::currentRowChanged, guide, updateSelection);
    connect(resources, &QListWidget::itemSelectionChanged, guide, updateSelection);
    connect(resources, &QListWidget::itemActivated, guide, [download] { download->click(); });
    connect(download, &QPushButton::clicked, guide, [this, guide, resources, download] {
        if (m_acquisition.guide != guide || !m_acquisition.active || !resources->currentItem()
            || !resources->currentItem()->isSelected()) return;
        const QString id = resources->currentItem()->data(Qt::UserRole).toString();
        if (id.isEmpty()) return;
        resources->setEnabled(false);
        download->setEnabled(false);
        m_acquisition.status->setText(QStringLiteral("Downloading the selected connection…"));
        m_browser->selectResource(id);
    });
    connect(manual, &QPushButton::clicked, guide, [this, guide] {
        if (m_acquisition.guide != guide) return;
        const quint64 generation = m_acquisition.generation;
        guide->reject();
        // Let the modal guide relinquish focus before opening the file chooser.
        QTimer::singleShot(0, this, [this, generation] {
            if (generation == m_acquisition.generation && !m_acquisition.guide
                && !m_acquisition.active && !m_session->active() && !m_closePending) m_import->click();
        });
    });
    connect(cancel, &QPushButton::clicked, guide, &QDialog::reject);
    connect(guide, &QDialog::finished, this, [this, guide](int) {
        if (m_acquisition.guide != guide) return;
        m_acquisition.guide = nullptr;
        m_acquisition.status = nullptr;
        m_acquisition.resources = nullptr;
        m_acquisition.download = nullptr;
        if (m_acquisition.active) {
            m_acquisition.active = false;
            m_browser->cancel();
            setStatus("disconnected", QStringLiteral("Connection download cancelled. You can import a trusted .rdpw file instead."));
        }
        updateControls();
    });
    m_acquisition.active = true;
    updateControls();
    setStatus("acquiring", QStringLiteral("Sign in in the private browser to find your Cloud PCs."));
    layout->fit();
    attention(guide);
    cancel->setFocus();
    if (m_acquisition.guide == guide && m_acquisition.active) m_browser->beginProfileDownload();
}

bool Window::importProfile(const QString& sourcePath)
{
    QMessageBox confirm(QMessageBox::Question, QStringLiteral("Import trusted connection"),
        QStringLiteral("Only import connection files from a source you trust. An RDP file chooses remote servers and connection settings.\n\nOMAWIN365 will keep a private copy. It will not connect until you choose Connect."),
        QMessageBox::Cancel | QMessageBox::Ok, this);
    if (!confirmProfileAction(confirm, m_theme, QMessageBox::Ok, QStringLiteral("Import"))) return false;
    QString error;
    const Profile profile = m_profiles->importFile(sourcePath, &error);
    if (profile.id.isEmpty()) {
        showError(QStringLiteral("Import failed"), error);
        return false;
    }
    refreshProfiles(profile.id);
    m_list->setFocus();
    if (!m_session->active()) setStatus("disconnected", QStringLiteral("Connection imported. Select Connect when ready."));
    return true;
}

void Window::removeSelected()
{
    const QString id = selectedId();
    if (id.isEmpty() || m_session->active()) return;
    QMessageBox confirm(QMessageBox::Question, QStringLiteral("Remove connection"),
        QStringLiteral("Remove this connection from OMAWIN365? Only the app's private copy is removed. Your original file is not changed."),
        QMessageBox::Cancel | QMessageBox::Yes, this);
    if (!confirmProfileAction(confirm, m_theme, QMessageBox::Yes, QStringLiteral("Remove"))) return;
    QString error;
    if (!m_profiles->removeProfile(id, &error)) showError(QStringLiteral("Could not remove connection"), error);
}

void Window::renameSelected()
{
    if (!m_rename->isEnabled()) return;
    if (m_renameDialog) {
        attention(m_renameDialog);
        return;
    }
    const QString id = selectedId();
    QString currentName;
    bool found = false;
    for (const Profile& profile : m_profiles->profiles()) {
        if (profile.id == id) {
            currentName = profile.name;
            found = true;
            break;
        }
    }
    if (!found) return;
    auto* prompt = dialog(this, QStringLiteral("Rename Cloud PC label"));
    m_renameDialog = prompt;
    auto* layout = new DialogLayout(prompt, m_theme);
    auto* content = layout->content();
    layout->details->addWidget(label(QStringLiteral("Rename in OMAWIN365"), content, "title"));
    layout->details->addWidget(label(QStringLiteral("This changes only the label in this app. Your Microsoft Cloud PC and its connection settings are not changed."), content, "muted"));
    auto* nameLabel = label(QStringLiteral("Cloud PC &label"), content);
    auto* name = new QLineEdit(currentName, content);
    name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    name->setAccessibleName(QStringLiteral("Cloud PC label"));
    name->setMaxLength(160);
    name->setValidator(new ProfileNameValidator(name));
    nameLabel->setBuddy(name);
    layout->details->addWidget(nameLabel);
    layout->details->addWidget(name);
    auto* errorLabel = label({}, content, "error");
    errorLabel->hide();
    layout->details->addWidget(errorLabel);
    auto* save = new WrappedButton(QStringLiteral("&Save"), prompt);
    save->setProperty("primary", true);
    save->setDefault(true);
    save->setEnabled(name->hasAcceptableInput());
    auto* cancel = new WrappedButton(QStringLiteral("&Cancel"), prompt);
    layout->controls->addWidget(save);
    layout->controls->addWidget(cancel);
    connect(name, &QLineEdit::textChanged, prompt, [name, save, errorLabel] {
        save->setEnabled(name->hasAcceptableInput());
        errorLabel->hide();
    });
    connect(save, &QPushButton::clicked, prompt, [this, prompt, id, name, errorLabel, layout] {
        if (m_renameDialog != prompt || !name->hasAcceptableInput() || m_acquisition.active || m_closePending) return;
        QString error;
        if (!m_profiles->renameProfile(id, name->text(), &error)) {
            errorLabel->setText(error);
            errorLabel->show();
            layout->fit(true);
            return;
        }
        refreshProfiles(id);
        prompt->accept();
        m_list->setFocus();
    });
    connect(cancel, &QPushButton::clicked, prompt, &QDialog::reject);
    connect(prompt, &QDialog::finished, this, [this, prompt] {
        if (m_renameDialog == prompt) m_renameDialog = nullptr;
    });
    layout->fit();
    attention(prompt);
    name->selectAll();
    name->setFocus();
}

void Window::updateControls()
{
    const bool active = m_session->active();
    const bool selected = !selectedId().isEmpty();
    m_connect->setEnabled(selected && !active && !m_acquisition.active && !m_closePending && m_restartPath.isEmpty());
    m_connect->setVisible(!active);
    m_disconnect->setEnabled(active && !m_stopping);
    m_reconnect->setEnabled(active && !m_stopping && !m_closePending);
    m_disconnect->setVisible(active);
    m_reconnect->setVisible(active);
    m_disconnect->parentWidget()->setVisible(active);
    m_remove->setEnabled(selected && !active && !m_acquisition.active);
    m_rename->setEnabled(selected && !m_acquisition.active && !m_closePending && !m_stopping
        && !m_challenge.dialog && !m_closeConfirmation
        && m_phase != "awaiting-pin" && m_phase != "awaiting-touch");
    m_list->setEnabled(!active && !m_acquisition.active);
    m_import->setEnabled(!active && !m_acquisition.active && !m_closePending);
    m_acquire->setEnabled(!active && !m_acquisition.active && !m_closePending);
}

void Window::setStatus(const QString& phase, const QString& detail)
{
    m_phase = phase;
    m_phaseLabel->setText(phaseTitle(phase));
    m_phaseLabel->setProperty("role", phase == "error" || phase == "acquisition-error" ? "error" : "phase");
    m_phaseLabel->style()->unpolish(m_phaseLabel);
    m_phaseLabel->style()->polish(m_phaseLabel);
    m_detailLabel->setText(detail);
    m_progressNote->setText(QStringLiteral("Connection setup may ask you to sign in to Microsoft more than once. Follow each prompt; you do not need to press Connect again."));
    m_progressNote->setVisible(!m_connected && !m_stopping && !m_acquisition.active
        && (phase == "connecting" || phase == "signing-in"));
    updateControls();
}

void Window::connectSelected()
{
    if (m_session->active() || m_acquisition.active || m_closePending || !m_restartPath.isEmpty()) return;
    const QString path = selectedPath();
    if (path.isEmpty()) return;
    m_errorVisible = false;
    m_connected = false;
    m_stopping = false;
    m_activeProfile = path;
    m_sessionLabel->setText(m_list->currentItem()->text());
    m_sessionLabel->show();
    setStatus("connecting", QStringLiteral("Starting FreeRDP…"));
    m_session->start(path);
    updateControls();
}

void Window::disconnectSession()
{
    m_stopping = true;
    dismissChallenge();
    m_browser->cancel();
    if (!m_errorVisible) setStatus("disconnecting", QStringLiteral("Closing the desktop and private sign-in window…"));
    m_session->stop();
    updateControls();
}

void Window::reconnectSession()
{
    if (!m_session->active() || m_stopping || m_activeProfile.isEmpty()) return;
    m_restartPath = m_activeProfile;
    disconnectSession();
}

void Window::dismissChallenge()
{
    if (m_challenge.pin) m_challenge.pin->clear();
    m_dismissingChallenge = true;
    if (m_challenge.dialog) m_challenge.dialog->reject();
    m_challenge.dialog = nullptr;
    m_challenge.pin = nullptr;
    m_dismissingChallenge = false;
}

bool Window::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_challenge.pin && m_challenge.dialog && !m_challenge.dialog->isActiveWindow()
        && (event->type() == QEvent::KeyPress || event->type() == QEvent::InputMethod))
        return true;
    if (watched == m_challenge.dialog && (event->type() == QEvent::WindowDeactivate
        || event->type() == QEvent::ActivationChange || event->type() == QEvent::WindowActivate)) {
        if (event->type() == QEvent::WindowDeactivate && m_challenge.pin) {
            m_challenge.pin->clear();
            m_challenge.pin->setEnabled(false);
            if (m_challenge.focus && m_phase == "awaiting-pin") m_challenge.focus->show();
            if (m_challenge.submit && m_phase == "awaiting-pin") m_challenge.submit->setEnabled(false);
        }
        QTimer::singleShot(0, this, &Window::updatePinFocus);
    }
    return QWidget::eventFilter(watched, event);
}

void Window::updatePinFocus()
{
    if (!m_challenge.dialog || !m_challenge.pin || m_phase != "awaiting-pin") return;
    const bool active = m_challenge.dialog->isActiveWindow();
    m_challenge.pin->setEnabled(active);
    if (!active) m_challenge.pin->clear();
    if (m_challenge.focus) m_challenge.focus->setVisible(!active);
    if (m_challenge.submit) m_challenge.submit->setEnabled(active && m_challenge.pin->hasAcceptableInput());
    refit(m_challenge.dialog);
    if (active) m_challenge.pin->setFocus(Qt::OtherFocusReason);
}

void Window::requestPin(const QString& message)
{
    if (!m_session->active() || m_stopping) return;
    if (m_renameDialog) m_renameDialog->reject();
    dismissChallenge();
    setStatus("awaiting-pin", QStringLiteral("Enter your security key PIN locally, then touch the key."));
    auto* prompt = dialog(this, QStringLiteral("OMAWIN365 · security key PIN"));
    m_challenge.dialog = prompt;
    prompt->installEventFilter(this);
    auto* layout = new DialogLayout(prompt, m_theme);
    auto* content = layout->content();
    m_challenge.title = label(QStringLiteral("Security key PIN"), content, "title");
    layout->details->addWidget(m_challenge.title);
    if (!message.isEmpty()) layout->details->addWidget(label(message, content));
    m_challenge.detail = label(QStringLiteral("Enter the PIN for your local security key. It is sent privately to FreeRDP, not the browser. You will then need to touch the key."), content);
    layout->details->addWidget(m_challenge.detail);
    layout->details->addWidget(label(QStringLiteral("Cancel closes this desktop session: FreeRDP cannot safely cancel just this key request. No automatic PIN retry."), content, "muted"));
    m_challenge.focus = new WrappedButton(QStringLiteral("Click here to enter your YubiKey PIN"), prompt);
    layout->controls->addWidget(m_challenge.focus);
    connect(m_challenge.focus, &QPushButton::clicked, prompt, [this, prompt] {
        attention(prompt);
        QTimer::singleShot(0, this, &Window::updatePinFocus);
    });
    auto* pinLabel = label(QStringLiteral("&PIN"), prompt);
    layout->controls->addWidget(pinLabel);
    auto* input = new QLineEdit(prompt);
    input->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_challenge.pin = input;
    input->installEventFilter(this);
    input->setEchoMode(QLineEdit::Password);
    input->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText);
    input->setAccessibleName(QStringLiteral("Security key PIN"));
    input->setMaxLength(63);
    input->setValidator(new PinValidator(input));
    input->setEnabled(false);
    pinLabel->setBuddy(input);
    layout->controls->addWidget(input);
    auto* cancel = new WrappedButton(QStringLiteral("Cancel and dis&connect"), prompt);
    m_challenge.submit = new WrappedButton(QStringLiteral("&Submit PIN"), prompt);
    m_challenge.submit->setProperty("primary", true);
    m_challenge.submit->setDefault(true);
    m_challenge.submit->setEnabled(false);
    layout->controls->addWidget(cancel);
    layout->controls->addWidget(m_challenge.submit);
    connect(input, &QLineEdit::textChanged, prompt, [this, prompt] {
        if (m_challenge.dialog == prompt && m_challenge.submit && m_phase == "awaiting-pin")
            m_challenge.submit->setEnabled(prompt->isActiveWindow() && m_challenge.pin->hasAcceptableInput());
    });
    connect(m_challenge.submit, &QPushButton::clicked, prompt, [this, prompt] {
        if (m_challenge.dialog != prompt || !m_session->active() || m_stopping) return;
        if (m_phase == "awaiting-touch") {
            dismissChallenge();
            if (m_connected)
                setStatus("connected", QStringLiteral("Desktop remains connected. Check the remote app for the sign-in result; OMAWIN365 cannot confirm security key authentication."));
            else
                setStatus("connecting", QStringLiteral("Check the remote app for the sign-in result; PIN submission is not proof of authentication."));
            return;
        }
        if (m_phase != "awaiting-pin" || !m_challenge.pin || !prompt->isActiveWindow()
            || !m_challenge.pin->hasAcceptableInput()) return;
        QString pin = m_challenge.pin->text();
        m_challenge.pin->clear();
        m_challenge.pin->setEnabled(false);
        m_session->submitPin(pin);
        pin.fill(QChar(0));
        pin.clear();
        // Clearing Qt strings limits retention; it is not a zeroization guarantee.
        if (m_session->active() && !m_stopping && m_challenge.dialog == prompt) {
            setStatus("awaiting-touch", QStringLiteral("PIN submitted. Touch your key, then check the remote application for the result."));
            showTouch();
        }
    });
    connect(cancel, &QPushButton::clicked, prompt, &QDialog::reject);
    connect(prompt, &QDialog::rejected, this, [this, prompt] {
        if (m_challenge.dialog != prompt || m_dismissingChallenge) return;
        if (m_challenge.pin) m_challenge.pin->clear();
        m_challenge.dialog = nullptr;
        m_restartPath.clear();
        disconnectSession();
    });
    layout->fit();
    attention(prompt);
    QTimer::singleShot(0, this, &Window::updatePinFocus);
}

void Window::showTouch()
{
    if (!m_challenge.dialog || !m_challenge.pin || !m_challenge.submit) return;
    m_challenge.pin->clear();
    m_challenge.pin->hide();
    if (m_challenge.focus) m_challenge.focus->hide();
    if (QWidget* buddy = m_challenge.pin->parentWidget()) {
        for (QLabel* text : buddy->findChildren<QLabel*>())
            if (text->buddy() == m_challenge.pin) text->hide();
    }
    m_challenge.dialog->setWindowTitle(QStringLiteral("OMAWIN365 · touch your security key"));
    m_challenge.title->setText(QStringLiteral("Touch your security key"));
    m_challenge.detail->setText(QStringLiteral("PIN submitted. Touch the physical key when it flashes. Check the remote app for success or failure: FreeRDP provides no reliable authentication-success event to OMAWIN365."));
    m_challenge.submit->setText(QStringLiteral("&Done"));
    m_challenge.submit->setEnabled(true);
    refit(m_challenge.dialog);
    m_challenge.submit->setFocus();
}

void Window::showError(const QString& title, const QString& message)
{
    auto* box = new QMessageBox(QMessageBox::Warning, title, message, QMessageBox::Ok, this);
    box->setTextFormat(Qt::PlainText);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::ApplicationModal);
    themeMessageBox(box, m_theme);
    box->open();
    QApplication::alert(this, 0);
}

void Window::closeEvent(QCloseEvent* event)
{
    if (!m_session->active()) {
        m_acquisition.active = false;
        m_browser->cancel();
        event->accept();
        return;
    }
    event->ignore();
    if (m_closePending) return;
    if (m_closeConfirmation) {
        attention(m_closeConfirmation);
        return;
    }
    if (m_renameDialog) m_renameDialog->reject();
    auto* prompt = dialog(this, QStringLiteral("Close OMAWIN365?"));
    m_closeConfirmation = prompt;
    updateControls();
    auto* layout = new DialogLayout(prompt, m_theme);
    layout->details->addWidget(label(QStringLiteral("Closing OMAWIN365 disconnects your active desktop session and closes its private sign-in browser."), layout->content()));
    auto* cancel = new WrappedButton(QStringLiteral("&Keep open"), prompt);
    auto* close = new WrappedButton(QStringLiteral("&Disconnect and close"), prompt);
    cancel->setDefault(true);
    layout->controls->addWidget(cancel);
    layout->controls->addWidget(close);
    connect(cancel, &QPushButton::clicked, prompt, &QDialog::reject);
    connect(close, &QPushButton::clicked, prompt, &QDialog::accept);
    connect(prompt, &QDialog::finished, this, [this](int result) {
        m_closeConfirmation = nullptr;
        updateControls();
        if (result != QDialog::Accepted) return;
        m_closePending = true;
        m_restartPath.clear();
        if (m_session->active()) disconnectSession();
        else QTimer::singleShot(0, this, &QWidget::close);
    });
    layout->fit();
    attention(prompt);
    cancel->setFocus();
}
