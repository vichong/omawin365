#pragma once

#include "session.h"

#include <QStringList>
#include <QUrl>
#include <QList>
#include <functional>

// Link-time adapters for Window only, not production transport implementations.
// Stop deliberately waits for a test-issued ended signal to model async teardown.
namespace WindowFixture {
struct Controls {
    bool active = false;
    int rememberSignIn = 0; // BrowserAuth::RememberSignIn
    int forgets = 0;
    int savedForgets = 0;
    QStringList starts;
    QList<LaunchOptions> options;
    int stops = 0;
    QStringList pins;
    QList<QUrl> callbacks;
    QList<QUrl> authorizations;
    int acquisitions = 0;
    int cancellations = 0;
    QStringList selections;
    std::function<void()> onStop;
    std::function<void()> onPin;
    std::function<void()> onAcquire;
    std::function<void()> onCancel;
};
extern Controls controls;
}
