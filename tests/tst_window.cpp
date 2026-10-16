#include "synthetic_profile.h"
#include "certificate_fixture.h"
#include <QTest>

#include "window.h"
#include "session.h"
#include "browserauth.h"
#include "profilestore.h"
#include "window_fixture.h"
#include "windowpresentation.h"
#include "theme.h"
#include "displayscale.h"

#include <QApplication>
#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QVBoxLayout>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <memory>
#include <sys/stat.h>

using WindowFixture::controls;

// Hyprland's IPC socket on its own thread: DisplayScale queries it synchronously.
class FakeHyprland {
public:
    explicit FakeHyprland(const QString& path)
    {
        m_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const QByteArray encoded = QFile::encodeName(path);
        std::memcpy(address.sun_path, encoded.constData(), std::min<size_t>(encoded.size(), sizeof(address.sun_path) - 1));
        m_ok = m_fd >= 0 && ::bind(m_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && ::listen(m_fd, 8) == 0;
        if (m_ok) m_thread = std::thread([this] { serve(); });
    }
    ~FakeHyprland()
    {
        m_stop = true;
        if (m_thread.joinable()) m_thread.join();
        if (m_fd >= 0) ::close(m_fd);
    }
    bool ok() const { return m_ok; }
    void reply(const QByteArray& bytes, bool silent = false)
    {
        std::lock_guard lock(m_mutex);
        m_reply = bytes;
        m_silent = silent;
    }
    QList<QByteArray> requests()
    {
        std::lock_guard lock(m_mutex);
        return m_requests;
    }
private:
    void serve()
    {
        while (!m_stop) {
            pollfd waiting{m_fd, POLLIN, 0};
            if (::poll(&waiting, 1, 20) <= 0) continue;
            const int client = ::accept4(m_fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) continue;
            char buffer[256];
            pollfd readable{client, POLLIN, 0};
            const ssize_t got = ::poll(&readable, 1, 500) > 0 ? ::read(client, buffer, sizeof(buffer)) : 0;
            QByteArray reply;
            bool silent = false;
            {
                std::lock_guard lock(m_mutex);
                m_requests.append(QByteArray(buffer, int(std::max<ssize_t>(got, 0))));
                reply = m_reply;
                silent = m_silent;
            }
            if (silent) {
                for (int i = 0; i < 50 && !m_stop; ++i) ::usleep(20000); // Hold the connection, say nothing.
            } else {
                for (qsizetype sent = 0; sent < reply.size();) {
                    const ssize_t wrote = ::send(client, reply.constData() + sent, size_t(reply.size() - sent), MSG_NOSIGNAL);
                    if (wrote <= 0) break;
                    sent += wrote;
                }
            }
            ::close(client);
        }
    }
    int m_fd = -1;
    bool m_ok = false;
    std::atomic<bool> m_stop = false;
    std::thread m_thread;
    std::mutex m_mutex;
    QByteArray m_reply;
    bool m_silent = false;
    QList<QByteArray> m_requests;
};
namespace {
const QByteArray profileBytes = supportedProfile;
bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()
        && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}
QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
template<class T> T* accessible(QWidget* root, const QString& name)
{
    for (T* child : root->findChildren<T*>())
        if (child->accessibleName() == name) return child;
    return nullptr;
}
QPushButton* button(QWidget* root, const QString& caption)
{
    for (auto* child : root->findChildren<QPushButton*>()) {
        QString text = child->text();
        text.remove('&');
        if (text == caption) return child;
    }
    return nullptr;
}
bool click(QWidget* root, const QString& caption)
{
    auto* target = button(root, caption);
    if (!target || !target->isVisible() || !target->isEnabled()) return false;
    QTest::mouseClick(target, Qt::LeftButton);
    return true;
}
bool visibleLabelText(QWidget* root, const QString& text)
{
    for (auto* child : root->findChildren<QLabel*>())
        if (child->isVisible() && child->text() == text) return true;
    return false;
}
QDialog* prompt(QWidget* root, const QString& title)
{
    for (auto* child : root->findChildren<QDialog*>())
        if (child->isVisible() && child->windowTitle() == title) return child;
    return nullptr;
}
bool fullyVisible(QWidget* widget)
{
    return widget && widget->isVisible() && QRegion(widget->rect()).subtracted(widget->visibleRegion()).isEmpty();
}
bool reveal(QWidget* widget)
{
    if (!widget) return false;
    for (QWidget* parent = widget->parentWidget(); parent; parent = parent->parentWidget()) {
        if (auto* scroll = qobject_cast<QScrollArea*>(parent)) {
            scroll->ensureWidgetVisible(widget, 0, 0);
            QCoreApplication::processEvents();
        }
    }
    return fullyVisible(widget);
}
QWidget* acquisitionPanel(QWidget* root)
{
    for (auto* panel : root->findChildren<QWidget*>("acquisitionPanel"))
        if (panel->isVisible()) return panel;
    return nullptr;
}
void settle()
{
    QTest::qWait(30);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
}

class WindowTests final : public QObject {
    Q_OBJECT
private:
    std::unique_ptr<QTemporaryDir> temporary;
    std::unique_ptr<Session> session;
    std::unique_ptr<BrowserAuth> browser;
    std::unique_ptr<ProfileStore> store;
    std::unique_ptr<Window> window;
    QString source;

    Profile seed(const QString& name = "Saved Cloud PC")
    {
        QString error;
        auto result = store->importFile(source, &error, name);
        if (result.id.isEmpty()) qFatal("Synthetic seed failed: %s", qPrintable(error));
        return result;
    }
    void openWindow()
    {
        window = std::make_unique<Window>(session.get(), browser.get(), store.get());
        window->show();
        window->activateWindow();
        settle();
    }
    QListWidget* profiles() const
    { return accessible<QListWidget>(window.get(), "Cloud PC profiles"); }
    QLabel* phase() const
    { return accessible<QLabel>(window.get(), "Connection status"); }
    void finish()
    {
        controls.active = false;
        emit session->ended();
        settle();
    }
    // exec() confirmations need a queued real button interaction. A watchdog
    // closes only this test's modal dialog so a selector failure cannot hang.
    void confirmation(const QString& title, QMessageBox::StandardButton response, int key = 0)
    {
        QTimer::singleShot(0, window.get(), [this, title, response, key] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!box) { QTest::qFail("Expected a modal QMessageBox", __FILE__, __LINE__); return; }
            QCOMPARE(box->windowTitle(), title);
            QCOMPARE(box->defaultButton(), box->button(QMessageBox::Cancel));
            if (title == "Import trusted connection")
                QCOMPARE(box->button(QMessageBox::Ok)->text(), "Import");
            else {
                QCOMPARE(title, "Remove connection");
                QCOMPARE(box->button(QMessageBox::Yes)->text(), "Remove");
            }
            if (key) QTest::keyClick(box, Qt::Key(key));
            else QTest::mouseClick(box->button(response), Qt::LeftButton);
        });
        QTimer::singleShot(1000, window.get(), [] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                QTest::qFail("Confirmation did not finish via its selected button", __FILE__, __LINE__);
                box->reject();
            }
        });
    }
private slots:
    void sessionSettingsFollowProfilesAndReconnect()
    {
        const auto first = seed("Audio PC");
        seed("Silent PC");
        openWindow();
        profiles()->setCurrentRow(0);
        auto* playback = window->findChild<QCheckBox*>("audioPlayback");
        auto* capture = window->findChild<QCheckBox*>("audioCapture");
        auto* clipboard = accessible<QCheckBox>(window.get(), "Share clipboard");
        auto* pending = window->findChild<QLabel*>("optionsPending");
        QVERIFY(playback && capture && clipboard && pending);
        QVERIFY(playback->isEnabled() && capture->isEnabled() && clipboard->isEnabled());
        QVERIFY(!playback->isChecked() && !capture->isChecked() && clipboard->isChecked());
        playback->setFocus();
        QTest::keyClick(playback, Qt::Key_Space);
        capture->click();
        QCOMPARE(controls.starts.size(), 0);
        QCOMPARE(store->profiles().first().devices.audioPlayback, true);
        QCOMPARE(store->profiles().first().devices.audioCapture, true);
        profiles()->setCurrentRow(1);
        QVERIFY(!playback->isChecked() && !capture->isChecked());
        profiles()->setCurrentRow(0);
        QVERIFY(playback->isChecked() && capture->isChecked());
        QVERIFY(click(window.get(), "Connect"));
        QCOMPARE(controls.options.size(), 1);
        QVERIFY(controls.options.first().devices.audioPlayback && controls.options.first().devices.audioCapture);
        QVERIFY(!pending->isVisible());
        // Still editable while connected: the change is saved and waits for Reconnect.
        QVERIFY(capture->isEnabled());
        capture->click();
        QVERIFY(!store->profiles().first().devices.audioCapture);
        QVERIFY(pending->isVisible());
        QCOMPARE(controls.stops, 0);
        QCOMPARE(controls.starts.size(), 1);
        capture->click();
        QVERIFY(!pending->isVisible()); // Back to what the running session uses.
        // Another Cloud PC's settings never mark this session pending.
        profiles()->setCurrentRow(1);
        QVERIFY(!playback->isChecked() && !capture->isChecked());
        clipboard->click();
        QVERIFY(!store->profiles().last().devices.clipboard);
        QVERIFY(!pending->isVisible());
        profiles()->setCurrentRow(0);
        capture->click();
        QVERIFY(pending->isVisible());
        // Reconnect uses the connected Cloud PC's settings even when another is highlighted.
        profiles()->setCurrentRow(1);
        QVERIFY(click(window.get(), "Reconnect"));
        finish();
        QCOMPARE(controls.starts.size(), 2);
        QCOMPARE(controls.starts.last(), first.path);
        QVERIFY(controls.options.last().devices.audioPlayback && !controls.options.last().devices.audioCapture);
        QVERIFY(controls.options.last().devices.clipboard);
        QVERIFY(!pending->isVisible());
    }

    void audioOptionsDisabledWithoutProfileAndRestoredOnReopen()
    {
        openWindow();
        auto* playback = window->findChild<QCheckBox*>("audioPlayback");
        auto* capture = window->findChild<QCheckBox*>("audioCapture");
        QVERIFY(playback && capture);
        QVERIFY(!playback->isEnabled() && !capture->isEnabled());
        seed();
        QVERIFY(playback->isEnabled() && capture->isEnabled());
        capture->click();
        window.reset();
        store = std::make_unique<ProfileStore>();
        openWindow();
        QVERIFY(!window->findChild<QCheckBox*>("audioPlayback")->isChecked());
        QVERIFY(window->findChild<QCheckBox*>("audioCapture")->isChecked());
        QCOMPARE(controls.starts.size(), 0);
    }

    void rememberSignInChoices()
    {
        openWindow();
        auto* remember = window->findChild<QComboBox*>("rememberSignIn");
        auto* forget = window->findChild<QPushButton*>("forgetSignIn");
        auto* note = window->findChild<QLabel*>("signInCacheNote");
        QVERIFY(remember && forget && note);
        QCOMPARE(remember->currentText(), "Off"); // Off is the default.
        QCOMPARE(controls.rememberSignIn, 0);
        QVERIFY(forget->isHidden()); // Nothing to forget.
        remember->setCurrentIndex(1);
        QCOMPARE(controls.rememberSignIn, 1);
        QCOMPARE(QSettings().value("rememberSignIn").toString(), "app");
        QVERIFY(!forget->isHidden());
        QVERIFY(note->text().contains("until OMAWIN365 closes"));
        QCOMPARE(controls.forgets + controls.savedForgets, 0);
        remember->setCurrentIndex(2);
        QCOMPARE(controls.rememberSignIn, 2);
        QCOMPARE(QSettings().value("rememberSignIn").toString(), "device");
        // Leaving the keyring setting deletes the saved item but keeps memory.
        remember->setCurrentIndex(1);
        QCOMPARE(controls.savedForgets, 1);
        QCOMPARE(controls.rememberSignIn, 1);
        QVERIFY(!remember->isEnabled() && !forget->isEnabled());
        QVERIFY(!window->close()); // An unfinished deletion must not be abandoned.
        emit browser->signInForgotten(true);
        QVERIFY(remember->isEnabled());
        QVERIFY(note->text().contains("Removed from the system keyring") && note->text().contains("until OMAWIN365 closes"));
        // Off forgets everything.
        remember->setCurrentIndex(0);
        QCOMPARE(controls.forgets, 1);
        QCOMPARE(controls.rememberSignIn, 0);
        emit browser->signInForgotten(false);
        QVERIFY(note->text().contains("could not remove"));
        QVERIFY(!forget->isHidden() && forget->isEnabled()); // So the user can try again.
        forget->click();
        QCOMPARE(controls.forgets, 2);
        emit browser->signInForgotten(true);
        QVERIFY(note->text().contains("removed"));
        QVERIFY(forget->isHidden());
        QCOMPARE(QSettings().value("rememberSignIn").toString(), "off");
        // Forget from a remembering setting turns it off too.
        remember->setCurrentIndex(2);
        forget->click();
        QCOMPARE(controls.forgets, 3);
        QCOMPARE(remember->currentText(), "Off");
        QCOMPARE(controls.rememberSignIn, 0);
        emit browser->signInForgotten(true);
    }
    void failedForgetOfferedAfterRestart()
    {
        openWindow();
        window->findChild<QComboBox*>("rememberSignIn")->setCurrentIndex(2);
        window->findChild<QPushButton*>("forgetSignIn")->click();
        emit browser->signInForgotten(false);
        window.reset();
        openWindow(); // Off is restored, but the keyring item may still be there.
        auto* forget = window->findChild<QPushButton*>("forgetSignIn");
        QCOMPARE(window->findChild<QComboBox*>("rememberSignIn")->currentText(), "Off");
        QVERIFY(!forget->isHidden() && forget->isEnabled());
        QVERIFY(window->findChild<QLabel*>("signInCacheNote")->text().contains("may still be in the system keyring"));
        forget->click();
        emit browser->signInForgotten(true);
        QVERIFY(forget->isHidden());
        window.reset();
        openWindow();
        QVERIFY(window->findChild<QPushButton*>("forgetSignIn")->isHidden());
    }
    void rememberSignInRestoredOnReopen()
    {
        QSettings().setValue("rememberSignIn", "device");
        openWindow();
        QCOMPARE(window->findChild<QComboBox*>("rememberSignIn")->currentIndex(), 2);
        QCOMPARE(controls.rememberSignIn, 2);
        window.reset();
        QSettings().setValue("rememberSignIn", true); // An earlier draft's value means Off.
        openWindow();
        QCOMPARE(window->findChild<QComboBox*>("rememberSignIn")->currentIndex(), 0);
        QCOMPARE(controls.rememberSignIn, 0);
    }
    void signInControlsDisabledDuringConnection()
    {
        seed(); openWindow();
        QVERIFY(click(window.get(), "Connect"));
        QVERIFY(!window->findChild<QComboBox*>("rememberSignIn")->isEnabled());
        QVERIFY(!window->findChild<QPushButton*>("forgetSignIn")->isEnabled());
        finish();
        QVERIFY(window->findChild<QComboBox*>("rememberSignIn")->isEnabled());
    }
    void acquisitionInitialFocusAndAutomaticScroll_data()
    {
        QTest::addColumn<QSize>("bounds");
        QTest::newRow("natural") << QSize();
        QTest::newRow("short") << QSize(252, 120);
    }
    void acquisitionInitialFocusAndAutomaticScroll()
    {
        QFETCH(QSize, bounds);
        openWindow();
        if (bounds.isValid()) { window->resize(bounds); settle(); }
        auto* get = button(window.get(), "Get a Cloud PC connection");
        QVERIFY(get);
        get->setFocus();
        QTRY_COMPARE(QApplication::focusWidget(), get);
        get->click();
        auto* panel = acquisitionPanel(window.get());
        QVERIFY(panel);
        auto* cancel = button(panel, "Cancel");
        QVERIFY(cancel);
        settle();
        // No reveal(), ensureWidgetVisible() or scrollbar manipulation here.
        QVERIFY2(fullyVisible(cancel), "Automatic scroll did not expose complete Cancel");
        QCOMPARE(QApplication::focusWidget(), cancel);
        cancel->click();
        settle();
        QCOMPARE(QApplication::focusWidget(), get);
        QCOMPARE(controls.cancellations, 1);
        QVERIFY(controls.starts.isEmpty());
    }
    void acquisitionTabOrderAndUserFocus()
    {
        openWindow();
        auto* get = button(window.get(), "Get a Cloud PC connection");
        auto* about = button(window.get(), "About…");
        QVERIFY(get && about);
        get->setFocus();
        QTRY_COMPARE(QApplication::focusWidget(), get);
        get->click();
        auto* panel = acquisitionPanel(window.get());
        QVERIFY(panel);
        auto* cancel = button(panel, "Cancel");
        auto* manual = button(panel, "Import a trusted .rdpw instead…");
        auto* resources = accessible<QListWidget>(panel, "Available Microsoft Cloud PCs");
        auto* download = button(panel, "Download selected connection");
        QVERIFY(cancel && manual && resources && download);
        QCOMPARE(QApplication::focusWidget(), cancel);
        // A user's move before the queued layout/scroll must not be overwritten.
        QTest::keyClick(cancel, Qt::Key_Backtab);
        QCOMPARE(QApplication::focusWidget(), manual);
        settle();
        QCOMPARE(QApplication::focusWidget(), manual);
        QTest::keyClick(manual, Qt::Key_Tab);
        QCOMPARE(QApplication::focusWidget(), cancel);
        QTest::keyClick(cancel, Qt::Key_Tab);
        QCOMPARE(QApplication::focusWidget(), about);
        QTest::keyClick(about, Qt::Key_Backtab);
        QCOMPARE(QApplication::focusWidget(), cancel);
        emit browser->resourcesAvailable({"pc-a", "pc-b"}, {"Cloud A", "Cloud B"});
        settle();
        QCOMPARE(QApplication::focusWidget(), resources);
        resources->setCurrentRow(0);
        QVERIFY(download->isEnabled());
        QTest::keyClick(resources, Qt::Key_Tab);
        QCOMPARE(QApplication::focusWidget(), download);
        QTest::keyClick(download, Qt::Key_Tab);
        QCOMPARE(QApplication::focusWidget(), manual);
        QTest::keyClick(manual, Qt::Key_Tab);
        QCOMPARE(QApplication::focusWidget(), cancel);
        // Cancel invoked while the user has deliberately left the panel does
        // not force focus back to Get.
        about->setFocus();
        QCOMPARE(QApplication::focusWidget(), about);
        cancel->click();
        settle();
        QCOMPARE(QApplication::focusWidget(), about);
        QCOMPARE(controls.cancellations, 1);
        QVERIFY(controls.starts.isEmpty());
    }
    void acquisitionPresentationPreservesExternalFocus()
    {
        openWindow();
        QWidget other;
        auto* otherButton = new QPushButton("Synthetic other window", &other);
        controls.onAcquire = [&other, otherButton] {
            other.show();
            other.activateWindow();
            otherButton->setFocus();
        };
        auto* get = button(window.get(), "Get a Cloud PC connection");
        QVERIFY(get);
        get->setFocus();
        QTRY_COMPARE(QApplication::focusWidget(), get);
        get->click();
        QTRY_VERIFY(other.isActiveWindow());
        QTRY_COMPARE(QApplication::focusWidget(), otherButton);
        settle();
        QVERIFY(!window->isActiveWindow());
        QCOMPARE(QApplication::focusWidget(), otherButton);
        auto* panel = acquisitionPanel(window.get());
        QVERIFY(panel);
        auto* cancel = button(panel, "Cancel");
        QVERIFY(cancel);
        cancel->click(); // An external/queued cancellation is not activation.
        settle();
        QVERIFY(other.isActiveWindow());
        QCOMPARE(QApplication::focusWidget(), otherButton);
        QCOMPARE(controls.cancellations, 1);
        QVERIFY(controls.starts.isEmpty());
        controls.onAcquire = {};
    }
    void downloadErrorReadable_data()
    {
        QTest::addColumn<int>("fontSize");
        QTest::addColumn<QSize>("bounds");
        QTest::newRow("body12-natural") << 12 << QSize();
        QTest::newRow("body24-natural") << 24 << QSize();
        QTest::newRow("body12-short") << 12 << QSize(246, 76);
        QTest::newRow("body24-short") << 24 << QSize(492, 152);
    }
    void downloadErrorReadable()
    {
        QFETCH(int, fontSize);
        QFETCH(QSize, bounds);
        QVERIFY(QDir().mkpath(temporary->filePath("config/omarchy")));
        QVERIFY(writeFile(temporary->filePath("config/omarchy/shell.toml"),
            QByteArray("[font]\nbase-size = ") + QByteArray::number(fontSize) + "\n"));
        openWindow();
        controls.onAcquire = [this] { emit browser->failed("Diagnostic acquisition already used; close this app."); };
        QVERIFY(click(window.get(), "Get a Cloud PC connection"));
        settle();
        auto* box = qobject_cast<QMessageBox*>(prompt(window.get(), "Connection download stopped"));
        QVERIFY(box);
        if (bounds.isValid()) { box->resize(bounds); settle(); }
        auto* text = box->findChild<QLabel*>("qt_msgbox_label");
        auto* ok = box->button(QMessageBox::Ok);
        QVERIFY(text && ok);
        if (!bounds.isValid()) {
            QVERIFY2(fullyVisible(text), "Natural fit unnecessarily clips text");
            QVERIFY2(fullyVisible(ok), "Natural fit unnecessarily clips OK");
        }
        QVERIFY(text->height() >= text->heightForWidth(text->width()));
        QVERIFY2(reveal(text), "Error text cannot be fully exposed by scrolling");
        QVERIFY2(reveal(ok), "Complete OK action cannot be exposed by scrolling");
        QVERIFY(click(box, "OK"));
        settle();
        QVERIFY(!prompt(window.get(), "Connection download stopped"));
        QVERIFY(controls.starts.isEmpty());
    }
    void acquisitionGuidanceInline_data()
    {
        QTest::addColumn<int>("fontSize");
        QTest::newRow("body12") << 12;
        QTest::newRow("body24") << 24;
    }
    void acquisitionGuidanceInline()
    {
        QFETCH(int, fontSize);
        QVERIFY(QDir().mkpath(temporary->filePath("config/omarchy")));
        QVERIFY(writeFile(temporary->filePath("config/omarchy/shell.toml"),
            QByteArray("[font]\nbase-size = ") + QByteArray::number(fontSize) + "\n"));
        openWindow();
        window->resize(fontSize * 21, fontSize * 10);
        settle();
        QVERIFY(click(window.get(), "Get a Cloud PC connection"));
        settle();
        QVERIFY(!prompt(window.get(), "OMAWIN365 · get a Cloud PC connection"));
        auto* panel = window->findChild<QWidget*>("acquisitionPanel");
        QVERIFY(panel && panel->isVisible() && !panel->isWindow());
        auto* status = accessible<QLabel>(panel, "Connection download status");
        QVERIFY(status);
        emit browser->status("Synthetic sign-in waiting");
        QCOMPARE(status->text(), "Synthetic sign-in waiting");
        auto* resources = accessible<QListWidget>(panel, "Available Microsoft Cloud PCs");
        QVERIFY(resources && !resources->isVisible());
        QVERIFY(!button(panel, "Download selected connection")->isVisible());
        auto* cancel = button(panel, "Cancel");
        QVERIFY(cancel && reveal(cancel));
        const int changedFont = fontSize == 12 ? 24 : 12;
        QVERIFY(writeFile(temporary->filePath("config/omarchy/shell.toml"),
            QByteArray("[font]\nbase-size = ") + QByteArray::number(changedFont) + "\n"));
        QTRY_COMPARE(window->font().pixelSize(), changedFont);
        settle();
        QCOMPARE(cancel->font().pixelSize(), changedFont);
        QVERIFY(reveal(cancel));
        QVERIFY(!prompt(window.get(), "OMAWIN365 · get a Cloud PC connection"));
        QVERIFY(click(panel, "Cancel"));
        settle();
        QCOMPARE(controls.cancellations, 1);
        QCOMPARE(controls.acquisitions, 1);
        QVERIFY(controls.starts.isEmpty());
    }
    void staleInlineCancelCannotCancelReplacement()
    {
        openWindow();
        QVERIFY(click(window.get(), "Get a Cloud PC connection"));
        auto* oldPanel = acquisitionPanel(window.get());
        QVERIFY(oldPanel);
        QPointer<QPushButton> oldCancel = button(oldPanel, "Cancel");
        QVERIFY(oldCancel);
        oldCancel->click();
        QCOMPARE(controls.cancellations, 1);
        button(window.get(), "Get a Cloud PC connection")->click();
        QCOMPARE(controls.acquisitions, 2);
        QVERIFY(oldCancel); // Before deferred deletion, model an old queued action.
        oldCancel->click();
        QCOMPARE(controls.cancellations, 1);
        settle();
        auto* replacement = acquisitionPanel(window.get());
        QVERIFY(replacement);
        QCOMPARE(QApplication::focusWidget(), button(replacement, "Cancel"));
        QVERIFY(click(replacement, "Cancel"));
        QCOMPARE(controls.cancellations, 2);
        QVERIFY(controls.starts.isEmpty());
    }
    void dialogLayoutTinyAndThemeReload()
    {
        QVERIFY(QDir().mkpath(temporary->filePath("config/omarchy")));
        QVERIFY(writeFile(temporary->filePath("config/omarchy/shell.toml"), "[font]\nbase-size = 12\n"));
        Theme theme;
        QMessageBox box(QMessageBox::Warning, "Connection download stopped",
            "Diagnostic acquisition already used; close this app.", QMessageBox::Ok);
        WindowPresentation::themeMessageBox(&box, &theme);
        box.show(); settle();
        auto* text = box.findChild<QLabel*>("qt_msgbox_label");
        auto* ok = box.button(QMessageBox::Ok);
        QVERIFY(text && ok);
        for (int fontSize : {12, 24}) {
            QVERIFY(QDir().mkpath(temporary->filePath("config/omarchy")));
            QVERIFY(writeFile(temporary->filePath("config/omarchy/shell.toml"),
                QByteArray("[font]\nbase-size = ") + QByteArray::number(fontSize) + "\n"));
            QTRY_COMPARE(theme.fontSize("body"), fontSize);
            settle();
            QCOMPARE(box.font().pixelSize(), fontSize);
            QVERIFY(box.width() <= box.screen()->availableGeometry().width());
            QVERIFY(box.height() <= box.screen()->availableGeometry().height());
            box.resize(fontSize * 21, fontSize * 7); settle();
            QVERIFY(reveal(text));
            QVERIFY(reveal(ok));
            // Less than one action row: every part must still be scroll-reachable.
            box.resize(fontSize * 21, qMax(1, ok->height() / 2)); settle();
            QRegion seen, textSeen;
            auto* scroll = box.findChild<QScrollArea*>();
            QVERIFY(scroll);
            auto* bar = scroll->verticalScrollBar();
            for (int offset = 0; offset <= bar->maximum(); ++offset) {
                bar->setValue(offset);
                QCoreApplication::processEvents();
                seen += ok->visibleRegion();
                textSeen += text->visibleRegion();
            }
            QVERIFY2(QRegion(ok->rect()).subtracted(seen).isEmpty(), "Tiny dialog has unreachable action pixels");
            QVERIFY2(QRegion(text->rect()).subtracted(textSeen).isEmpty(), "Tiny dialog has unreachable text pixels");
        }
    }
    void sharedDialogControlsReadable_data()
    {
        QTest::addColumn<int>("fontSize");
        QTest::addColumn<bool>("pin");
        QTest::newRow("confirmation12") << 12 << false;
        QTest::newRow("confirmation24") << 24 << false;
        QTest::newRow("pin12") << 12 << true;
        QTest::newRow("pin24") << 24 << true;
    }
    void sharedDialogControlsReadable()
    {
        QFETCH(int, fontSize);
        QFETCH(bool, pin);
        QVERIFY(QDir().mkpath(temporary->filePath("config/omarchy")));
        QVERIFY(writeFile(temporary->filePath("config/omarchy/shell.toml"),
            QByteArray("[font]\nbase-size = ") + QByteArray::number(fontSize) + "\n"));
        if (pin) {
            seed();
            openWindow();
            QVERIFY(click(window.get(), "Connect"));
            emit session->pinRequested("Synthetic key challenge");
            auto* box = prompt(window.get(), "OMAWIN365 · security key PIN");
            QVERIFY(box);
            auto* input = accessible<QLineEdit>(box, "Security key PIN");
            QVERIFY(input);
            QTRY_VERIFY(input->isEnabled());
            box->resize(fontSize * 21, fontSize * 7); settle();
            QCOMPARE(input->echoMode(), QLineEdit::Password);
            QVERIFY(reveal(input));
            QVERIFY(reveal(button(box, "Submit PIN")));
            QVERIFY(reveal(button(box, "Cancel and disconnect")));
            QVERIFY(click(box, "Cancel and disconnect"));
            QCOMPARE(controls.stops, 1);
            QVERIFY(controls.pins.isEmpty());
        } else {
            Theme theme;
            QMessageBox box(QMessageBox::Question, "Import trusted connection",
                "Only import a connection you trust.", QMessageBox::Cancel | QMessageBox::Ok);
            box.setDefaultButton(QMessageBox::Cancel);
            WindowPresentation::themeMessageBox(&box, &theme);
            box.open(); settle();
            box.resize(fontSize * 21, fontSize * 7); settle();
            QVERIFY(reveal(box.button(QMessageBox::Ok)));
            QVERIFY(reveal(box.button(QMessageBox::Cancel)));
            QCOMPARE(box.defaultButton(), box.button(QMessageBox::Cancel));
            QTest::keyClick(&box, Qt::Key_Return);
            QCOMPARE(box.result(), int(QMessageBox::Cancel));
        }
    }
    void initTestCase()
    {
        QCOMPARE(QGuiApplication::platformName(), "offscreen");
        QCOMPARE(QDir::homePath(), QString::fromUtf8(qgetenv("HOME")));
        QVERIFY(QDir::homePath().startsWith(QDir::tempPath() + '/'));
        QCOMPARE(QDir::currentPath(), QDir::homePath());
        QCOMPARE(qgetenv("DBUS_SESSION_BUS_ADDRESS"),
            QByteArray("unix:path=") + qgetenv("HOME") + "/no-session-bus");
        QVERIFY(!QFileInfo::exists(QDir::homePath() + "/no-session-bus"));
        // Auto must not consult the developer's live compositor.
        qunsetenv("HYPRLAND_INSTANCE_SIGNATURE");
    }
    void init()
    {
        controls = {};
        temporary = std::make_unique<QTemporaryDir>();
        QVERIFY(temporary->isValid());
        qputenv("XDG_DATA_HOME", temporary->filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", temporary->filePath("config").toUtf8());
        QVERIFY(QDir().mkpath(temporary->filePath("config")));
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary->filePath("config"));
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, temporary->filePath("system"));
        source = temporary->filePath("Synthetic Cloud PC.rdpw");
        QVERIFY(writeFile(source, profileBytes));
        session = std::make_unique<Session>();
        browser = std::make_unique<BrowserAuth>();
        store = std::make_unique<ProfileStore>();
        QVERIFY(store->profiles().isEmpty());
    }
    void cleanup()
    {
        window.reset();
        store.reset();
        browser.reset();
        session.reset();
        settle();
        temporary.reset();
    }
    void emptyStartupNeverConnects()
    {
        openWindow();
        QVERIFY(profiles());
        QCOMPARE(profiles()->count(), 0);
        QVERIFY(!button(window.get(), "Connect")->isEnabled());
        QCOMPARE(phase()->text(), "Ready");
        QVERIFY(controls.starts.isEmpty());
        QCOMPARE(controls.acquisitions, 0);
    }
    void savedSelectionNeverConnects()
    {
        seed("First");
        auto second = seed("Second");
        QSettings().setValue("selectedProfile", second.id);
        openWindow();
        QCOMPARE(profiles()->currentItem()->text(), "Second");
        QVERIFY(button(window.get(), "Connect")->isEnabled());
        profiles()->setCurrentRow(0);
        settle();
        QCOMPARE(QSettings().value("selectedProfile").toString(), store->profiles().first().id);
        QVERIFY(controls.starts.isEmpty());
    }
    void importConfirmation_data()
    {
        QTest::addColumn<bool>("accept");
        QTest::addColumn<int>("key");
        QTest::newRow("cancel") << false << 0;
        QTest::newRow("import") << true << 0;
        QTest::newRow("enter-default-cancel") << false << int(Qt::Key_Return);
        QTest::newRow("escape-cancel") << false << int(Qt::Key_Escape);
    }
    void importConfirmation()
    {
        QFETCH(bool, accept);
        QFETCH(int, key);
        openWindow();
        confirmation("Import trusted connection", accept ? QMessageBox::Ok : QMessageBox::Cancel, key);
        QCOMPARE(window->importProfile(source), accept);
        QCOMPARE(store->profiles().size(), accept ? 1 : 0);
        QCOMPARE(profiles()->count(), accept ? 1 : 0);
        QCOMPARE(readFile(source), profileBytes);
        if (accept) {
            QCOMPARE(profiles()->currentItem()->text(), "Synthetic Cloud PC");
            QCOMPARE(readFile(store->profiles().first().path), profileBytes);
            struct stat st {};
            QVERIFY(::stat(QFile::encodeName(store->profiles().first().path).constData(), &st) == 0);
            QCOMPARE(st.st_mode & 0777, mode_t(0600));
            QCOMPARE(phase()->text(), "Ready");
        }
        settle();
        QVERIFY(controls.starts.isEmpty());
    }
    void explicitConnectActions_data()
    {
        QTest::addColumn<QString>("action");
        QTest::newRow("button") << QString("button");
        QTest::newRow("shortcut") << QString("shortcut");
        QTest::newRow("list-activation") << QString("list");
    }
    void explicitConnectActions()
    {
        QFETCH(QString, action);
        seed("First");
        auto second = seed("Second");
        openWindow();
        QTest::mouseClick(profiles()->viewport(), Qt::LeftButton, Qt::NoModifier,
            profiles()->visualItemRect(profiles()->item(1)).center());
        QCOMPARE(profiles()->currentItem()->text(), "Second");
        QVERIFY(controls.starts.isEmpty());
        if (action == "button") QVERIFY(click(window.get(), "Connect"));
        else if (action == "shortcut") QTest::keyClick(window.get(), Qt::Key_Return, Qt::ControlModifier);
        else QTest::keyClick(profiles(), Qt::Key_Return);
        QCOMPARE(controls.starts, QStringList{second.path});
        // The connected Cloud PC is marked, visibly and for screen readers.
        QCOMPARE(profiles()->item(1)->text(), "● Second");
        QCOMPARE(profiles()->item(1)->data(Qt::AccessibleTextRole).toString(), "Second, connected");
        QCOMPARE(profiles()->item(0)->text(), "First");
        // The list stays usable during a session, but can never start a second one.
        QVERIFY(profiles()->isEnabled());
        QVERIFY(!button(window.get(), "Connect")->isVisible());
        QVERIFY(button(window.get(), "Disconnect")->isEnabled());
        QVERIFY(!button(window.get(), "Import .rdpw…")->isEnabled());
        QVERIFY(!button(window.get(), "Get a Cloud PC connection")->isEnabled());
        QVERIFY(!button(window.get(), "Remove…")->isEnabled()); // The connected profile.
        QTest::keyClick(window.get(), Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(controls.starts.size(), 1);
        QTest::mouseClick(profiles()->viewport(), Qt::LeftButton, Qt::NoModifier,
            profiles()->visualItemRect(profiles()->item(0)).center());
        QCOMPARE(profiles()->currentItem()->text(), "First");
        QVERIFY(button(window.get(), "Remove…")->isEnabled()); // Not the connected profile.
        QVERIFY(button(window.get(), "Rename…")->isEnabled());
        QTest::keyClick(profiles(), Qt::Key_Return);
        QTest::keyClick(window.get(), Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(controls.starts.size(), 1);
        emit session->connected();
        QCOMPARE(phase()->text(), "Connected");
        QVERIFY(click(window.get(), "Disconnect"));
        QCOMPARE(controls.stops, 1);
        QVERIFY(!button(window.get(), "Disconnect")->isEnabled());
        QVERIFY(!button(window.get(), "Reconnect")->isEnabled());
        QCOMPARE(controls.starts.size(), 1);
        finish();
        QCOMPARE(phase()->text(), "Ready");
        QVERIFY(button(window.get(), "Connect")->isEnabled());
        QVERIFY(profiles()->isEnabled());
        QCOMPARE(profiles()->item(1)->text(), "Second");
        settle();
        QCOMPARE(controls.starts.size(), 1);
    }
    void idlePromptNamesSelection()
    {
        seed("First");
        seed("Second");
        openWindow();
        const auto prompt = [this](const QString& text) {
            for (auto* label : window->findChildren<QLabel*>())
                if (label->isVisible() && label->text() == text) return true;
            return false;
        };
        QVERIFY(prompt(QStringLiteral("Press Connect or Enter to open %1.").arg(profiles()->currentItem()->text())));
        profiles()->setCurrentRow(1);
        QVERIFY(prompt(QStringLiteral("Press Connect or Enter to open Second.")));
    }
    void profileListShowsSeveralRows()
    {
        seed("First");
        seed("Second");
        openWindow();
        const auto rowsVisible = [this] {
            const int row = profiles()->sizeHintForRow(0);
            return row > 0 ? profiles()->viewport()->height() / row : 0;
        };
        QVERIFY(rowsVisible() >= 2);
        QVERIFY(!profiles()->verticalScrollBar()->isVisible());
        window.reset();
        for (int index = 3; index <= 10; ++index)
            seed(QStringLiteral("Cloud PC %1").arg(index));
        openWindow();
        QCOMPARE(profiles()->count(), 10);
        QVERIFY(rowsVisible() >= 6);
    }
    void reconnectWaitsForEnded_data()
    {
        QTest::addColumn<QString>("challenge");
        QTest::addColumn<bool>("synchronous");
        for (const char* name : {"none", "pin"}) {
            QTest::newRow(qPrintable(QString(name) + "-async")) << QString(name) << false;
            QTest::newRow(qPrintable(QString(name) + "-sync")) << QString(name) << true;
        }
    }
    void reconnectWaitsForEnded()
    {
        QFETCH(QString, challenge);
        QFETCH(bool, synchronous);
        auto saved = seed();
        openWindow();
        QVERIFY(click(window.get(), "Connect"));
        QPointer<QLineEdit> oldPin;
        if (challenge == "pin") {
            emit session->pinRequested("Synthetic key request");
            auto* keyPrompt = prompt(window.get(), "OMAWIN365 · security key PIN");
            QVERIFY(keyPrompt);
            oldPin = accessible<QLineEdit>(keyPrompt, "Security key PIN");
            QVERIFY(oldPin);
            QTRY_VERIFY(oldPin->isEnabled());
            QTest::keyClicks(oldPin, "2468");
            QCOMPARE(oldPin->text(), "2468");
        }
        settle();
        // A modal prompt prevents a physical launcher click. Dismissal behavior
        // is reached through the existing launcher button, not private methods.
        // QPushButton::click models a queued explicit action from the launcher.
        auto* reconnect = button(window.get(), "Reconnect");
        QVERIFY(reconnect && reconnect->isEnabled());
        if (synchronous) controls.onStop = [this] { controls.active = false; emit session->ended(); };
        if (oldPin) QCOMPARE(oldPin->text(), "2468");
        reconnect->click();
        // Observe the OLD widget before deferred deletion can conceal retention.
        QVERIFY(!oldPin || oldPin->text().isEmpty());
        QCOMPARE(controls.stops, 1);
        QVERIFY(controls.pins.isEmpty());
        QVERIFY(!prompt(window.get(), "OMAWIN365 · security key PIN"));
        QVERIFY(!prompt(window.get(), "OMAWIN365 · changed server identity"));
        if (!synchronous) {
            QCOMPARE(controls.starts.size(), 1);
            QVERIFY(!button(window.get(), "Reconnect")->isEnabled());
            finish();
        }
        settle();
        QCOMPARE(controls.starts, (QStringList{saved.path, saved.path}));
        QVERIFY(controls.active);
        QVERIFY(button(window.get(), "Disconnect")->isEnabled());
        finish();
        QCOMPARE(controls.starts.size(), 2);
    }
    void displayScaleRounding()
    {
        QCOMPARE(DisplayScale::fromFactor(1.0), 100);
        QCOMPARE(DisplayScale::fromFactor(1.25), 125);
        QCOMPARE(DisplayScale::fromFactor(1.5), 150);
        QCOMPARE(DisplayScale::fromFactor(1.6), 150);
        QCOMPARE(DisplayScale::fromFactor(1.625), 175); // Ties round up.
        QCOMPARE(DisplayScale::fromFactor(2.0), 200);
        QCOMPARE(DisplayScale::fromFactor(0.5), 100);
        QCOMPARE(DisplayScale::fromFactor(9.0), 500);
        QCOMPARE(DisplayScale::fromFactor(0), 100);
        QCOMPARE(DisplayScale::fromFactor(-2), 100);
        QCOMPARE(DisplayScale::fromFactor(std::numeric_limits<double>::quiet_NaN()), 100);
        QCOMPARE(DisplayScale::fromFactor(std::numeric_limits<double>::infinity()), 100);
        QCOMPARE(DisplayScale::normalizeChoice(150), 150);
        QCOMPARE(DisplayScale::normalizeChoice(QStringLiteral("175")), 175);
        QCOMPARE(DisplayScale::normalizeChoice(160), DisplayScale::Auto);
        QCOMPARE(DisplayScale::normalizeChoice(QStringLiteral("big")), DisplayScale::Auto);
        QCOMPARE(DisplayScale::normalizeChoice(QVariant()), DisplayScale::Auto);
    }
    void hyprlandFocusedScale()
    {
        using DisplayScale::focusedScaleFromJson;
        QCOMPARE(focusedScaleFromJson(R"([{"name":"DP-1","focused":false,"scale":1},{"name":"eDP-1","focused":true,"scale":1.6}])"),
            std::optional<double>(1.6));
        QVERIFY(!focusedScaleFromJson(R"([{"focused":false,"scale":2}])"));
        QVERIFY(!focusedScaleFromJson(R"([{"focused":true,"scale":2},{"focused":true,"scale":1}])"));
        QVERIFY(!focusedScaleFromJson(R"([{"focused":true,"scale":"2"}])"));
        QVERIFY(!focusedScaleFromJson(R"([{"focused":true,"scale":0}])"));
        QVERIFY(!focusedScaleFromJson(R"([{"focused":"true","scale":2}])"));
        QVERIFY(!focusedScaleFromJson(R"({"focused":true,"scale":2})"));
        QVERIFY(!focusedScaleFromJson("not json"));
        QVERIFY(!focusedScaleFromJson({}));
    }
    std::unique_ptr<FakeHyprland> fakeHyprland(const char* signature = "fixture_1")
    {
        const QString runtime = temporary->filePath("runtime");
        QDir().mkpath(runtime + "/hypr/fixture_1");
        qputenv("XDG_RUNTIME_DIR", runtime.toUtf8());
        qputenv("HYPRLAND_INSTANCE_SIGNATURE", signature);
        return std::make_unique<FakeHyprland>(runtime + "/hypr/fixture_1/.socket.sock");
    }
    void autoScaleFollowsHyprland()
    {
        const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR");
        auto restore = qScopeGuard([&] { qputenv("XDG_RUNTIME_DIR", runtime); qunsetenv("HYPRLAND_INSTANCE_SIGNATURE"); });
        auto hyprland = fakeHyprland();
        QVERIFY(hyprland->ok());
        hyprland->reply(R"([{"name":"DP-1","focused":false,"scale":1},{"name":"eDP-1","focused":true,"scale":1.6}])");
        seed();
        openWindow();
        auto* scale = accessible<QComboBox>(window.get(), "Remote desktop scale");
        QCOMPARE(scale->currentText(), "Auto (150%)");
        QVERIFY(!hyprland->requests().isEmpty());
        for (const auto& request : hyprland->requests()) QCOMPARE(request, QByteArray("j/monitors"));
        QVERIFY(click(window.get(), "Connect"));
        QCOMPARE(controls.options.last().desktopScale, 150);
        auto* pending = window->findChild<QLabel*>("optionsPending");
        QVERIFY(pending && !pending->isVisible());
        // The focused monitor changes mid-session: activating the launcher re-reads Auto,
        // and 200% is pending.
        hyprland->reply(R"([{"name":"DP-2","focused":true,"scale":2}])");
        window->activateWindow();
        QTRY_VERIFY(window->isActiveWindow());
        QEvent activated(QEvent::ActivationChange);
        QApplication::sendEvent(window.get(), &activated);
        QCOMPARE(scale->currentText(), "Auto (200%)");
        QVERIFY(pending->isVisible());
        QCOMPARE(controls.starts.size(), 1);
        QCOMPARE(controls.stops, 0);
        QVERIFY(click(window.get(), "Reconnect"));
        controls.active = false;
        emit session->ended();
        settle();
        QCOMPARE(controls.options.last().desktopScale, 200);
        QVERIFY(!pending->isVisible());
        // A fixed size never asks the compositor.
        scale->setCurrentIndex(scale->findData(125));
        const auto asked = hyprland->requests().size();
        QVERIFY(window->isActiveWindow());
        QEvent activation(QEvent::ActivationChange);
        QApplication::sendEvent(window.get(), &activation);
        settle();
        QCOMPARE(hyprland->requests().size(), asked);
    }
    void autoScaleRejectsBadHyprlandReplies_data()
    {
        QTest::addColumn<QByteArray>("reply");
        QTest::addColumn<bool>("silent");
        QTest::addColumn<QByteArray>("signature");
        QTest::newRow("malformed") << QByteArray("{not json") << false << QByteArray("fixture_1");
        QTest::newRow("oversized") << (QByteArray("[{\"focused\":true,\"scale\":2,\"pad\":\"") + QByteArray(300 * 1024, 'x') + "\"}]")
            << false << QByteArray("fixture_1");
        QTest::newRow("silent") << QByteArray() << true << QByteArray("fixture_1");
        QTest::newRow("unsafe-signature") << QByteArray(R"([{"focused":true,"scale":2}])") << false << QByteArray("../hypr/fixture_1");
    }
    void autoScaleRejectsBadHyprlandReplies()
    {
        QFETCH(QByteArray, reply);
        QFETCH(bool, silent);
        QFETCH(QByteArray, signature);
        const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR");
        auto restore = qScopeGuard([&] { qputenv("XDG_RUNTIME_DIR", runtime); qunsetenv("HYPRLAND_INSTANCE_SIGNATURE"); });
        auto hyprland = fakeHyprland(signature.constData());
        QVERIFY(hyprland->ok());
        hyprland->reply(reply, silent);
        QElapsedTimer elapsed;
        elapsed.start();
        openWindow();
        QVERIFY(elapsed.elapsed() < 2000); // Bounded: a silent compositor costs at most the query budget.
        // Falls back to the offscreen screen's 100%.
        QCOMPARE(accessible<QComboBox>(window.get(), "Remote desktop scale")->currentText(), "Auto (100%)");
        if (signature.contains('/')) QVERIFY(hyprland->requests().isEmpty());
    }
    void remoteOptionsPersistAndNeverConnect()
    {
        seed();
        openWindow();
        auto* scale = accessible<QComboBox>(window.get(), "Remote desktop scale");
        auto* clipboard = accessible<QCheckBox>(window.get(), "Share clipboard");
        QVERIFY(scale && clipboard);
        QCOMPARE(scale->currentData().toInt(), DisplayScale::Auto);
        QCOMPARE(scale->currentText(), "Auto (100%)"); // Offscreen screen, no compositor.
        QVERIFY(clipboard->isChecked());
        scale->setCurrentIndex(scale->findData(175));
        clipboard->setChecked(false);
        QVERIFY(controls.starts.isEmpty());
        QCOMPARE(QSettings().value("remoteDesktop/scale").toInt(), 175);
        QVERIFY(!store->profiles().first().devices.clipboard);
        window.reset();
        openWindow();
        scale = accessible<QComboBox>(window.get(), "Remote desktop scale");
        clipboard = accessible<QCheckBox>(window.get(), "Share clipboard");
        QCOMPARE(scale->currentData().toInt(), 175);
        QVERIFY(!clipboard->isChecked());
        QVERIFY(controls.starts.isEmpty());
        QVERIFY(click(window.get(), "Connect"));
        QCOMPARE(controls.options.size(), 1);
        QCOMPARE(controls.options.first().desktopScale, 175);
        QVERIFY(!controls.options.first().devices.clipboard);
    }
    void remoteOptionsInvalidSettingsFallBack()
    {
        QSettings().setValue("remoteDesktop/scale", 160);
        QSettings().setValue("remoteDesktop/clipboard", "false");
        seed();
        openWindow();
        QCOMPARE(accessible<QComboBox>(window.get(), "Remote desktop scale")->currentData().toInt(), DisplayScale::Auto);
        QVERIFY(accessible<QCheckBox>(window.get(), "Share clipboard")->isChecked()); // The old app-wide key is ignored.
        QVERIFY(click(window.get(), "Connect"));
        QCOMPARE(controls.options.first().desktopScale, 100);
    }
    void remoteOptionsApplyOnReconnect()
    {
        auto saved = seed();
        openWindow();
        auto* scale = accessible<QComboBox>(window.get(), "Remote desktop scale");
        auto* clipboard = accessible<QCheckBox>(window.get(), "Share clipboard");
        auto* pending = window->findChild<QLabel*>("optionsPending");
        QVERIFY(pending);
        scale->setCurrentIndex(scale->findData(150));
        QVERIFY(!pending->isVisible()); // No session: nothing is pending.
        QVERIFY(click(window.get(), "Connect"));
        QCOMPARE(controls.options.last().desktopScale, 150);
        QVERIFY(controls.options.last().devices.clipboard);
        QVERIFY(!pending->isVisible());
        // A change during a session never stops or starts one; it waits for Reconnect.
        scale->setCurrentIndex(scale->findData(200));
        clipboard->setChecked(false);
        QVERIFY(pending->isVisible());
        QCOMPARE(controls.stops, 0);
        QCOMPARE(controls.starts.size(), 1);
        scale->setCurrentIndex(scale->findData(150));
        clipboard->setChecked(true);
        QVERIFY(!pending->isVisible()); // Back to what the running session uses.
        scale->setCurrentIndex(scale->findData(200));
        QVERIFY(pending->isVisible());
        QVERIFY(click(window.get(), "Reconnect"));
        // Options are captured at the Reconnect press, not when the old session ends.
        scale->setCurrentIndex(scale->findData(250));
        controls.active = false;
        emit session->ended();
        settle();
        QCOMPARE(controls.starts, (QStringList{saved.path, saved.path}));
        QCOMPARE(controls.options.last().desktopScale, 200);
        QVERIFY(pending->isVisible()); // 250 is still pending for the new session.
    }
    void closeActiveSession_data()
    {
        QTest::addColumn<bool>("accept");
        QTest::newRow("keep-open") << false;
        QTest::newRow("disconnect-close") << true;
    }
    void closeActiveSession()
    {
        QFETCH(bool, accept);
        seed();
        openWindow();
        QVERIFY(click(window.get(), "Connect"));
        QVERIFY(!window->close());
        auto* closePrompt = prompt(window.get(), "Close OMAWIN365?");
        QVERIFY(closePrompt);
        QCOMPARE(button(closePrompt, "Keep open")->isDefault(), true);
        QVERIFY(!button(closePrompt, "Disconnect and close")->isDefault());
        QVERIFY(!window->close());
        QCOMPARE(window->findChildren<QDialog*>().size(), 1);
        QVERIFY(click(closePrompt, accept ? "Disconnect and close" : "Keep open"));
        settle();
        QCOMPARE(controls.stops, accept ? 1 : 0);
        QVERIFY(window->isVisible());
        if (accept) {
            QVERIFY(!button(window.get(), "Reconnect")->isEnabled());
            QTest::keyClick(window.get(), Qt::Key_Return, Qt::ControlModifier);
            QCOMPARE(controls.starts.size(), 1);
            finish();
            QTRY_VERIFY(!window->isVisible());
            QCOMPARE(controls.starts.size(), 1);
        } else {
            QVERIFY(button(window.get(), "Disconnect")->isEnabled());
            finish();
            QVERIFY(window->isVisible());
        }
    }
    void closeDuringAcquisitionCancelsWithoutConnecting()
    {
        openWindow();
        QVERIFY(click(window.get(), "Get a Cloud PC connection"));
        QVERIFY(window->close());
        QCOMPARE(controls.cancellations, 1);
        emit browser->profileDownloaded(source, "Late download after close");
        settle();
        QVERIFY(store->profiles().isEmpty());
        QVERIFY(controls.starts.isEmpty());
    }
    void pinEnterSendsOnceThenTouchDone_data()
    {
        QTest::addColumn<bool>("connected");
        QTest::addColumn<bool>("synchronousTouch");
        QTest::newRow("during-setup") << false << false;
        QTest::newRow("on-desktop") << true << false;
        QTest::newRow("real-synchronous-touch-status") << false << true;
    }
    void pinEnterSendsOnceThenTouchDone()
    {
        QFETCH(bool, connected);
        QFETCH(bool, synchronousTouch);
        seed();
        openWindow();
        QVERIFY(click(window.get(), "Connect"));
        if (connected) emit session->connected();
        emit session->pinRequested("Synthetic key challenge");
        auto* keyPrompt = prompt(window.get(), "OMAWIN365 · security key PIN");
        QVERIFY(keyPrompt);
        auto* input = accessible<QLineEdit>(keyPrompt, "Security key PIN");
        QVERIFY(input);
        QTRY_VERIFY(input->isEnabled());
        QCOMPARE(input->echoMode(), QLineEdit::Password);
        QVERIFY(input->inputMethodHints().testFlag(Qt::ImhSensitiveData));
        bool touchStatusEmitted = false;
        if (synchronousTouch) controls.onPin = [this, keyPrompt, &touchStatusEmitted] {
            // Real Session emits this inside submitPin, before it returns.
            emit session->statusChanged("awaiting-touch", "Synthetic PIN sent; touch your key");
            touchStatusEmitted = true;
            QCOMPARE(keyPrompt->windowTitle(), "OMAWIN365 · touch your security key");
            QVERIFY(button(keyPrompt, "Done") && button(keyPrompt, "Done")->isEnabled());
            QCOMPARE(controls.stops, 0);
        };
        settle();
        const QSize size = keyPrompt->size();
        QTest::keyClicks(input, "2468");
        QVERIFY(button(keyPrompt, "Submit PIN")->isEnabled());
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(touchStatusEmitted, synchronousTouch);
        QCOMPARE(controls.pins, QStringList{"2468"});
        QCOMPARE(input->text(), "");
        QVERIFY(!input->isVisible());
        settle();
        // The touch step reuses the PIN slot; the dialog keeps its size.
        QCOMPARE(keyPrompt->size(), size);
        QVERIFY(visibleLabelText(keyPrompt, "Sent once. Never retried automatically."));
        QVERIFY(reveal(button(keyPrompt, "Done")));
        QCOMPARE(keyPrompt->windowTitle(), "OMAWIN365 · touch your security key");
        QVERIFY(button(keyPrompt, "Done") && button(keyPrompt, "Done")->isEnabled());
        QCOMPARE(phase()->text(), "Touch your security key");
        emit session->statusChanged("connected", "Mapped desktop, not authentication proof");
        if (connected) emit session->connected();
        QCOMPARE(phase()->text(), "Touch your security key");
        QVERIFY(click(keyPrompt, "Done"));
        settle();
        QCOMPARE(controls.pins.size(), 1);
        QCOMPARE(controls.stops, 0);
        QCOMPARE(phase()->text(), connected ? "Connected" : "Connecting");
        finish();
        emit session->pinRequested("Late key challenge");
        settle();
        QVERIFY(!prompt(window.get(), "OMAWIN365 · security key PIN"));
    }
    void pinUserRejectionDisconnects_data()
    {
        QTest::addColumn<QString>("action");
        QTest::newRow("pin-escape") << QString("pin-escape");
        QTest::newRow("pin-dialog-close") << QString("pin-close");
        QTest::newRow("touch-escape") << QString("touch-escape");
    }
    void pinUserRejectionDisconnects()
    {
        QFETCH(QString, action);
        seed();
        openWindow();
        QVERIFY(click(window.get(), "Connect"));
        emit session->pinRequested("Synthetic user-cancel challenge");
        auto* keyPrompt = prompt(window.get(), "OMAWIN365 · security key PIN");
        QVERIFY(keyPrompt);
        QPointer<QLineEdit> input = accessible<QLineEdit>(keyPrompt, "Security key PIN");
        QVERIFY(input);
        QTRY_VERIFY(input->isEnabled());
        QTest::keyClicks(input, "2468");
        if (action == "touch-escape") {
            controls.onPin = [this] { emit session->statusChanged("awaiting-touch", "Synthetic touch request"); };
            QVERIFY(click(keyPrompt, "Submit PIN"));
            QCOMPARE(controls.pins, QStringList{"2468"});
            QCOMPARE(keyPrompt->windowTitle(), "OMAWIN365 · touch your security key");
        } else QVERIFY(controls.pins.isEmpty());
        const QStringList priorPins = controls.pins;
        if (action == "pin-close") QVERIFY(keyPrompt->close());
        else QTest::keyClick(action == "touch-escape" ? static_cast<QWidget*>(keyPrompt) : input.data(), Qt::Key_Escape);
        QCOMPARE(controls.stops, 1);
        QCOMPARE(controls.pins, priorPins);
        QVERIFY(!input || input->text().isEmpty());
        settle();
        QVERIFY(!prompt(window.get(), "OMAWIN365 · security key PIN"));
        QVERIFY(!prompt(window.get(), "OMAWIN365 · touch your security key"));
        finish();
        QCOMPARE(controls.stops, 1);
        QCOMPARE(controls.pins, priorPins);
        QCOMPARE(controls.starts.size(), 1);
    }
    void invalidPinCannotSubmit_data()
    {
        QTest::addColumn<QString>("value");
        QTest::newRow("empty") << QString("");
        QTest::newRow("short") << QString("123");
        QTest::newRow("control") << QString("12\n34");
        QTest::newRow("utf8-byte-limit") << QString(32, QChar(0x00e9));
        QTest::newRow("paragraph-separator") << QString("12\u202934");
    }
    void invalidPinCannotSubmit()
    {
        QFETCH(QString, value);
        seed();
        openWindow();
        QVERIFY(click(window.get(), "Connect"));
        emit session->pinRequested("Synthetic challenge");
        auto* keyPrompt = prompt(window.get(), "OMAWIN365 · security key PIN");
        QVERIFY(keyPrompt);
        auto* input = accessible<QLineEdit>(keyPrompt, "Security key PIN");
        QTRY_VERIFY(input->isEnabled());
        // setText intentionally exercises validation of complete Unicode inputs.
        input->setText(value);
        QVERIFY(!input->hasAcceptableInput());
        QVERIFY(!button(keyPrompt, "Submit PIN")->isEnabled());
        QTest::keyClick(input, Qt::Key_Return);
        QVERIFY(controls.pins.isEmpty());
        QVERIFY(click(keyPrompt, "Cancel and disconnect"));
        QCOMPARE(controls.stops, 1);
        finish();
        QCOMPARE(controls.starts.size(), 1);
    }
    void pinFocusLossClearsAndBlocksInput()
    {
        seed();
        openWindow();
        QVERIFY(click(window.get(), "Connect"));
        emit session->pinRequested("Synthetic challenge");
        auto* keyPrompt = prompt(window.get(), "OMAWIN365 · security key PIN");
        QVERIFY(keyPrompt);
        auto* input = accessible<QLineEdit>(keyPrompt, "Security key PIN");
        QTRY_VERIFY(input->isEnabled());
        settle();
        // Hyprland focus follows the mouse: no focus state may resize the dialog.
        const QSize size = keyPrompt->size();
        auto* focusButton = button(keyPrompt, "Click to enter PIN");
        QVERIFY(focusButton && !focusButton->isVisible());
        QTest::keyClicks(input, "2468");
        QVERIFY(button(keyPrompt, "Submit PIN")->isEnabled());
        // Offscreen Qt activation only: no compositor/display proof is claimed.
        window->activateWindow();
        QTRY_VERIFY(!keyPrompt->isActiveWindow());
        QTRY_VERIFY(!input->isEnabled());
        QCOMPARE(input->text(), "");
        settle();
        QVERIFY(focusButton->isVisible());
        QVERIFY(!input->isVisible());
        QCOMPARE(keyPrompt->size(), size);
        QVERIFY(!button(keyPrompt, "Submit PIN")->isEnabled());
        QTest::keyClicks(input, "1357");
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(input->text(), "");
        QVERIFY(controls.pins.isEmpty());
        keyPrompt->activateWindow();
        QTRY_VERIFY(input->isEnabled());
        settle();
        QVERIFY(input->isVisible() && !focusButton->isVisible());
        QCOMPARE(keyPrompt->size(), size);
        QVERIFY(!button(keyPrompt, "Submit PIN")->isEnabled());
        QVERIFY(click(keyPrompt, "Cancel and disconnect"));
        QCOMPARE(controls.stops, 1);
    }
    void programmaticPinDismissalDoesNotCancelAttempt_data()
    {
        QTest::addColumn<QString>("reason");
        QTest::newRow("replacement") << QString("replacement");
        QTest::newRow("error") << QString("error");
        QTest::newRow("ended") << QString("ended");
    }
    void programmaticPinDismissalDoesNotCancelAttempt()
    {
        QFETCH(QString, reason);
        seed();
        openWindow();
        QVERIFY(click(window.get(), "Connect"));
        emit session->pinRequested("First synthetic request");
        auto* keyPrompt = prompt(window.get(), "OMAWIN365 · security key PIN");
        QVERIFY(keyPrompt);
        QPointer<QDialog> oldPrompt = keyPrompt;
        QPointer<QLineEdit> oldPin = accessible<QLineEdit>(keyPrompt, "Security key PIN");
        QVERIFY(oldPin);
        QTRY_VERIFY(oldPin->isEnabled());
        QTest::keyClicks(oldPin, "2468");
        QCOMPARE(oldPin->text(), "2468");
        if (reason == "replacement") emit session->pinRequested("Second synthetic request");
        else if (reason == "error") emit session->error("Synthetic prompt failure");
        else { controls.active = false; emit session->ended(); }
        QVERIFY(!oldPin || oldPin->text().isEmpty());
        settle();
        QVERIFY(oldPrompt.isNull());
        QCOMPARE(controls.stops, 0);
        QVERIFY(controls.pins.isEmpty());
        if (reason == "replacement") {
            auto* replacement = prompt(window.get(), "OMAWIN365 · security key PIN");
            QVERIFY(replacement);
            QVERIFY(click(replacement, "Cancel and disconnect"));
            QCOMPARE(controls.stops, 1);
            finish();
        } else if (reason == "error") {
            auto* error = prompt(window.get(), "Connection stopped");
            QVERIFY(error);
            QVERIFY(click(error, "OK"));
            finish();
        }
        QCOMPARE(controls.starts.size(), 1);
    }
    void synchronousPinCompletionNeverShowsTouch_data()
    {
        QTest::addColumn<bool>("error");
        QTest::newRow("ended") << false;
        QTest::newRow("error") << true;
    }
    void synchronousPinCompletionNeverShowsTouch()
    {
        QFETCH(bool, error);
        seed();
        openWindow();
        QVERIFY(click(window.get(), "Connect"));
        emit session->pinRequested("Synthetic request");
        auto* keyPrompt = prompt(window.get(), "OMAWIN365 · security key PIN");
        QVERIFY(keyPrompt);
        auto* input = accessible<QLineEdit>(keyPrompt, "Security key PIN");
        QTRY_VERIFY(input->isEnabled());
        controls.onPin = [this, error] {
            if (error) emit session->error("Synthetic submit failure");
            else { controls.active = false; emit session->ended(); }
        };
        QTest::keyClicks(input, "2468");
        QVERIFY(click(keyPrompt, "Submit PIN"));
        settle();
        QCOMPARE(controls.pins, QStringList{"2468"});
        QVERIFY(!prompt(window.get(), "OMAWIN365 · touch your security key"));
        QVERIFY(!prompt(window.get(), "OMAWIN365 · security key PIN"));
        QCOMPARE(phase()->text(), error ? "Connection stopped" : "Ready");
        QCOMPARE(controls.stops, 0);
    }
    void certificateExplanationOnly_data()
    {
        QTest::addColumn<QString>("action");
        for (const char* action : {"ok", "return", "keypad-enter", "escape", "close", "child-return"})
            QTest::newRow(action) << QString::fromLatin1(action);
    }
    void certificateExplanationOnly()
    {
        QFETCH(QString, action);
        seed();
        openWindow();
        QVERIFY(click(window.get(), "Connect"));
        emit session->statusChanged("error", certificateStoppedMessage());
        emit session->error(certificateStoppedMessage());
        auto* box = qobject_cast<QMessageBox*>(prompt(window.get(), "Connection stopped"));
        QVERIFY(box);
        QCOMPARE(box->text(), certificateStoppedMessage());
        QCOMPARE(box->textFormat(), Qt::PlainText);
        // Shared text-led theme intentionally removes platform warning icons.
        // The warning contract is title/text/OK-only, not a bitmap assertion.
        QCOMPARE(box->standardButtons(), QMessageBox::StandardButtons(QMessageBox::Ok));
        QVERIFY(!button(window.get(), "Continue with verified identity"));
        QVERIFY(!accessible<QLineEdit>(box, "Verified server"));
        if (action == "ok") QVERIFY(click(box, "OK"));
        else if (action == "close") QVERIFY(box->close());
        else if (action == "child-return") {
            auto* child = box->button(QMessageBox::Ok);
            child->setFocus();
            QTest::keyClick(child, Qt::Key_Return);
        } else QTest::keyClick(box, action == "escape" ? Qt::Key_Escape :
            action == "return" ? Qt::Key_Return : Qt::Key_Enter);
        settle();
        QVERIFY(!prompt(window.get(), "Connection stopped"));
        // Dismissal neither approves, retries, nor claims teardown completion.
        QVERIFY(controls.active);
        QCOMPARE(controls.starts.size(), 1);
        QCOMPARE(controls.stops, 0);
        QVERIFY(controls.pins.isEmpty());
        QVERIFY(controls.callbacks.isEmpty());
        QCOMPARE(phase()->text(), "Connection stopped");
        finish();
        QCOMPARE(phase()->text(), "Connection stopped");
        QCOMPARE(controls.starts.size(), 1);
        QVERIFY(click(window.get(), "Connect"));
        QCOMPARE(controls.starts.size(), 2);
    }
    void certificateTransitionUsesNeutralProgress()
    {
        seed();
        openWindow();
        const QString note = QStringLiteral("Connection setup may ask you to sign in to Microsoft more than once. Follow each prompt; you do not need to press Connect again.");
        QVERIFY(click(window.get(), "Connect"));
        QVERIFY(visibleLabelText(window.get(), note));
        emit session->statusChanged("signing-in", "Synthetic sign-in");
        QVERIFY(visibleLabelText(window.get(), note));
        emit session->pinRequested("Synthetic request");
        QVERIFY(!visibleLabelText(window.get(), note));
        emit session->statusChanged("awaiting-touch", "Synthetic touch");
        QVERIFY(!visibleLabelText(window.get(), note));
        emit session->statusChanged("error", certificateStoppedMessage());
        emit session->error(certificateStoppedMessage());
        QVERIFY(!visibleLabelText(window.get(), note));
        auto* box = prompt(window.get(), "Connection stopped");
        QVERIFY(box);
        QVERIFY(click(box, "OK"));
        finish();
        QVERIFY(!visibleLabelText(window.get(), note));
        QCOMPARE(phase()->text(), "Connection stopped");
    }
    void certificateErrorClearsSensitiveUiAndQueuedReconnect_data()
    {
        QTest::addColumn<QString>("pending");
        for (const char* pending : {"pin", "browser", "reconnect"})
            QTest::newRow(pending) << QString::fromLatin1(pending);
    }
    void certificateErrorClearsSensitiveUiAndQueuedReconnect()
    {
        QFETCH(QString, pending);
        seed();
        openWindow();
        QVERIFY(click(window.get(), "Connect"));
        QPointer<QLineEdit> oldPin;
        if (pending == "pin") {
            emit session->pinRequested("Synthetic request");
            auto* keyPrompt = prompt(window.get(), "OMAWIN365 · security key PIN");
            QVERIFY(keyPrompt);
            oldPin = accessible<QLineEdit>(keyPrompt, "Security key PIN");
            QVERIFY(oldPin);
            QTRY_VERIFY(oldPin->isEnabled());
            QTest::keyClicks(oldPin, "2468");
        } else if (pending == "browser") {
            emit session->authRequested(QUrl("https://login.microsoftonline.com/fixture/oauth2/authorize"));
            QCOMPARE(controls.authorizations.size(), 1);
        } else QVERIFY(click(window.get(), "Reconnect"));
        const int cancellations = controls.cancellations;
        const int stops = controls.stops;
        emit session->statusChanged("error", certificateStoppedMessage());
        emit session->error(certificateStoppedMessage());
        QVERIFY(!oldPin || oldPin->text().isEmpty());
        QVERIFY(!prompt(window.get(), "OMAWIN365 · security key PIN"));
        QVERIFY(controls.cancellations > cancellations);
        emit browser->callbackReady(QUrl("https://login.microsoftonline.com/common/oauth2/nativeclient?code=late"));
        QVERIFY(controls.callbacks.isEmpty());
        auto* box = prompt(window.get(), "Connection stopped");
        QVERIFY(box);
        QVERIFY(click(box, "OK"));
        emit session->statusChanged("disconnecting", "Late cleanup status");
        finish();
        QCOMPARE(phase()->text(), "Connection stopped");
        QCOMPARE(controls.starts.size(), 1);
        QCOMPARE(controls.stops, stops);
        QVERIFY(controls.pins.isEmpty());
        QVERIFY(click(window.get(), "Connect"));
        QCOMPARE(controls.starts.size(), 2);
    }
    void authorizationIsGatedByActiveAttemptAndStopping()
    {
        const QUrl authorization("https://login.microsoftonline.com/fixture/oauth2/authorize");
        const QUrl callback("https://login.microsoftonline.com/common/oauth2/nativeclient?code=synthetic");
        seed();
        openWindow();
        emit session->authRequested(authorization);
        emit browser->callbackReady(callback);
        QVERIFY(controls.authorizations.isEmpty());
        QVERIFY(controls.callbacks.isEmpty());
        QVERIFY(click(window.get(), "Connect"));
        emit session->statusChanged("signing-in", "Waiting for sign-in");
        emit session->authRequested(authorization);
        emit browser->callbackReady(callback);
        QCOMPARE(controls.authorizations, QList<QUrl>{authorization});
        QCOMPARE(controls.callbacks, QList<QUrl>{callback});
        emit browser->status("Synthetic browser status");
        QCOMPARE(phase()->text(), "Microsoft sign-in");
        QVERIFY(click(window.get(), "Disconnect"));
        emit session->authRequested(authorization);
        emit browser->callbackReady(callback);
        emit session->pinRequested("Late request");
        QCOMPARE(controls.authorizations.size(), 1);
        QCOMPARE(controls.callbacks.size(), 1);
        QVERIFY(!prompt(window.get(), "OMAWIN365 · security key PIN"));
        QVERIFY(!prompt(window.get(), "OMAWIN365 · changed server identity"));
        finish();
        emit browser->callbackReady(callback);
        QCOMPARE(controls.callbacks.size(), 1);
    }
    void errorsPreserveStoppedStatusAndCancelQueuedRestart_data()
    {
        QTest::addColumn<bool>("browserError");
        QTest::newRow("session-error") << false;
        QTest::newRow("browser-error") << true;
    }
    void errorsPreserveStoppedStatusAndCancelQueuedRestart()
    {
        QFETCH(bool, browserError);
        seed();
        openWindow();
        QVERIFY(click(window.get(), "Connect"));
        if (browserError) emit browser->failed("Synthetic sign-in failure");
        else {
            QVERIFY(click(window.get(), "Reconnect"));
            emit session->error("Synthetic transport failure");
        }
        auto* error = qobject_cast<QMessageBox*>(prompt(window.get(), browserError ? "Sign-in stopped" : "Connection stopped"));
        QVERIFY(error);
        QCOMPARE(error->text(), browserError ? "Synthetic sign-in failure" : "Synthetic transport failure");
        QVERIFY(click(error, "OK"));
        emit session->statusChanged("disconnecting", "Late cleanup status");
        finish();
        QCOMPARE(phase()->text(), "Connection stopped");
        QCOMPARE(controls.starts.size(), 1);
        QVERIFY(click(window.get(), "Connect"));
        QCOMPARE(controls.starts.size(), 2);
        QCOMPARE(phase()->text(), "Connecting");
    }
    void connectedDesktopClosesSignInWindow()
    {
        seed();
        openWindow();
        QVERIFY(click(window.get(), "Connect"));
        emit session->authRequested(QUrl("https://login.microsoftonline.com/fixture/oauth2/authorize"));
        const int before = controls.cancellations;
        emit session->connected();
        // The sign-in window's job is done once the desktop is up.
        QCOMPARE(controls.cancellations, before + 1);
        QCOMPARE(phase()->text(), "Connected");
    }
    void closingSignInWindowCancelsCalmly()
    {
        seed();
        openWindow();
        QVERIFY(click(window.get(), "Connect"));
        emit session->authRequested(QUrl("https://login.microsoftonline.com/fixture/oauth2/authorize"));
        emit browser->closed();
        QCOMPARE(controls.stops, 1); // The pending connection is stopped for the user.
        QVERIFY(!prompt(window.get(), "Sign-in stopped"));
        finish();
        QCOMPARE(phase()->text(), "Ready");
        bool explained = false;
        for (auto* label : window->findChildren<QLabel*>())
            explained = explained || label->text()
                == "Sign-in window closed, so the connection was cancelled. Press Connect to try again.";
        QVERIFY(explained);
        QVERIFY(button(window.get(), "Connect")->isEnabled());
    }
    void closingAcquisitionWindowCancelsCalmly()
    {
        openWindow();
        controls.onAcquire = [this] { emit browser->closed(); };
        QVERIFY(click(window.get(), "Get a Cloud PC connection"));
        settle();
        QVERIFY(!prompt(window.get(), "Connection download stopped"));
        QCOMPARE(phase()->text(), "Ready");
        QVERIFY(button(window.get(), "Get a Cloud PC connection")->isEnabled());
    }
    void acquisitionRequiresExplicitChoiceAndRefreshClearsSelection()
    {
        openWindow();
        QVERIFY(click(window.get(), "Get a Cloud PC connection"));
        QCOMPARE(controls.acquisitions, 1);
        auto* guide = acquisitionPanel(window.get());
        QVERIFY(guide);
        auto* resources = accessible<QListWidget>(guide, "Available Microsoft Cloud PCs");
        auto* download = button(guide, "Download selected connection");
        QVERIFY(resources && download);
        QVERIFY(!download->isVisible());
        QVERIFY(!button(window.get(), "Import .rdpw…")->isEnabled());
        emit browser->resourcesAvailable({"pc-a", "pc-b"}, {"Cloud A", "<Cloud B>"});
        settle();
        QCOMPARE(resources->count(), 2);
        QVERIFY(resources->selectedItems().isEmpty());
        QVERIFY(!download->autoDefault());
        QVERIFY(!download->isEnabled());
        QTest::keyClick(resources, Qt::Key_Return);
        QVERIFY(controls.selections.isEmpty());
        QTest::mouseClick(resources->viewport(), Qt::LeftButton, Qt::NoModifier,
            resources->visualItemRect(resources->item(1)).center());
        QVERIFY(download->isEnabled());
        QVERIFY(click(guide, "Download selected connection"));
        QCOMPARE(controls.selections, QStringList{"pc-b"});
        QVERIFY(!download->isEnabled());
        QVERIFY(!resources->isEnabled());
        emit browser->resourcesAvailable({"pc-c"}, {"Cloud C"});
        settle();
        QVERIFY(resources->isEnabled());
        QVERIFY(resources->selectedItems().isEmpty());
        QVERIFY(!download->isEnabled());
        QCOMPARE(controls.selections.size(), 1);
        // Mismatched payload is ignored, preserving the last valid chooser.
        emit browser->resourcesAvailable({"bad", "other"}, {"Only one"});
        QCOMPARE(resources->count(), 1);
        QCOMPARE(resources->item(0)->text(), "Cloud C");
        emit browser->resourcesAvailable({}, {});
        QVERIFY(!download->isVisible());
        QVERIFY(click(guide, "Cancel"));
        settle();
        QCOMPARE(controls.cancellations, 1);
        QCOMPARE(phase()->text(), "Ready");
        emit browser->resourcesAvailable({"stale"}, {"Stale"});
        emit browser->profileDownloaded(source, "Late download");
        emit browser->failed("Late failure");
        settle();
        QVERIFY(store->profiles().isEmpty());
        QVERIFY(!prompt(window.get(), "Connection download stopped"));
        QVERIFY(controls.starts.isEmpty());
    }
    void acquisitionBrowserStatusForwardsOnlyWhileOwned()
    {
        openWindow();
        controls.onAcquire = [this] { emit browser->status("Synthetic browser opening portal"); };
        QVERIFY(click(window.get(), "Get a Cloud PC connection"));
        auto* guide = acquisitionPanel(window.get());
        QVERIFY(guide);
        QPointer<QLabel> downloadStatus = accessible<QLabel>(guide, "Connection download status");
        auto* panel = window->findChild<QWidget*>("statusPanel");
        QVERIFY(downloadStatus && panel);
        QCOMPARE(downloadStatus->text(), "Synthetic browser opening portal");
        QVERIFY(visibleLabelText(panel, "Synthetic browser opening portal"));
        emit browser->status("Synthetic browser finding Cloud PCs");
        QCOMPARE(phase()->text(), "Getting Cloud PC connection");
        QCOMPARE(downloadStatus->text(), "Synthetic browser finding Cloud PCs");
        QVERIFY(visibleLabelText(panel, "Synthetic browser finding Cloud PCs"));
        bool cancelStatusEmitted = false;
        controls.onCancel = [this, panel, downloadStatus, &cancelStatusEmitted] {
            // Real BrowserAuth::cancel can emit status inside guide completion.
            emit browser->status("Synthetic browser cancelled");
            cancelStatusEmitted = true;
            QVERIFY(visibleLabelText(panel, "Synthetic browser finding Cloud PCs"));
            QVERIFY(!visibleLabelText(panel, "Synthetic browser cancelled"));
            QVERIFY(!downloadStatus || downloadStatus->text() == "Synthetic browser finding Cloud PCs");
        };
        QVERIFY(reveal(button(guide, "Cancel")));
        QVERIFY(click(guide, "Cancel"));
        QVERIFY(cancelStatusEmitted);
        QCOMPARE(controls.cancellations, 1);
        emit browser->status("Late browser status after cancellation");
        QCOMPARE(phase()->text(), "Ready");
        QVERIFY(visibleLabelText(panel, "Connection download cancelled. You can import a trusted .rdpw file instead."));
        QVERIFY(!visibleLabelText(panel, "Late browser status after cancellation"));
        QVERIFY(store->profiles().isEmpty());
        QVERIFY(controls.starts.isEmpty());
        controls.onCancel = {}; // No callback may outlive this row's references.
    }
    void acquisitionImportsBorrowedFileSynchronously()
    {
        openWindow();
        QVERIFY(click(window.get(), "Get a Cloud PC connection"));
        emit browser->profileDownloaded(source, "Downloaded Cloud PC");
        // No event-loop turn: BrowserAuth lends the file only during this signal.
        QCOMPARE(store->profiles().size(), 1);
        QCOMPARE(store->profiles().first().name, "Downloaded Cloud PC");
        QVERIFY(writeFile(source, "Borrowed source no longer valid"));
        QCOMPARE(readFile(store->profiles().first().path), profileBytes);
        settle();
        QVERIFY(!acquisitionPanel(window.get()));
        QCOMPARE(controls.cancellations, 0);
        QVERIFY(button(window.get(), "Connect")->isEnabled());
        QCOMPARE(profiles()->currentItem()->text(), "Downloaded Cloud PC");
        emit browser->profileDownloaded(source, "Duplicate");
        QCOMPARE(store->profiles().size(), 1);
        QVERIFY(controls.starts.isEmpty());
    }
    void acquisitionFailure_data()
    {
        QTest::addColumn<bool>("synchronous");
        QTest::newRow("async") << false;
        QTest::newRow("during-begin") << true;
    }
    void acquisitionFailure()
    {
        QFETCH(bool, synchronous);
        openWindow();
        if (synchronous) controls.onAcquire = [this] { emit browser->failed("Synthetic acquisition failure"); };
        QVERIFY(click(window.get(), "Get a Cloud PC connection"));
        if (!synchronous) emit browser->failed("Synthetic acquisition failure");
        settle();
        QCOMPARE(phase()->text(), "Connection download stopped");
        QVERIFY(!acquisitionPanel(window.get()));
        auto* error = qobject_cast<QMessageBox*>(prompt(window.get(), "Connection download stopped"));
        QVERIFY(error);
        QCOMPARE(error->text(), "Synthetic acquisition failure");
        QVERIFY(click(error, "OK"));
        QCOMPARE(controls.cancellations, 0);
        QVERIFY(store->profiles().isEmpty());
        QVERIFY(controls.starts.isEmpty());
    }
    void acquisitionIgnoresDownloadWhileTransportActive()
    {
        openWindow();
        QVERIFY(click(window.get(), "Get a Cloud PC connection"));
        controls.active = true; // External activity is not import permission.
        emit browser->profileDownloaded(source, "Ignored");
        QVERIFY(store->profiles().isEmpty());
        auto* guide = acquisitionPanel(window.get());
        QVERIFY(guide);
        QVERIFY(click(guide, "Cancel"));
        controls.active = false;
        QVERIFY(controls.starts.isEmpty());
    }
    void acquisitionManualAlternativeOpensChooserAfterGuideCloses()
    {
        openWindow();
        QVERIFY(click(window.get(), "Get a Cloud PC connection"));
        auto* guide = acquisitionPanel(window.get());
        QVERIFY(guide);
        QTimer::singleShot(0, window.get(), [this] {
            // Window queues opening the chooser first; this timer follows it.
            QTimer::singleShot(0, window.get(), [this] {
                auto* chooser = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
                QVERIFY(chooser);
                QCOMPARE(chooser->windowTitle(), "Import Cloud PC connection");
                QVERIFY(!acquisitionPanel(window.get()));
                QVERIFY(click(chooser, "Cancel"));
            });
        });
        QTimer::singleShot(1000, window.get(), [] {
            if (auto* chooser = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
                QTest::qFail("Chooser did not finish", __FILE__, __LINE__);
                chooser->reject();
            }
        });
        QVERIFY(click(guide, "Import a trusted .rdpw instead…"));
        settle();
        QCOMPARE(controls.cancellations, 1);
        QVERIFY(store->profiles().isEmpty());
        QVERIFY(controls.starts.isEmpty());
    }
    void manualImportChooserStillRequiresConfirmation()
    {
        openWindow();
        QTimer::singleShot(0, window.get(), [this] {
            auto* chooser = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
            QVERIFY(chooser);
            QCOMPARE(chooser->windowTitle(), "Import Cloud PC connection");
            auto* fileName = chooser->findChild<QLineEdit*>("fileNameEdit");
            QVERIFY(fileName);
            fileName->setText(source);
            confirmation("Import trusted connection", QMessageBox::Ok);
            QVERIFY(click(chooser, "Open"));
        });
        QTimer::singleShot(1000, window.get(), [] {
            if (auto* chooser = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
                QTest::qFail("Import chooser did not finish", __FILE__, __LINE__);
                chooser->reject();
            }
        });
        QVERIFY(click(window.get(), "Import .rdpw…"));
        settle();
        QCOMPARE(store->profiles().size(), 1);
        QCOMPARE(profiles()->currentItem()->text(), "Synthetic Cloud PC");
        QVERIFY(controls.starts.isEmpty());
    }
    void staleManualAlternativeCannotOpenChooserAfterReplacementFailure()
    {
        openWindow();
        QVERIFY(click(window.get(), "Get a Cloud PC connection"));
        auto* guide = acquisitionPanel(window.get());
        QVERIFY(guide);
        // During cancellation's synchronous outward callback, start a
        // replacement which fails. Only the generation fence must suppress
        // the old manual action's queued chooser.
        controls.onCancel = [this] {
            controls.onAcquire = [this] { emit browser->failed("Synthetic replacement failure"); };
            button(window.get(), "Get a Cloud PC connection")->click();
        };
        QTimer::singleShot(300, window.get(), [] {
            if (auto* chooser = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
                QTest::qFail("Stale manual action opened a chooser", __FILE__, __LINE__);
                chooser->reject();
            }
        });
        QVERIFY(click(guide, "Import a trusted .rdpw instead…"));
        auto* error = prompt(window.get(), "Connection download stopped");
        QVERIFY(error);
        QVERIFY(click(error, "OK"));
        settle();
        QCOMPARE(controls.acquisitions, 2);
        QCOMPARE(controls.cancellations, 1);
        QVERIFY(!qobject_cast<QFileDialog*>(QApplication::activeModalWidget()));
        QVERIFY(store->profiles().isEmpty());
        QVERIFY(controls.starts.isEmpty());
    }
    void renamePreservesConnectionAndSelection_data()
    {
        QTest::addColumn<bool>("accept");
        QTest::newRow("cancel") << false;
        QTest::newRow("save") << true;
    }
    void renamePreservesConnectionAndSelection()
    {
        QFETCH(bool, accept);
        auto original = seed("Original label");
        openWindow();
        QTest::keyClick(window.get(), Qt::Key_F2);
        auto* rename = prompt(window.get(), "Rename Cloud PC label");
        QVERIFY(rename);
        auto* input = accessible<QLineEdit>(rename, "Cloud PC label");
        QVERIFY(input);
        QCOMPARE(input->selectedText(), "Original label");
        QTest::keyClicks(input, "New label");
        QVERIFY(click(rename, accept ? "Save" : "Cancel"));
        settle();
        QCOMPARE(store->profiles().first().id, original.id);
        QCOMPARE(store->profiles().first().path, original.path);
        QCOMPARE(readFile(original.path), profileBytes);
        QCOMPARE(profiles()->currentItem()->text(), accept ? "New label" : "Original label");
        QVERIFY(controls.starts.isEmpty());
        // Later Connect still names the same connection, not a new identity.
        QVERIFY(click(window.get(), "Connect"));
        QCOMPARE(controls.starts, QStringList{original.path});
    }
    void invalidRenameCannotSave_data()
    {
        QTest::addColumn<QString>("value");
        QTest::newRow("empty") << QString("");
        QTest::newRow("spaces") << QString("   ");
        QTest::newRow("control") << QString("New\tlabel");
        QTest::newRow("line-separator") << QString("New\u2028label");
    }
    void invalidRenameCannotSave()
    {
        QFETCH(QString, value);
        seed("Original label");
        openWindow();
        QVERIFY(click(window.get(), "Rename…"));
        auto* rename = prompt(window.get(), "Rename Cloud PC label");
        QVERIFY(rename);
        auto* input = accessible<QLineEdit>(rename, "Cloud PC label");
        QVERIFY(input);
        QCOMPARE(input->maxLength(), 160);
        input->setText(value);
        QVERIFY(!input->hasAcceptableInput());
        QVERIFY(!button(rename, "Save")->isEnabled());
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(store->profiles().first().name, "Original label");
        QVERIFY(click(rename, "Cancel"));
        QVERIFY(controls.starts.isEmpty());
    }
    void removeConfirmationOnlyRemovesPrivateCopy_data()
    {
        QTest::addColumn<bool>("accept");
        QTest::addColumn<int>("key");
        QTest::newRow("cancel") << false << 0;
        QTest::newRow("remove") << true << 0;
        QTest::newRow("enter-default-cancel") << false << int(Qt::Key_Return);
        QTest::newRow("escape-cancel") << false << int(Qt::Key_Escape);
    }
    void removeConfirmationOnlyRemovesPrivateCopy()
    {
        QFETCH(bool, accept);
        QFETCH(int, key);
        seed();
        openWindow();
        confirmation("Remove connection", accept ? QMessageBox::Yes : QMessageBox::Cancel, key);
        QVERIFY(click(window.get(), "Remove…"));
        settle();
        QCOMPARE(store->profiles().size(), accept ? 0 : 1);
        QCOMPARE(profiles()->count(), accept ? 0 : 1);
        QCOMPARE(readFile(source), profileBytes);
        QVERIFY(controls.starts.isEmpty());
        QCOMPARE(button(window.get(), "Connect")->isEnabled(), !accept);
    }
    void activeRenameIsDismissedByAuthenticationPrompts_data()
    {
        QTest::addColumn<bool>("terminalError");
        QTest::newRow("pin") << false;
        QTest::newRow("certificate-error") << true;
    }
    void activeRenameIsDismissedByAuthenticationPrompts()
    {
        QFETCH(bool, terminalError);
        seed("Original label");
        openWindow();
        QVERIFY(click(window.get(), "Connect"));
        QVERIFY(click(window.get(), "Rename…"));
        auto* rename = prompt(window.get(), "Rename Cloud PC label");
        QVERIFY(rename);
        accessible<QLineEdit>(rename, "Cloud PC label")->setText("Unsaved label");
        if (terminalError)
            emit session->error(certificateStoppedMessage());
        else emit session->pinRequested("Synthetic challenge");
        settle();
        QVERIFY(!prompt(window.get(), "Rename Cloud PC label"));
        QCOMPARE(store->profiles().first().name, "Original label");
        QVERIFY(!button(window.get(), "Rename…")->isEnabled());
        QCOMPARE(controls.stops, 0);
    }
    void aboutShowsBuildVersion_data()
    {
        QTest::addColumn<QString>("version");
        QTest::addColumn<QString>("shown");
        QTest::newRow("release") << "0.1.0" << "Version 0.1.0";
        QTest::newRow("pre-release") << "0.1.0-rc.2" << "Version 0.1.0-rc.2 · pre-release";
        QTest::newRow("development") << "0.1.0-rc.2+git.0123456789ab" << "Version 0.1.0-rc.2+git.0123456789ab · development build";
    }
    void aboutShowsBuildVersion()
    {
        QFETCH(QString, version);
        QFETCH(QString, shown);
        // main.cpp sets the version stamped by the build (VERSION, or VERSION+git/local.<commit>).
        const QString previous = QCoreApplication::applicationVersion();
        const auto restore = qScopeGuard([previous] { QCoreApplication::setApplicationVersion(previous); });
        QCoreApplication::setApplicationVersion(version);
        openWindow();
        QVERIFY(click(window.get(), "About…"));
        auto* about = prompt(window.get(), "About OMAWIN365");
        QVERIFY(about);
        bool found = false;
        for (auto* label : about->findChildren<QLabel*>())
            found = found || label->text() == shown;
        QVERIFY(found);
    }
    void aboutReusesDialogAndReopensAfterDeletion()
    {
        openWindow();
        QVERIFY(click(window.get(), "About…"));
        auto* about = prompt(window.get(), "About OMAWIN365");
        QVERIFY(about);
        QPointer<QDialog> old = about;
        // Repeated explicit launcher action must raise, not duplicate, About.
        button(window.get(), "About…")->click();
        QCOMPARE(prompt(window.get(), "About OMAWIN365"), about);
        QCOMPARE(window->findChildren<QDialog*>("aboutDialog").size(), 1);
        QVERIFY(click(about, "Licence and notices"));
        bool foundLicence = false;
        for (auto* text : about->findChildren<QLabel*>())
            if (text->isVisible() && text->text().contains("MIT License")) foundLicence = true;
        QVERIFY(foundLicence);
        QVERIFY(click(about, "Close"));
        settle();
        QVERIFY(old.isNull());
        QVERIFY(click(window.get(), "About…"));
        auto* reopened = prompt(window.get(), "About OMAWIN365");
        QVERIFY(reopened);
        QVERIFY(click(reopened, "Close"));
        QVERIFY(controls.starts.isEmpty());
    }
    void invalidImportShowsErrorWithoutConnecting()
    {
        openWindow();
        confirmation("Import trusted connection", QMessageBox::Ok);
        QVERIFY(!window->importProfile(temporary->filePath("missing.rdpw")));
        auto* error = prompt(window.get(), "Import failed");
        QVERIFY(error);
        QVERIFY(click(error, "OK"));
        QVERIFY(store->profiles().isEmpty());
        QVERIFY(controls.starts.isEmpty());
    }
};

int main(int argc, char** argv)
{
    // Keep explicit QtTest -o paths relative to the caller, despite sandboxing
    // the file chooser's working directory. Other CLI arguments are unchanged.
    QList<QByteArray> arguments;
    for (int index = 0; index < argc; ++index) {
        QByteArray argument(argv[index]);
        if (index > 0 && QByteArray(argv[index - 1]) == "-o") {
            const qsizetype comma = argument.lastIndexOf(',');
            const QByteArray file = comma < 0 ? argument : argument.left(comma);
            if (!file.isEmpty() && file != "-" && QFileInfo(QString::fromLocal8Bit(file)).isRelative())
                argument = QDir::current().absoluteFilePath(QString::fromLocal8Bit(file)).toLocal8Bit()
                    + (comma < 0 ? QByteArray() : argument.mid(comma));
        }
        arguments.append(argument);
    }
    QList<char*> argumentPointers;
    for (auto& argument : arguments) argumentPointers.append(argument.data());
    argumentPointers.append(nullptr);
    // All isolation precedes QApplication/fontconfig/platform initialization.
    QTemporaryDir home;
    if (!home.isValid()) return 2;
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qunsetenv("QT_QPA_PLATFORMTHEME");
    qputenv("HOME", home.path().toUtf8());
    qputenv("DBUS_SESSION_BUS_ADDRESS", QByteArray("unix:path=") + home.path().toUtf8() + "/no-session-bus");
    if (!QDir::setCurrent(home.path())) return 2;
    for (const char* key : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"}) {
        const QString path = home.filePath(QString::fromLatin1(key));
        if (!QDir().mkpath(path)) return 2;
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        qputenv(key, path.toUtf8());
    }
    qputenv("XDG_CONFIG_DIRS", home.filePath("system-config").toUtf8());
    qputenv("XDG_DATA_DIRS", home.filePath("system-data").toUtf8());
    qputenv("FONTCONFIG_PATH", home.path().toUtf8());
    qputenv("FONTCONFIG_FILE", home.filePath("fonts.conf").toUtf8());
    if (!writeFile(home.filePath("fonts.conf"),
        "<?xml version=\"1.0\"?><!DOCTYPE fontconfig SYSTEM \"urn:fontconfig:fonts.dtd\">"
        "<fontconfig><dir>/usr/share/fonts</dir><cachedir>" + home.path().toUtf8()
        + "/font-cache</cachedir></fontconfig>")) return 2;
    // Theme's fc-match subprocess cannot find a user executable/configuration.
    qputenv("PATH", home.filePath("no-executables").toUtf8());
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argumentPointers.data());
    QApplication::setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName("omawin365-window-tests");
    QCoreApplication::setApplicationName("window-tests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    WindowTests tests;
    return QTest::qExec(&tests, argc, argumentPointers.data());
}

#include "tst_window.moc"
