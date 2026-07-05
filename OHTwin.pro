QT -= gui
QT += core network

CONFIG += console
CONFIG -= app_bundle

CONFIG += c++14

# ============================================================
# Build folder
# ============================================================
BUILD_DIR = $$PWD/build-qmake

!exists($$BUILD_DIR)     { system(mkdir -p $$BUILD_DIR) }
!exists($$BUILD_DIR/obj) { system(mkdir -p $$BUILD_DIR/obj) }
!exists($$BUILD_DIR/moc) { system(mkdir -p $$BUILD_DIR/moc) }
!exists($$BUILD_DIR/rcc) { system(mkdir -p $$BUILD_DIR/rcc) }
!exists($$BUILD_DIR/ui)  { system(mkdir -p $$BUILD_DIR/ui) }
!exists($$BUILD_DIR/bin) { system(mkdir -p $$BUILD_DIR/bin) }

OBJECTS_DIR = $$BUILD_DIR/obj
MOC_DIR     = $$BUILD_DIR/moc
RCC_DIR     = $$BUILD_DIR/rcc
UI_DIR      = $$BUILD_DIR/ui
DESTDIR     = $$BUILD_DIR/bin

message("BUILD_DIR = $$BUILD_DIR")
message("DESTDIR   = $$DESTDIR")

# Set to 1 to compile OHQ from source, 0 to use the shared library
OHQ_FROM_SOURCE = 1

INCLUDEPATH += ../OpenHydroQual/aquifolium/include
INCLUDEPATH += ../OpenHydroQual/aquifolium/src
INCLUDEPATH += ../OpenHydroQual/aquifolium/include/GA
INCLUDEPATH += ../OpenHydroQual/aquifolium/include/MCMC
INCLUDEPATH += ../OpenHydroQual/jsoncpp/include/
INCLUDEPATH += ../OpenHydroQual/

macx:  DEFINES += mac_version
linux: DEFINES += ubuntu_version
win32: DEFINES += windows_version

DEFINES += Terminal_version
DEFINES += Q_JSON_SUPPORT

TARGET = OHTwin
TEMPLATE = app
win32:QMAKE_CXXFLAGS += /MP

# ── OHQ: source vs library ───────────────────────────────────────
equals(OHQ_FROM_SOURCE, 1) {
    message(Building with OHQ source files)
    SOURCES += \
        ../OpenHydroQual/aquifolium/src/Block.cpp \
        ../OpenHydroQual/aquifolium/src/Command.cpp \
        ../OpenHydroQual/aquifolium/src/Condition.cpp \
        ../OpenHydroQual/aquifolium/src/ErrorHandler.cpp \
        ../OpenHydroQual/aquifolium/src/Expression.cpp \
        ../OpenHydroQual/aquifolium/src/Link.cpp \
        ../OpenHydroQual/aquifolium/src/Matrix.cpp \
        ../OpenHydroQual/aquifolium/src/Matrix_arma.cpp \
        ../OpenHydroQual/aquifolium/src/MetaModel.cpp \
        ../OpenHydroQual/aquifolium/src/NormalDist.cpp \
        ../OpenHydroQual/aquifolium/src/Object.cpp \
        ../OpenHydroQual/aquifolium/src/Objective_Function.cpp \
        ../OpenHydroQual/aquifolium/src/Objective_Function_Set.cpp \
        ../OpenHydroQual/aquifolium/src/Parameter.cpp \
        ../OpenHydroQual/aquifolium/src/Parameter_Set.cpp \
        ../OpenHydroQual/aquifolium/src/Precipitation.cpp \
        ../OpenHydroQual/aquifolium/src/Quan.cpp \
        ../OpenHydroQual/aquifolium/src/QuanSet.cpp \
        ../OpenHydroQual/aquifolium/src/QuickSort.cpp \
        ../OpenHydroQual/aquifolium/src/Rule.cpp \
        ../OpenHydroQual/aquifolium/src/RxnParameter.cpp \
        ../OpenHydroQual/aquifolium/src/Script.cpp \
        ../OpenHydroQual/aquifolium/src/Source.cpp \
        ../OpenHydroQual/aquifolium/src/System.cpp \
        ../OpenHydroQual/aquifolium/src/Utilities.cpp \
        ../OpenHydroQual/aquifolium/src/Vector.cpp \
        ../OpenHydroQual/aquifolium/src/Vector_arma.cpp \
        ../OpenHydroQual/aquifolium/src/constituent.cpp \
        ../OpenHydroQual/aquifolium/src/observation.cpp \
        ../OpenHydroQual/aquifolium/src/precalculatedfunction.cpp \
        ../OpenHydroQual/aquifolium/src/reaction.cpp \
        ../OpenHydroQual/aquifolium/src/restorepoint.cpp \
        ../OpenHydroQual/aquifolium/src/solutionlogger.cpp \
        ../OpenHydroQual/aquifolium/src/GA/Binary.cpp \
        ../OpenHydroQual/aquifolium/src/GA/Individual.cpp \
        ../OpenHydroQual/aquifolium/src/GA/DistributionNUnif.cpp \
        ../OpenHydroQual/aquifolium/src/GA/Distribution.cpp \
        ../OpenHydroQual/jsoncpp/src/lib_json/json_reader.cpp \
        ../OpenHydroQual/jsoncpp/src/lib_json/json_value.cpp \
        ../OpenHydroQual/jsoncpp/src/lib_json/json_writer.cpp
} else {
    message(Building with OHQ shared library)
    CONFIG(debug, debug|release) {
        LIBS += -L$$PWD/libs/debug -lOHQLib
        QMAKE_RPATHDIR += $$PWD/libs/debug
    }
    CONFIG(release, debug|release) {
        LIBS += -L$$PWD/libs/release -lOHQLib
        QMAKE_RPATHDIR += $$PWD/libs/release
    }
}

# ── Platform libs ────────────────────────────────────────────────
linux {
DEFINES += ARMA_USE_LAPACK ARMA_USE_BLAS GSL
    QMAKE_CXXFLAGS += -fopenmp
    QMAKE_LFLAGS   += -fopenmp
    LIBS += -larmadillo -llapack -lblas -lgsl -lgomp
}

macx {
    LIBS += /opt/homebrew/Cellar/armadillo/11.4.2/lib/libarmadillo.dylib
    INCLUDEPATH += $$PWD/../../../../opt/homebrew/Cellar/armadillo/11.4.2/include
    DEPENDPATH += $$PWD/../../../../opt/homebrew/Cellar/armadillo/11.4.2/include
}

# ── Debug/Release ────────────────────────────────────────────────
CONFIG(debug, debug|release) {
    message(Building in debug mode)
    DEFINES += NO_OPENMP DEBUG
}

QMAKE_CXXFLAGS += -Werror=return-type

# ── Project sources ──────────────────────────────────────────────
SOURCES += \
    DTAssimilation.cpp \
    DTConfig.cpp \
    DTDebugLog.cpp \
    DTObservationBuffer.cpp \
    DTRunner.cpp \
    DTStreamingMCMC.cpp \
    DTWeather.cpp \
    RunLogger.cpp \
    VizRenderer.cpp \
    main.cpp \
    noaaweatherfetcher.cpp

HEADERS += \
    ../../XString.h \
    ../OpenHydroQual/aquifolium/include/Objective_Function.h \
    ../OpenHydroQual/aquifolium/include/Objective_Function_Set.h \
    ../OpenHydroQual/aquifolium/include/Precipitation.h \
    ../OpenHydroQual/aquifolium/include/RxnParameter.h \
    ../OpenHydroQual/aquifolium/include/constituent.h \
    ../OpenHydroQual/aquifolium/include/observation.h \
    ../OpenHydroQual/aquifolium/include/precalculatedfunction.h \
    ../OpenHydroQual/aquifolium/include/solutionlogger.h \
    ../OpenHydroQual/aquifolium/include/GA/GA.h \
    ../OpenHydroQual/aquifolium/include/MCMC/MCMC.h \
    ../OpenHydroQual/aquifolium/include/MCMC/MCMC.hpp \
    ../OpenHydroQual/aquifolium/include/Utilities.h \
    ../OpenHydroQual/aquifolium/include/restorepoint.h \
    ../OpenHydroQual/aquifolium/include/safevector.h \
    ../OpenHydroQual/aquifolium/include/safevector.hpp \
    ../OpenHydroQual/aquifolium/include/Block.h \
    ../OpenHydroQual/aquifolium/include/Expression.h \
    ../OpenHydroQual/aquifolium/include/Link.h \
    ../OpenHydroQual/aquifolium/include/Matrix.h \
    ../OpenHydroQual/aquifolium/include/Matrix_arma.h \
    ../OpenHydroQual/aquifolium/include/MetaModel.h \
    ../OpenHydroQual/aquifolium/include/NormalDist.h \
    ../OpenHydroQual/aquifolium/include/Object.h \
    ../OpenHydroQual/aquifolium/include/Quan.h \
    ../OpenHydroQual/aquifolium/include/QuanSet.h \
    ../OpenHydroQual/aquifolium/include/QuickSort.h \
    ../OpenHydroQual/aquifolium/include/System.h \
    ../OpenHydroQual/aquifolium/include/Vector.h \
    ../OpenHydroQual/aquifolium/include/Vector_arma.h \
    ../OpenHydroQual/jsoncpp/include/json/allocator.h \
    ../OpenHydroQual/jsoncpp/include/json/assertions.h \
    ../OpenHydroQual/jsoncpp/include/json/autolink.h \
    ../OpenHydroQual/jsoncpp/include/json/config.h \
    ../OpenHydroQual/jsoncpp/include/json/features.h \
    ../OpenHydroQual/jsoncpp/include/json/forwards.h \
    ../OpenHydroQual/jsoncpp/include/json/json.h \
    ../OpenHydroQual/jsoncpp/include/json/reader.h \
    ../OpenHydroQual/jsoncpp/include/json/value.h \
    ../OpenHydroQual/jsoncpp/include/json/version.h \
    ../OpenHydroQual/jsoncpp/include/json/writer.h \
    ../OpenHydroQual/jsoncpp/src/lib_json/json_tool.h \
    ../OpenHydroQual/jsoncpp/src/lib_json/version.h.in \
    ../OpenHydroQual/aquifolium/include/Parameter.h \
    ../OpenHydroQual/aquifolium/include/Parameter_Set.h \
    ../OpenHydroQual/aquifolium/include/Command.h \
    ../OpenHydroQual/aquifolium/include/Script.h \
    ../OpenHydroQual/aquifolium/include/GA/Binary.h \
    ../OpenHydroQual/aquifolium/include/GA/Distribution.h \
    ../OpenHydroQual/aquifolium/include/GA/DistributionNUnif.h \
    ../OpenHydroQual/aquifolium/include/GA/Individual.h \
    ../OpenHydroQual/aquifolium/include/GA/GA.hpp \
    ../OpenHydroQual/aquifolium/src/TimeSeries.hpp \
    ../OpenHydroQual/aquifolium/src/TimeSeriesSet.hpp \
    ../OpenHydroQual/aquifolium/include/TimeSeries.h \
    ../OpenHydroQual/aquifolium/include/TimeSeriesSet.h \
    ../OpenHydroQual/aquifolium/include/reaction.h \
    DTAssimilation.h \
    DTConfig.h \
    DTDebugLog.h \
    DTObservationBuffer.h \
    DTRunner.h \
    DTStreamingMCMC.h \
    DTWeather.h \
    RunLogger.h \
    VizRenderer.h \
    noaaweatherfetcher.h

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
