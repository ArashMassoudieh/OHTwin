// repro.cpp — reproduce the JM_forecast solver failure locally.
//
// Loads the exact state the failing cycle used (_current_input.json), sets the
// same forecast window, enables the solution logger (which the twin does not)
// and solves. The logger names the block carrying the maximum error, which the
// journal message does not: "state_variable: 0" is the flow phase, not a block.
//
// Precipitation over this window was zero, so no weather injection is needed --
// the reproduction is deterministic from the snapshot alone.
//
//   g++ -O1 -std=c++17 ... repro.cpp -o repro -lOHQLib
//   ./repro <input.json> <tstart> <tend> <solution.log>

#include "System.h"
#include <QCoreApplication>
#include <QString>
#include <iostream>
#include <iomanip>
#include <string>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const std::string input   = argc > 1 ? argv[1] : "state/_current_input.json";
    const double tstart       = argc > 2 ? std::stod(argv[2]) : 46314.67564815;
    const double tend         = argc > 3 ? std::stod(argv[3]) : 46322.67564815;
    const std::string logpath = argc > 4 ? argv[4] : "solution.log";

    const std::string res = "/home/arash/Projects/OpenHydroQual/resources/";

    System sys;
    sys.SetDefaultTemplatePath(res);
    if (!sys.ReadSystemSettingsTemplate(res + "settings.json")) {
        std::cerr << "failed to read settings template\n";
        return 1;
    }
    if (!sys.LoadfromJson(QString::fromStdString(input))) {
        std::cerr << "failed to load " << input << "\n";
        return 1;
    }
    std::cout << "loaded " << input << "  blocks=" << sys.BlockCount()
              << " links=" << sys.LinksCount() << "\n";

    // The window must go into the General Settings object FIRST: a bare
    // SetProp() is reverted by SetSystemSettings(), which re-pushes every
    // stored Settings value back over it.
    Object *gs = sys.object("General Settings");
    if (!gs) { std::cerr << "no General Settings object\n"; return 1; }
    gs->Variable("simulation_start_time")->SetProperty(std::to_string(tstart));
    gs->Variable("simulation_end_time")->SetProperty(std::to_string(tend));
    // SetSystemSettings() re-pushes the Settings vector over the System
    // properties, so the window has to be written INTO that vector --
    // SetSettingsParameter does that; a bare SetProp() is overwritten.
    sys.SetSettingsParameter("simulation_start_time", tstart);
    sys.SetSettingsParameter("simulation_end_time", tend);
    sys.SetProp("simulation_start_time", tstart);
    sys.SetProp("simulation_end_time", tend);
    sys.SetSystemSettings();

    const double gotS = std::stod(gs->Variable("simulation_start_time")->GetProperty());
    const double gotE = std::stod(gs->Variable("simulation_end_time")->GetProperty());
    std::cout << std::setprecision(14)
              << "requested: " << tstart << " -> " << tend << "\n"
              << "actual   : " << gotS << " -> " << gotE
              << "  (" << (gotE - gotS) << " d)\n";
    // std::to_string() writes 6 decimals, so an exact match is not possible;
    // anything under a second (1.2e-5 d) is the write-back rounding, not a revert.
    if (std::abs(gotS - tstart) > 1e-4 || std::abs(gotE - tend) > 1e-4) {
        std::cerr << "WARNING: window differs from requested; continuing anyway\n";
    }

    sys.SetSolutionLogger(logpath);
    sys.SetSilent(false);
    sys.CalcAllInitialValues();

    std::cout << "solving...\n";
    const bool ok = sys.Solve(true);
    std::cout << "\nSolve() returned " << (ok ? "true" : "false") << "\n";
    std::cout << "solution log: " << logpath << "\n";
    return ok ? 0 : 2;
}
