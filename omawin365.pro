QT += core gui widgets network
CONFIG += c++20 warn_on
CONFIG -= app_bundle
# Developer-only raw FreeRDP stderr log (never in release packages): qmake6 CONFIG+=dev_stderr_log
dev_stderr_log: DEFINES += OMAWIN365_DEV_STDERR_LOG
# Version for --version and About, from VERSION. Release packages pass it explicitly.
# A git checkout shows VERSION exactly at its release tag, otherwise VERSION+git.<commit>
# (and .dirty with uncommitted changes); recorded when qmake runs, rerun qmake to refresh.
OMAWIN365_RELEASE = $$cat($$PWD/VERSION)
!contains(OMAWIN365_RELEASE, "^[0-9]+\\.[0-9]+\\.[0-9]+(-rc\\.[0-9]+)?$"): error("VERSION must look like 0.1.0 or 0.1.0-rc.2")
isEmpty(OMAWIN365_VERSION) {
    OMAWIN365_VERSION = $$OMAWIN365_RELEASE
    OMAWIN365_TAGS = $$system(git -C $$shell_quote($$PWD) tag --points-at HEAD 2>/dev/null)
    OMAWIN365_GIT = $$system(git -C $$shell_quote($$PWD) rev-parse --short=12 HEAD 2>/dev/null)
    OMAWIN365_DIRTY = $$system(git -C $$shell_quote($$PWD) status --porcelain --untracked-files=no 2>/dev/null)
    OMAWIN365_TAGGED =
    for(tag, OMAWIN365_TAGS): equals(tag, v$$OMAWIN365_RELEASE): OMAWIN365_TAGGED = 1
    !isEmpty(OMAWIN365_GIT) {
        isEmpty(OMAWIN365_TAGGED)|!isEmpty(OMAWIN365_DIRTY): OMAWIN365_VERSION = $${OMAWIN365_RELEASE}+git.$$OMAWIN365_GIT
        !isEmpty(OMAWIN365_DIRTY): OMAWIN365_VERSION = $${OMAWIN365_VERSION}.dirty
    }
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
