QT       -= core
QT       -= gui

TARGET = test
CONFIG   += console
CONFIG   -= app_bundle

TEMPLATE = app

QMAKE_CFLAGS += -std=c89

SOURCES += \
    CspChan.c \
    test.c

HEADERS += \
    CspChan.h
