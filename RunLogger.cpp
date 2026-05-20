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

#include "RunLogger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QTextStream>

#include <iostream>

RunLogger::RunLogger(const QString &outputDir)
    : m_path(outputDir + "/run_log.csv")
{
    QDir().mkpath(outputDir);

    const bool exists = QFileInfo::exists(m_path);
    QFile f(m_path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        std::cerr << "[RunLogger] failed to open " << m_path.toStdString()
        << " — run logging disabled\n";
        return;
    }

    if (!exists)
    {
        QTextStream out(&f);
        out << "run_id,type,cycle,"
               "start_utc,end_utc,elapsed_sec,"
               "sim_start_serial,sim_end_serial,sim_days,"
               "initial_state_path,output_state_path,"
               "status,notes\n";
    }
    f.close();
}

void RunLogger::recordRun(RunType type,
                          int cycle,
                          const QDateTime &start,
                          const QDateTime &end,
                          double simStartSerial,
                          double simEndSerial,
                          const QString &initialStatePath,
                          const QString &outputStatePath,
                          Status status,
                          const QString &notes)
{
    const int   runId      = m_nextRunId.fetch_add(1);
    const qint64 elapsedMs = start.msecsTo(end);
    const double elapsedSec = elapsedMs / 1000.0;
    const double simDays    = simEndSerial - simStartSerial;

    QMutexLocker lock(&m_mutex);

    QFile f(m_path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        std::cerr << "[RunLogger] append failed: "
                  << f.errorString().toStdString() << "\n";
        return;
    }

    QTextStream out(&f);
    out.setRealNumberPrecision(8);
    out.setRealNumberNotation(QTextStream::FixedNotation);

    out << runId                                 << ','
        << typeToString(type)                    << ','
        << cycle                                 << ','
        << start.toString(Qt::ISODateWithMs)     << ','
        << end.toString(Qt::ISODateWithMs)       << ','
        << elapsedSec                            << ','
        << simStartSerial                        << ','
        << simEndSerial                          << ','
        << simDays                               << ','
        << sanitizeForCsv(initialStatePath)      << ','
        << sanitizeForCsv(outputStatePath)       << ','
        << statusToString(status)                << ','
        << sanitizeForCsv(notes)                 << '\n';

    f.close();
}

QString RunLogger::typeToString(RunType t)
{
    switch (t)
    {
    case RunType::ForwardAdvance:    return QStringLiteral("forward_advance");
    case RunType::ForwardForecast:   return QStringLiteral("forward_forecast");
    case RunType::AssimCalibration:  return QStringLiteral("assim_calibration");
    }
    return QStringLiteral("unknown");
}

QString RunLogger::statusToString(Status s)
{
    return (s == Status::Ok) ? QStringLiteral("ok") : QStringLiteral("failed");
}

QString RunLogger::sanitizeForCsv(const QString &s)
{
    // Strip newlines and replace commas with semicolons so the field stays
    // a single CSV column without needing full quoting/escaping rules.
    QString out = s;
    out.replace('\n', ' ');
    out.replace('\r', ' ');
    out.replace(',',  ';');
    return out;
}
