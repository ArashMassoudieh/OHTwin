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

#pragma once

// ---------------------------------------------------------------------------
// DTDebugLog
//
// Config-controlled deep-debug logger for OpenHydroTwin. One instance per
// process (Meyers singleton), writing timestamped, category-tagged lines to
// a single file. Designed so that:
//
//   - a disabled logger (or disabled category) costs one relaxed atomic
//     load per call site — guard expensive message construction with
//     enabled(cat) so no formatting happens when off;
//   - concurrent writers (the OpenMP chain sweep in DTStreamingMCMC, the
//     assimilation thread, the forward loop) are serialized by a mutex,
//     with each line carrying a thread tag so interleaved per-chain events
//     can be untangled;
//   - lines are buffered and flushed every flushEvery lines (config knob).
//     Set flush_every to 1 when chasing a crash so the tail of the log
//     survives; leave it higher for long soak runs.
//
// Configuration comes from the optional "logging" block in config.json
// (parsed by DTConfig, applied in main right after config load):
//
//   "logging": {
//       "enabled": true,
//       "file": "outputs/debug.log",          // deployment-root-relative
//       "categories": ["assim", "mcmc", "mcmc_trace", "snapshot", "weather"],
//       "flush_every": 50,
//       "truncate": true                       // false => append across runs
//   }
//
// Omitted "categories" enables everything EXCEPT mcmc_trace (the per-step
// firehose), which must be requested explicitly.
//
// Usage pattern:
//
//   DTDebugLog &dlog = DTDebugLog::instance();
//   if (dlog.enabled(DTDebugLog::Category::MCMC))
//       dlog.log(DTDebugLog::Category::MCMC,
//                QString("chain %1 plateaued at sweep %2").arg(c).arg(s));
// ---------------------------------------------------------------------------

#include <QString>
#include <QFile>
#include <QMutex>

#include <atomic>
#include <cstdint>
#include <vector>
#include <string>

class DTDebugLog
{
public:
    // Bit positions; extend freely (uint32 mask => 32 categories max).
    enum class Category : std::uint32_t
    {
        Runner    = 1u << 0,   // forward loop / DTRunner lifecycle
        Assim     = 1u << 1,   // calibration cycle orchestration
        MCMC      = 1u << 2,   // cycle-level sampler events (seeding,
                               // plateau transitions, culls, adaptation,
                               // publication)
        MCMCTrace = 1u << 3,   // per-chain, per-step firehose (accept/
                               // reject, logp) — very high volume
        Snapshot  = 1u << 4,   // snapshot / posterior-file I/O
        Weather   = 1u << 5,   // forcing fetch & injection
        Config    = 1u << 6    // resolved configuration echo
    };

    static DTDebugLog &instance();

    // Apply the parsed config. Call once, on the main thread, immediately
    // after DTConfig::load succeeds and before any threads start. Safe to
    // call with enabled=false (the default state): the logger stays inert.
    // categoryNames uses the lower-case config spellings ("assim", "mcmc",
    // "mcmc_trace", "snapshot", "weather", "runner", "config"); an empty
    // list means all categories except mcmc_trace.
    bool configure(const QString &filePath,
                   bool enabled,
                   const std::vector<std::string> &categoryNames,
                   int flushEvery,
                   bool truncate,
                   QString &errorMessage);

    // Fast gate: true iff logging is on AND the category is selected.
    // Call before constructing expensive messages.
    bool enabled(Category c) const
    {
        return m_enabled.load(std::memory_order_relaxed) &&
               (m_mask.load(std::memory_order_relaxed) &
                static_cast<std::uint32_t>(c));
    }

    // Append one line: "<ISO-8601 ms UTC> [<cat>] [t:<thread>] <message>".
    // No-op when the category gate is closed, so unguarded calls are safe
    // (just wasteful if the message was expensive to build).
    void log(Category c, const QString &message);

    // Force buffered lines to disk (called automatically every flushEvery
    // lines, at cycle boundaries by the instrumented code, and on exit).
    void flush();

    ~DTDebugLog();

    DTDebugLog(const DTDebugLog &)            = delete;
    DTDebugLog &operator=(const DTDebugLog &) = delete;

private:
    DTDebugLog() = default;

    static const char *categoryTag(Category c);

    std::atomic<bool>          m_enabled{false};
    std::atomic<std::uint32_t> m_mask{0};

    QMutex              m_mutex;      // guards everything below
    QFile               m_file;
    std::vector<QByteArray> m_buffer;
    int                 m_flushEvery = 50;
};
