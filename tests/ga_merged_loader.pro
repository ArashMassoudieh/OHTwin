QT += core network testlib
QT -= gui
CONFIG += c++17 console testcase
TEMPLATE = app
TARGET = ga_merged_loader_test
INCLUDEPATH += $$PWD/../viewer
SOURCES += $$PWD/ga_merged_loader_test.cpp $$PWD/../viewer/GaMergedLoader.cpp
HEADERS += $$PWD/../viewer/GaMergedLoader.h
