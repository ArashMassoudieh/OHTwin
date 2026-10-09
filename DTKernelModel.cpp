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
#include "DTKernelModel.h"

#include "ohq_kernel.h"

#include <QFile>
#include <QJsonDocument>

#include <iostream>

bool KernelState::save(const QString &path, const QJsonObject &extra) const
{
    QJsonObject o = extra;
    o["_kernel_state_time"] = time;
    QJsonObject s;
    for (const auto &kv : storage) s[QString::fromStdString(kv.first)] = kv.second;
    o["_kernel_states"] = s;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return true;
}

bool KernelState::load(const QString &path, QJsonObject *all)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (!o.contains("_kernel_states")) return false;
    time = o.value("_kernel_state_time").toDouble();
    storage.clear();
    const QJsonObject s = o.value("_kernel_states").toObject();
    for (auto it = s.constBegin(); it != s.constEnd(); ++it) storage[it.key().toStdString()] = it.value().toDouble();
    if (all) *all = o;
    return true;
}

DTKernelModel::DTKernelModel() : k_(new ohq::Kernel) {}
DTKernelModel::~DTKernelModel() = default;

bool DTKernelModel::load(const std::string &libraryPath, QString &err)
{
    if (!k_->load(libraryPath))
    {
        err = QString::fromStdString(k_->error());
        return false;
    }
    std::cout << "[Kernel] loaded " << libraryPath << " (class " << k_->class_name() << "): "
              << k_->n_states() << " states, " << k_->n_parameters() << " parameters, " << k_->n_observations()
              << " observations, " << k_->n_series() << " forcing series\n";
    return true;
}

bool DTKernelModel::loaded() const { return k_ && k_->n_states() > 0; }
std::string DTKernelModel::className() const { return k_->class_name(); }

std::vector<std::string> DTKernelModel::outputNames() const
{
    std::vector<std::string> n;
    for (int i = 0; k_ && i < k_->outputCount(); ++i) n.push_back(k_->output_name(i));
    return n;
}

KernelStageResult DTKernelModel::runStage(double t0, double t1, double dt0, const KernelState &init,
                                          const std::map<std::string, double> &parameters,
                                          const ForcingConfig &forcing,
                                          const std::vector<ForcingSeries> &series) const
{
    KernelStageResult r;
    ohq::Kernel &k = *k_;
    void *h = k.create();

    // parameters: overrides by name (values not listed keep those compiled into the kernel)
    int nSet = 0;
    for (int i = 0; i < k.n_parameters(); ++i)
    {
        const auto it = parameters.find(k.parameter_name(i));
        if (it != parameters.end()) { k.set_parameter(h, i, it->second); ++nSet; }
    }
    for (const auto &p : parameters)
    {
        bool found = false;
        for (int i = 0; i < k.n_parameters() && !found; ++i) found = (p.first == k.parameter_name(i));
        if (!found) std::cerr << "[Kernel] warning: parameter '" << p.first << "' is not in the kernel\n";
    }
    k.apply_parameters(h);
    k.initialize_at(h, t0, dt0);

    // saved state, matched by name
    if (!init.empty())
    {
        const int ns = k.n_states();
        std::vector<double> st(ns);
        k.export_state(h, st.data(), nullptr, nullptr, nullptr);   // initial values as the default
        int matched = 0;
        for (int i = 0; i < ns; ++i)
        {
            const auto it = init.storage.find(k.state_name(i));
            if (it != init.storage.end()) { st[i] = it->second; ++matched; }
        }
        k.import_state(h, t0, st.data(), nullptr, nullptr, nullptr);
        if (matched < ns)
            std::cerr << "[Kernel] warning: snapshot has " << matched << " of " << ns
                      << " states; the rest start from the model's initial values\n";
    }

    // forcing
    for (size_t e = 0; e < forcing.entries.size() && e < series.size(); ++e)
    {
        const ForcingEntry &fe = forcing.entries[e];
        const ForcingSeries &s = series[e];
        for (const ForcingTarget &t : fe.targets)
        {
            const int ok = fe.isPrecipitation()
                               ? k.set_precipitation(h, t.source.c_str(), t.quantity.c_str(), s.start.data(),
                                                     s.end.data(), s.value.data(), static_cast<int>(s.value.size()))
                               : k.set_series(h, t.source.c_str(), t.quantity.c_str(), s.t.data(), s.value.data(),
                                              static_cast<int>(s.value.size()));
            if (!ok)
            {
                r.error = QString("kernel has no forcing series %1/%2").arg(t.source.c_str(), t.quantity.c_str());
                k.destroy(h);
                return r;
            }
        }
    }

    // outputs: run to each sample time and read them (the step is clamped to the sample times)
    const int nOut = k.outputCount();
    std::vector<TimeSeries<double>> outs(outputInterval_ > 0 ? nOut : 0);
    std::vector<double> buf(nOut > 0 ? nOut : 1);
    auto sample = [&]() {
        k.outputs(h, buf.data());
        for (size_t i = 0; i < outs.size(); ++i) outs[i].append(k.time(h), buf[i]);
    };
    int ok = 1;
    if (!outs.empty())
    {
        sample();
        for (int n = 1; ok && t0 + n * outputInterval_ < t1 - 1e-9; ++n)
            if ((ok = k.run_to(h, t0 + n * outputInterval_)) && !k.solution_failed(h)) sample();
    }
    if (ok) ok = k.run_to(h, t1);
    if (ok && !outs.empty() && !k.solution_failed(h)) sample();
    for (size_t i = 0; i < outs.size(); ++i) r.outputs.append(outs[i], k.output_name(int(i)));
    r.steps = k.step_count(h);
    if (!ok || k.solution_failed(h))
    {
        r.error = QString("kernel solve failed at t = %1 (target %2)").arg(k.time(h), 0, 'f', 5).arg(t1, 0, 'f', 5);
        k.destroy(h);
        return r;
    }

    for (int io = 0; io < k.n_observations(); ++io)
    {
        TimeSeries<double> ts;
        ts.setName(k.observation_name(io));
        for (int j = 0; j < k.observation_count(h, io); ++j)
        {
            double t = 0, v = 0;
            if (k.observation_at(h, io, j, &t, &v)) ts.append(t, v);
        }
        r.observed.append(ts, k.observation_name(io));
    }
    const int ns = k.n_states();
    std::vector<double> st(ns);
    k.export_state(h, st.data(), nullptr, nullptr, nullptr);
    r.endState.time = k.time(h);
    for (int i = 0; i < ns; ++i) r.endState.storage[k.state_name(i)] = st[i];
    k.destroy(h);
    r.ok = true;
    std::cout << "[Kernel] stage " << t0 << " -> " << t1 << ": " << r.steps << " steps, " << nSet
              << " parameter overrides\n";
    return r;
}
