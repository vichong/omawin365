QT += core network testlib
QT -= gui
CONFIG += console c++20 testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = tst_session
INCLUDEPATH += ../src
SOURCES += tst_session.cpp ../src/promptparser.cpp ../src/rdpprofile.cpp
HEADERS += ../src/session.h ../src/promptparser.h ../src/rdpprofile.h synthetic_profile.h
LIBS += -lX11

SOURCES += ../src/oauthcontract.cpp
HEADERS += ../src/oauthcontract.h

HEADERS += certificate_fixture.h
