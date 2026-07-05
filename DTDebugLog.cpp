/*
 * OpenHydroTwin
 * Copyright (C) 2026  Arash Massoudieh
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

#include "DTDebugLog.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QThread>

#include <algorithm>
#include <iostream>

// ---------------------------------------------------------------------------
DTDebugLog &DTDebugLog::instance()
{
    static DTDebugLog s_instance;
    return s_instance;
}

// ---------------------------------------------------------------------------
const char *DTDebugLog::categoryTag(Category c)
{
    switch (c)
    {
    case Category::Runner:    return "runner";
    case Category::Assim:     return "assim";
    case Category::MCMC:      return "mcmc";
    case Category::MCMCTrace: return "trace";
    case Category::Snapshot:  return "snapshot";
    case Category::Weather:   return "weather";
    case Category::Config:    return "config";
    }
    return "?";
}

// ---------------------------------------------------------------------------
bool DTDebugLog::configure(const QString &filePath,
                           bool enabled,
                           const std::vector<std::string> &categoryNames,
                           int flushEvery,
                           bool truncate,
                           QString &errorMessage)
{
    QMutexLocker lock(&m_mutex);

    // Reconfiguration (e.g., tests): close any previous target first.
    if (m_file.isOpen())
    {
        for (const QByteArray &line : m_buffer) m_file.write(line);
        m_buffer.clear();
        m_file.close();
    }
    m_enabled.store(false, std::memory_order_relaxed);

    if (!enabled)
        return true;                        // stay inert; not an error

    // --- category mask -----------------------------------------------------
    std::uint32_t mask = 0;
    if (categoryNames.empty())
    {
        // Everything except the per-step firehose, which is opt-in.
        mask = ~0u & ~static_cast<std::uint32_t>(Category::MCMCTrace);
    }
    else
    {
        for (std::string name : categoryNames)
        {
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            if      (name == "runner")     mask |= static_cast<std::uint32_t>(Category::Runner);
            else if (name == "assim")      mask |= static_cast<std::uint32_t>(Category::Assim);
            else if (name == "mcmc")       mask |= static_cast<std::uint32_t>(Category::MCMC);
            else if (name == "mcmc_trace") mask |= static_cast<std::uint32_t>(Category::MCMCTrace);
            else if (name == "snapshot")   mask |= static_cast<std::uint32_t>(Category::Snapshot);
            else if (name == "weather")    mask |= static_cast<std::uint32_t>(Category::Weather);
            else if (name == "config")     mask |= static_cast<std::uint32_t>(Category::Config);
            else
            {
                errorMessage = "logging.categories: unknown category '"
                               + QString::fromStdString(name) + "'";
                return false;
            }
        }
    }

    // --- output file --------------------------------------------------------
    if (filePath.isEmpty())
    {
        errorMessage = "logging.file resolved to an empty path";
        return false;
    }
    QDir().mkpath(QFileInfo(filePath).absolutePath());

    m_file.setFileName(filePath);
    const QIODevice::OpenMode openMode =
        QIODevice::WriteOnly | (truncate ? QIODevice::Truncate
                                         : QIODevice::Append);
    if (!m_file.open(openMode))
    {
        errorMessage = "cannot open debug log for writing: " + filePath;
        return false;
    }

    m_flushEvery = std::max(1, flushEvery);
    m_buffer.clear();
    m_buffer.reserve(static_cast<size_t>(m_flushEvery));

    m_mask.store(mask, std::memory_order_relaxed);
    m_enabled.store(true, std::memory_order_relaxed);

    // Session banner, written through immediately.
    const QByteArray banner =
        (QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
         + " [config] debug log opened (flush_every="
         + QString::number(m_flushEvery)
         + (truncate ? ", truncated" : ", appending") + ")\n").toUtf8();
    m_file.write(banner);
    m_file.flush();

    std::cout << "[DebugLog] enabled -> " << filePath.toStdString() << "\n";
    return true;
}

// ---------------------------------------------------------------------------
void DTDebugLog::log(Category c, const QString &message)
{
    if (!enabled(c))
        return;

    // Timestamp and thread tag are built outside the lock; only the
    // buffer append and (occasional) file write are serialized.
    const QString line =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
        + " [" + categoryTag(c) + "] [t:"
        + QString::number(reinterpret_cast<quintptr>(
              QThread::currentThreadId()), 16)
        + "] " + message + "\n";
    const QByteArray bytes = line.toUtf8();

    QMutexLocker lock(&m_mutex);
    if (!m_file.isOpen())
        return;
    m_buffer.push_back(bytes);
    if (static_cast<int>(m_buffer.size()) >= m_flushEvery)
    {
        for (const QByteArray &b : m_buffer) m_file.write(b);
        m_buffer.clear();
        m_file.flush();
    }
}

// ---------------------------------------------------------------------------
void DTDebugLog::flush()
{
    QMutexLocker lock(&m_mutex);
    if (!m_file.isOpen())
        return;
    for (const QByteArray &b : m_buffer) m_file.write(b);
    m_buffer.clear();
    m_file.flush();
}

// ---------------------------------------------------------------------------
DTDebugLog::~DTDebugLog()
{
    flush();
    QMutexLocker lock(&m_mutex);
    if (m_file.isOpen())
        m_file.close();
}
