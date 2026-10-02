QT += core network testlib
QT -= gui
CONFIG += console c++20 testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = tst_promptparser
INCLUDEPATH += ../src
SOURCES += tst_promptparser.cpp ../src/promptparser.cpp
HEADERS += ../src/promptparser.h

SOURCES += ../src/oauthcontract.cpp
HEADERS += ../src/oauthcontract.h
