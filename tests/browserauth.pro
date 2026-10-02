QT += core
QT -= gui
CONFIG += console c++20
CONFIG -= app_bundle
TEMPLATE = app
TARGET = browserauth_test
SOURCES += browserauth_test.cpp
HEADERS += ../src/browserauth.h browserauth_public_tests.h
RESOURCES += browserauth.qrc
DEFINES += BROWSER_TEST_SOURCE_DIR=\\\"$$PWD/..\\\"

SOURCES += ../src/oauthcontract.cpp
HEADERS += ../src/oauthcontract.h
