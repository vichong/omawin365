QT += core gui widgets network
CONFIG += c++20 warn_on
CONFIG -= app_bundle
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
