QT += core gui widgets testlib
CONFIG += console c++20 testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = tst_window
INCLUDEPATH += ../src
SOURCES += tst_window.cpp window_adapters.cpp ../src/window.cpp ../src/windowpresentation.cpp ../src/profilestore.cpp ../src/theme.cpp ../src/rdpprofile.cpp
HEADERS += window_fixture.h ../src/window.h ../src/windowpresentation.h ../src/session.h ../src/browserauth.h ../src/profilestore.h ../src/theme.h ../src/rdpprofile.h synthetic_profile.h
RESOURCES += ../resources.qrc

HEADERS += certificate_fixture.h
