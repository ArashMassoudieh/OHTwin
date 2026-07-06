QT += core gui widgets charts network svg svgwidgets
CONFIG += c++17
TARGET   = OHTwinViewer
TEMPLATE = app

SOURCES += \
    PosteriorHistoryLoader.cpp \
    RealizationCILoader.cpp \
    main.cpp \
    Bootstrapper.cpp \
    MainWindow.cpp \
    AssimViewer.cpp \
    CsvLoader.cpp \
    GaMergedLoader.cpp

HEADERS += \
    Bootstrapper.h \
    MainWindow.h \
    AssimViewer.h \
    CsvLoader.h \
    GaMergedLoader.h \
    OhqTime.h \
    PosteriorHistoryLoader.h \
    RealizationCILoader.h

DISTFILES += \
    config.json \
    config_assimilation.json \
    config_forward.json

# Copy viewer config next to the binary when present in the project folder.
# Safe for desktop builds; for WASM deployment, keep config.json beside the
# generated .html/.js/.wasm files on the web server.
exists($$PWD/config.json) {
    copy_viewer_config.commands = $$QMAKE_COPY $$shell_quote($$PWD/config.json) $$shell_quote($$OUT_PWD/config.json)
    QMAKE_EXTRA_TARGETS += copy_viewer_config
    PRE_TARGETDEPS += copy_viewer_config
}

# Qt 6 wasm: QApplication + widgets are supported.
# Build with:
#   /path/to/qt-wasm/bin/qmake DrywellDTViewer.pro
#   make
# Output: DrywellDTViewer.html + .js + .wasm.
# Serve those files with config.json beside them.
#
# Mode is now selected by the top-level "mode" key in config.json:
#   "mode": "forward"      -> MainWindow (charts + SVG, like before)
#   "mode": "assimilation" -> AssimViewer (fitness + parameter tabs)
# On desktop, an alternative config can be passed with:
#   ./DrywellDTViewer --config /path/to/other_config.json
