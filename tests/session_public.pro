QT += core network testlib
QT -= gui
CONFIG += console c++20 testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = tst_session_public
INCLUDEPATH += ../src
SOURCES += session_public_tests.cpp ../src/session.cpp ../src/promptparser.cpp ../src/rdpprofile.cpp ../src/profilestore.cpp
HEADERS += ../src/session.h ../src/promptparser.h session_fixture.h session_public_cases.h ../src/rdpprofile.h synthetic_profile.h ../src/profilestore.h
LIBS += -lX11
DEFINES += SESSION_FIXTURE_PATH=\\\"$$PWD/session_fixture.py\\\"
DISTFILES += session_fixture.py

SOURCES += ../src/oauthcontract.cpp
HEADERS += ../src/oauthcontract.h

HEADERS += ../src/browserauth.h auth_composed.h

HEADERS += certificate_fixture.h
