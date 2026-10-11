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

// Code-generated kernel backend for the forward cycle (solver.backend = "codegen").
//
// A kernel is the OpenHydroQual model compiled to C++ (ohq_generate --project shared); it is driven through
// the model-independent ohq_kernel_* ABI (OpenHydroQual/codegen/tools/ohq_kernel.h). The twin uses it for the
// Advance and Forecast stages instead of building an interpreter System:
//
//   create -> set parameters (overrides by name) -> apply -> initialize_at(t0) -> import saved state ->
//   replace forcing series (forcing map) -> run_to(t1) -> observations + end state.
//
// State snapshots are written as JSON keyed by state NAME, so a kernel rebuilt from the same model (or one
// with blocks added) can still be restarted from an older snapshot; states not in the snapshot keep the
// model's initial values.
#pragma once
#include "DTForcing.h"
#include "TimeSeriesSet.h"

#include <QJsonObject>
#include <QString>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ohq { class Kernel; }

struct KernelState
{
    double time = 0;                     // OHQ day serial
    std::map<std::string, double> storage;
    bool empty() const { return storage.empty(); }
    bool save(const QString &path, const QJsonObject &extra) const;
    bool load(const QString &path, QJsonObject *all = nullptr);
};

struct KernelStageResult
{
    bool ok = false;
    QString error;
    TimeSeriesSet<double> observed;      // one series per model observation, per accepted step
    // kernel outputs (ohq_generate --outputs) on the sampling grid: outputRows[k][i] = output i at outputTimes[k]
    std::vector<double> outputTimes;
    std::vector<std::vector<double>> outputRows;
    KernelState endState;
    long steps = 0;
};

class DTKernelModel
{
public:
    DTKernelModel();
    ~DTKernelModel();

    bool load(const std::string &libraryPath, QString &err);
    bool loaded() const;
    std::string className() const;
    // Kernel outputs ("object:quantity", empty for a kernel generated without --outputs). runStage samples them
    // at the multiples of `days` (an absolute grid, e.g. whole hours) that are >= `from`; days = 0: not sampled.
    std::vector<std::string> outputNames() const;
    void setOutputSampling(double days, double from = -1e300) { outputInterval_ = days; outputFrom_ = from; }

    // Solve [t0, t1]. `init` empty -> the model's own initial values at t0.
    KernelStageResult runStage(double t0, double t1, double dt0, const KernelState &init,
                               const std::map<std::string, double> &parameters,
                               const ForcingConfig &forcing, const std::vector<ForcingSeries> &series) const;

private:
    std::unique_ptr<ohq::Kernel> k_;
    double outputInterval_ = 0, outputFrom_ = -1e300;
};
