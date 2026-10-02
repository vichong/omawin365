#pragma once
#include "../src/browserauth.h"
// Only CDP/owned-page and inert process scaffolding is substituted.
struct BrowserAuthTestAccess {
    static bool inertTransport(BrowserAuth& browser);
    static void adoptPage(BrowserAuth& browser);
    static void callback(BrowserAuth& browser, const QUrl& url, const QString& method = "GET");
};
