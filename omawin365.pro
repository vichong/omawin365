QT += core gui widgets network
CONFIG += c++20 warn_on
CONFIG -= app_bundle
# Developer-only raw FreeRDP stderr log (never in release packages): qmake6 CONFIG+=dev_stderr_log
dev_stderr_log: DEFINES += OMAWIN365_DEV_STDERR_LOG
# Version for --version and About. Packages pass OMAWIN365_VERSION=<pkgver>-<pkgrel>;
# a git checkout reports its commit (recorded when qmake runs; rerun qmake to refresh).
isEmpty(OMAWIN365_VERSION) {
    OMAWIN365_GIT = $$system(git -C $$shell_quote($$PWD) describe --always --dirty --abbrev=12 --exclude=* 2>/dev/null)
    isEmpty(OMAWIN365_GIT): OMAWIN365_VERSION = dev
    else: OMAWIN365_VERSION = dev+$$OMAWIN365_GIT
}
!contains(OMAWIN365_VERSION, "^[A-Za-z0-9.+_~-]+$"): error("OMAWIN365_VERSION has unsupported characters")
DEFINES += OMAWIN365_VERSION=\\\"$$OMAWIN365_VERSION\\\"
TEMPLATE = app
TARGET = omawin365
INCLUDEPATH += src
SOURCES += \
    src/main.cpp \
    src/window.cpp \
    src/windowpresentation.cpp \
    src/theme.cpp \
    src/profilestore.cpp \
    src/rdpprofile.cpp \
    src/session.cpp \
    src/promptparser.cpp \
    src/browserauth.cpp
HEADERS += \
    src/window.h \
    src/windowpresentation.h \
    src/theme.h \
    src/profilestore.h \
    src/rdpprofile.h \
    src/session.h \
    src/promptparser.h \
    src/browserauth.h
RESOURCES += resources.qrc
LIBS += -lutil -lX11

SOURCES += src/oauthcontract.cpp
HEADERS += src/oauthcontract.h
