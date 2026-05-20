/*
 * OpenHydroTwin
 * Copyright (C) 2026  EnviroInformatics, LLC
 *
 * This file is part of OpenHydroTwin.
 *
 * OpenHydroTwin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenHydroTwin is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <QDateTime>
#include <QMutex>
#include <QString>
#include <atomic>

// ---------------------------------------------------------------------------
// RunLogger
//
// Thread-safe append-only logger for forward and inverse run records.
// Both DTRunner (main thread, forward stages) and DTAssimilation
// (assim thread, calibration cycles) write to the same outputs/run_log.csv
// via recordRun().  A QMutex serializes file appends so rows are atomic;
// a std::atomic counter assigns globally-unique run_ids.
//
// Lifetime: owned by DTRunner.  A raw pointer is handed to DTAssimilation
// which must outlive its assim thread (DTRunner ensures this in its
// destructor by joining the thread before m_assimilation is destroyed).
// ---------------------------------------------------------------------------
class RunLogger
{
public:
    enum class RunType
    {
        ForwardAdvance,
        ForwardForecast,
        AssimCalibration
    };

    enum class Status
    {
        Ok,
        Failed
    };

    // Constructs the logger and writes the CSV header if the file
    // does not yet exist.  outputDir must already exist on disk.
    explicit RunLogger(const QString &outputDir);

    // Append one row to run_log.csv.  Thread-safe.
    //
    //   type, cycle              what kind of run, which cycle index
    //   start, end               UTC wall-clock bounds
    //   simStartSerial, simEndSerial
    //                            OHQ day-serials of the simulation window
    //   initialStatePath         JSON snapshot loaded as IC (or "(cold start)")
    //   outputStatePath          JSON snapshot written at end (empty for
    //                            stages that produce no snapshot, e.g. forecast)
    //   status                   Ok / Failed
    //   notes                    free-form, e.g. error message; commas/newlines
    //                            are sanitized so the CSV stays well-formed
    void recordRun(RunType type,
                   int cycle,
                   const QDateTime &start,
                   const QDateTime &end,
                   double simStartSerial,
                   double simEndSerial,
                   const QString &initialStatePath,
                   const QString &outputStatePath,
                   Status status,
                   const QString &notes = QString());

private:
    static QString typeToString(RunType t);
    static QString statusToString(Status s);
    static QString sanitizeForCsv(const QString &s);

    QString          m_path;
    QMutex           m_mutex;
    std::atomic<int> m_nextRunId{1};
};
