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

#include "DTViewerWriter.h"

#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>
#include <QTimeZone>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

namespace
{
const double kNaN = std::numeric_limits<double>::quiet_NaN();

bool globMatch(const char *p, const char *s)
{
    if (!*p) return !*s;
    if (*p == '*') return globMatch(p + 1, s) || (*s && globMatch(p, s + 1));
    return *s && (*p == '?' || *p == *s) && globMatch(p + 1, s + 1);
}

QString fill(QString pattern, const QHash<QString, QString> &ph)
{
    for (auto it = ph.constBegin(); it != ph.constEnd(); ++it) pattern.replace("{" + it.key() + "}", it.value());
    return pattern;
}

// a JSON number array, 4 significant digits, NaN as null (compact: the unit files hold ~50k numbers each)
QByteArray numbers(const std::vector<double> &v, int digits = 4)
{
    QByteArray b = "[";
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (i) b += ',';
        b += std::isfinite(v[i]) ? QByteArray::number(v[i], 'g', digits) : QByteArray("null");
    }
    return b + "]";
}

QByteArray str(const QString &s)
{
    return QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact).mid(1).chopped(1);
}

QString isoUtc(double serial)
{
    const QDateTime e(QDate(1899, 12, 30), QTime(0, 0), QTimeZone::UTC);
    return e.addMSecs(qint64(std::llround(serial * 86400000.0))).toString(Qt::ISODate);
}
} // namespace

// ---------------------------------------------------------------------------------------------------------- init
bool DTViewerWriter::init(const ViewerOutputConfig &cfg, const std::vector<std::string> &outputNames,
                          const ForcingConfig &forcing, QString &err)
{
    cfg_ = cfg;
    QFile f(QString::fromStdString(cfg.config));
    if (!f.open(QIODevice::ReadOnly)) { err = "viewer: cannot read " + f.fileName(); return false; }
    vc_ = QJsonDocument::fromJson(f.readAll()).object();
    if (vc_.isEmpty()) { err = "viewer: not a JSON object: " + f.fileName(); return false; }
    const QDir cdir = QFileInfo(f.fileName()).absoluteDir();
    const QJsonObject ot = vc_.value("output_times").toObject();
    chartStep_ = ot.value("chart_step_hours").toDouble(1) / 24.0;
    mapStep_ = ot.value("map_step_hours").toDouble(3) / 24.0;
    historyDays_ = ot.value("history_days").toDouble(45);

    // units and their placeholders
    const QJsonObject ul = vc_.value("layers").toObject().value("units").toObject();
    QFile uf(cdir.absoluteFilePath(ul.value("url").toString()));
    if (!uf.open(QIODevice::ReadOnly)) { err = "viewer: cannot read the units layer " + uf.fileName(); return false; }
    const QString idField = ul.value("id_field").toString("id");
    QHash<QString, QHash<QString, QString>> ph;
    for (const QJsonValue &fv : QJsonDocument::fromJson(uf.readAll()).object().value("features").toArray())
    {
        const QJsonObject p = fv.toObject().value("properties").toObject();
        const QString id = p.value(idField).toString();
        units_ << id;
        ph[id] = {{"unit", id}, {"segment", p.value("receiving_segment").toString()}};
    }
    units_.sort();

    // kernel outputs by name, with glob lookup
    QHash<QString, int> index;
    for (size_t i = 0; i < outputNames.size(); ++i) index[QString::fromStdString(outputNames[i])] = int(i);
    auto resolve = [&](const QString &name) -> int {
        if (index.contains(name)) return index.value(name);
        int best = -1;
        QString bestName;
        const QByteArray p = name.toUtf8();
        for (size_t i = 0; i < outputNames.size(); ++i)
            if (globMatch(p.constData(), outputNames[i].c_str()))
                if (best < 0 || QString::fromStdString(outputNames[i]) < bestName)
                    { best = int(i); bestName = QString::fromStdString(outputNames[i]); }
        return best;
    };

    int missing = 0;
    QString firstMissing;
    auto addChannel = [&](const QString &key, const QJsonObject &v, const QHash<QString, QString> &p) {
        Channel c;
        c.key = key;
        c.out = resolve(fill(v.value("source").toString(), p));
        c.div = v.contains("divide_by") ? resolve(fill(v.value("divide_by").toString(), p)) : -1;
        c.scale = v.value("scale").toDouble(1.0);
        if (c.out < 0 || (v.contains("divide_by") && c.div < 0))
        {
            if (!missing++) firstMissing = key;
            return;
        }
        channelIndex_[key] = int(channels_.size());
        channels_.push_back(c);
    };
    for (const QString &u : units_)
    {
        // rain: the forcing entry that feeds the unit's rain source
        const QString rs = fill(vc_.value("rain_source").toString(), ph[u]);
        for (size_t e = 0; !rs.isEmpty() && e < forcing.entries.size(); ++e)
            if (forcing.entries[e].isPrecipitation())
                for (const ForcingTarget &t : forcing.entries[e].targets)
                    if (QString::fromStdString(t.source) == rs && !channelIndex_.contains(u + "|rain"))
                    {
                        Channel c;
                        c.key = u + "|rain";
                        c.rainEntry = int(e);
                        channelIndex_[c.key] = int(channels_.size());
                        channels_.push_back(c);
                    }
        for (const QJsonValue &ev : vc_.value("elements").toArray())
            for (const QJsonValue &vv : ev.toObject().value("variables").toArray())
                addChannel(u + "|" + ev.toObject().value("id").toString() + ":" + vv.toObject().value("id").toString(),
                           vv.toObject(), ph[u]);
    }
    for (const QJsonValue &gv : vc_.value("gages").toArray())
    {
        const QJsonObject g = gv.toObject();
        for (const QJsonValue &sv : g.value("series").toArray())
            addChannel("gage|" + g.value("id").toString() + "|" + sv.toObject().value("id").toString(), sv.toObject(),
                       {{"gage", g.value("id").toString()}});
    }
    if (missing)
        warnings_ << QString("%1 viewer variable(s) are not kernel outputs (e.g. %2); regenerate the kernel with "
                             "the list from twin_outputs.py spec").arg(missing).arg(firstMissing);

    // the web folder: config and layers
    const QDir web(QString::fromStdString(cfg.webDir));
    QDir().mkpath(web.absoluteFilePath(vc_.value("outputs").toString("outputs/") + "units"));
    QDir().mkpath(web.absoluteFilePath(vc_.value("outputs").toString("outputs/") + "gages"));
    QStringList urls;
    const QJsonObject layers = vc_.value("layers").toObject();
    for (auto it = layers.begin(); it != layers.end(); ++it)
    {
        if (it.value().isArray())
            for (const QJsonValue &m : it.value().toArray()) urls << m.toObject().value("url").toString();
        else
            urls << it.value().toObject().value("url").toString();
    }
    for (const QString &u : urls)
    {
        if (u.isEmpty()) continue;
        QFile src(cdir.absoluteFilePath(u));
        if (!src.open(QIODevice::ReadOnly)) { warnings_ << "viewer: missing layer " + src.fileName(); continue; }
        QDir().mkpath(QFileInfo(web.absoluteFilePath(u)).absolutePath());
        if (!saveText(web.absoluteFilePath(u), src.readAll(), err)) return false;
    }
    if (!saveText(web.absoluteFilePath("viewer_config.json"), QJsonDocument(vc_).toJson(QJsonDocument::Indented), err))
        return false;

    loadHistory();
    std::cout << "[Viewer] " << units_.size() << " units, " << channels_.size() << " channels; history "
              << histT_.size() << " times -> " << cfg.webDir << "\n";
    for (const QString &w : warnings_) std::cerr << "[Viewer] Warning: " << w.toStdString() << "\n";
    return true;
}

// ------------------------------------------------------------------------------------------------------ values
std::vector<double> DTViewerWriter::values(const Channel &c, const std::vector<double> &t,
                                           const std::vector<std::vector<double>> &rows,
                                           const std::vector<ForcingSeries> &series) const
{
    std::vector<double> y(t.size(), kNaN);
    if (c.rainEntry >= 0)
    {
        // rain rate (mm/h) over [t, t + step) from the precipitation bins
        if (c.rainEntry >= int(series.size())) return y;
        const ForcingSeries &s = series[size_t(c.rainEntry)];
        size_t j = 0;
        for (size_t k = 0; k < t.size(); ++k)
        {
            const double a = t[k], b = t[k] + chartStep_;
            while (j < s.end.size() && s.end[j] <= a) ++j;
            double depth = 0;
            bool any = false;
            for (size_t i = j; i < s.start.size() && s.start[i] < b; ++i)
            {
                const double len = s.end[i] - s.start[i];
                const double ov = std::min(b, s.end[i]) - std::max(a, s.start[i]);
                if (ov > 0 && len > 0) { depth += s.value[i] * ov / len; any = true; }
            }
            y[k] = any ? depth * 1000.0 / (chartStep_ * 24.0) : 0.0;
        }
        return y;
    }
    for (size_t k = 0; k < t.size() && k < rows.size(); ++k)
    {
        double v = rows[k][size_t(c.out)];
        if (c.div >= 0) v = rows[k][size_t(c.div)] != 0 ? v / rows[k][size_t(c.div)] : kNaN;
        y[k] = v * c.scale;
    }
    return y;
}

// ----------------------------------------------------------------------------------------------------- history
void DTViewerWriter::addHistory(const std::vector<double> &t, const std::vector<std::vector<double>> &rows,
                                const std::vector<ForcingSeries> &series)
{
    if (histV_.size() != channels_.size()) { histV_.assign(channels_.size(), {}); histT_.clear(); }
    const double last = histT_.empty() ? -1e300 : histT_.back();
    size_t k0 = 0;
    while (k0 < t.size() && t[k0] <= last + 1e-6) ++k0;           // the stage starts where the history ends
    const std::vector<double> tn(t.begin() + long(k0), t.end());
    const std::vector<std::vector<double>> rn(rows.begin() + long(k0), rows.end());
    for (size_t c = 0; c < channels_.size(); ++c)
        for (double v : values(channels_[c], tn, rn, series)) histV_[c].push_back(float(v));
    histT_.insert(histT_.end(), tn.begin(), tn.end());
    // keep history_days
    size_t drop = 0;
    while (drop < histT_.size() && histT_[drop] < histT_.back() - historyDays_ - 1e-6) ++drop;
    if (drop)
    {
        histT_.erase(histT_.begin(), histT_.begin() + long(drop));
        for (auto &v : histV_) v.erase(v.begin(), v.begin() + long(drop));
    }
    if (!saveHistory()) std::cerr << "[Viewer] Warning: cannot write " << cfg_.historyFile << "\n";
}

bool DTViewerWriter::loadHistory()
{
    histT_.clear();
    histV_.assign(channels_.size(), {});
    QFile f(QString::fromStdString(cfg_.historyFile));
    if (!f.open(QIODevice::ReadOnly)) return false;
    QDataStream in(&f);
    QByteArray magic;
    qint32 version = 0;
    QStringList keys;
    QVector<double> t;
    in >> magic >> version >> keys >> t;
    if (magic != "OWTH" || version != 1) return false;
    histT_.assign(t.begin(), t.end());
    for (const QString &k : keys)
    {
        QVector<float> v;
        in >> v;
        const int c = channelIndex_.value(k, -1);
        if (c >= 0 && v.size() == t.size()) histV_[size_t(c)].assign(v.begin(), v.end());
    }
    for (auto &v : histV_)                               // a channel new to the config: no history
        if (v.size() != histT_.size()) v.assign(histT_.size(), std::numeric_limits<float>::quiet_NaN());
    return in.status() == QDataStream::Ok;
}

bool DTViewerWriter::saveHistory() const
{
    QSaveFile f(QString::fromStdString(cfg_.historyFile));
    if (!f.open(QIODevice::WriteOnly)) return false;
    QDataStream out(&f);
    QStringList keys;
    for (const Channel &c : channels_) keys << c.key;
    out << QByteArray("OWTH") << qint32(1) << keys << QVector<double>(histT_.begin(), histT_.end());
    for (const auto &v : histV_) out << QVector<float>(v.begin(), v.end());
    return f.commit();
}

bool DTViewerWriter::saveText(const QString &path, const QByteArray &data, QString &err)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
    {
        err = "viewer: cannot write " + path;
        return false;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------------- write
bool DTViewerWriter::write(double now, const std::vector<double> &t, const std::vector<std::vector<double>> &rows,
                           const std::vector<ForcingSeries> &series, const QJsonObject &status, QString &err)
{
    // time axis: the history up to now, then the forecast
    std::vector<double> T;
    for (double x : histT_) if (x <= now + 1e-6) T.push_back(x);
    const size_t nh = T.size();
    std::vector<double> tf;
    std::vector<std::vector<double>> rf;
    for (size_t k = 0; k < t.size(); ++k)
        if (t[k] > now + 1e-6) { tf.push_back(t[k]); rf.push_back(rows[k]); }
    T.insert(T.end(), tf.begin(), tf.end());
    auto full = [&](int c) {
        std::vector<double> y(T.size(), kNaN);
        const auto &h = histV_[size_t(c)];
        for (size_t k = 0; k < nh && k < h.size(); ++k) y[k] = h[k];
        const std::vector<double> f = values(channels_[size_t(c)], tf, rf, series);
        std::copy(f.begin(), f.end(), y.begin() + long(nh));
        return y;
    };
    std::vector<double> Tr(T.size());
    for (size_t k = 0; k < T.size(); ++k) Tr[k] = std::round(T[k] * 1e5) / 1e5;

    const QDir web(QString::fromStdString(cfg_.webDir));
    const QString outDir = web.absoluteFilePath(vc_.value("outputs").toString("outputs/"));
    const QByteArray nowB = QByteArray::number(now, 'f', 5);

    // units/<id>.json
    const QJsonArray elements = vc_.value("elements").toArray();
    for (const QString &u : units_)
    {
        QByteArray b = "{\"id\":" + str(u) + ",\"now\":" + nowB + ",\"t\":" + numbers(Tr, 10) + ",\"series\":{";
        bool first = true;
        if (channelIndex_.contains(u + "|rain"))
        {
            b += "\"rain\":{\"label\":\"Rainfall\",\"unit\":\"mm/h\",\"v\":" + numbers(full(channelIndex_.value(u + "|rain"))) + "}";
            first = false;
        }
        for (const QJsonValue &ev : elements)
            for (const QJsonValue &vv : ev.toObject().value("variables").toArray())
            {
                const QString key = ev.toObject().value("id").toString() + ":" + vv.toObject().value("id").toString();
                const int c = channelIndex_.value(u + "|" + key, -1);
                if (c < 0) continue;
                if (!first) b += ',';
                first = false;
                b += str(key) + ":{\"label\":" + str(ev.toObject().value("label").toString() + ": " +
                                                     vv.toObject().value("label").toString()) +
                     ",\"unit\":" + str(vv.toObject().value("unit").toString()) + ",\"v\":" + numbers(full(c)) + "}";
            }
        b += "}}";
        if (!saveText(outDir + "/units/" + u + ".json", b, err)) return false;
    }

    // map_state.json: the map variables on the map grid
    std::vector<size_t> mi;
    for (size_t k = 0; k < T.size(); ++k)
    {
        const double r = std::fmod(T[k] + 1e-6, mapStep_);
        if (r < 2e-6) mi.push_back(k);
    }
    std::vector<double> mt;
    for (size_t k : mi) mt.push_back(Tr[k]);
    QByteArray m = "{\"now\":" + nowB + ",\"times\":" + numbers(mt, 10) + ",\"variables\":{";
    bool firstVar = true;
    for (const QJsonValue &ev : elements)
        for (const QJsonValue &vv : ev.toObject().value("variables").toArray())
        {
            if (!vv.toObject().value("map").toBool()) continue;
            const QString key = ev.toObject().value("id").toString() + ":" + vv.toObject().value("id").toString();
            if (!firstVar) m += ',';
            firstVar = false;
            m += str(key) + ":{";
            bool firstUnit = true;
            for (const QString &u : units_)
            {
                if (!firstUnit) m += ',';
                firstUnit = false;
                const int c = channelIndex_.value(u + "|" + key, -1);
                if (c < 0) { m += str(u) + ":null"; continue; }
                const std::vector<double> y = full(c);
                std::vector<double> ym;
                for (size_t k : mi) ym.push_back(y[k]);
                m += str(u) + ":" + numbers(ym);
            }
            m += "}";
        }
    m += "}}";
    if (!saveText(outDir + "/map_state.json", m, err)) return false;

    // gages/<id>.json: model series and the observations up to now
    const double tFirst = T.empty() ? now : T.front();
    for (const QJsonValue &gv : vc_.value("gages").toArray())
    {
        const QJsonObject g = gv.toObject();
        const QString id = g.value("id").toString();
        QByteArray b = "{\"id\":" + str(id) + ",\"name\":" + str(g.value("name").toString()) + ",\"now\":" + nowB +
                       ",\"flood_stage_m\":" + (g.contains("flood_stage_m") ? QByteArray::number(g.value("flood_stage_m").toDouble()) : QByteArray("null")) +
                       ",\"series\":{";
        bool first = true;
        for (const QJsonValue &sv : g.value("series").toArray())
        {
            const QJsonObject s = sv.toObject();
            const QString sid = s.value("id").toString();
            const int c = channelIndex_.value("gage|" + id + "|" + sid, -1);
            if (c >= 0)
            {
                if (!first) b += ',';
                first = false;
                b += str(sid + "_model") + ":{\"label\":" + str(s.value("label").toString() + ", model") +
                     ",\"unit\":" + str(s.value("unit").toString()) + ",\"t\":" + numbers(Tr, 10) +
                     ",\"v\":" + numbers(full(c)) + "}";
            }
            const QString obs = s.value("observed").toString().replace("{gage}", id);
            if (obs.isEmpty() || cfg_.observationsDir.empty()) continue;
            QFile of(QDir(QString::fromStdString(cfg_.observationsDir)).absoluteFilePath(obs));
            if (!of.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
            std::vector<double> ot, ov;
            QTextStream in(&of);
            while (!in.atEnd())
            {
                const QStringList p = in.readLine().split(',');
                bool a = false, bb = false;
                if (p.size() < 2) continue;
                const double tt = p[0].toDouble(&a), vv = p[1].toDouble(&bb);
                if (a && bb && tt >= tFirst && tt <= now + 1e-6) { ot.push_back(std::round(tt * 1e5) / 1e5); ov.push_back(vv); }
            }
            if (ot.empty()) continue;
            if (!first) b += ',';
            first = false;
            b += str(sid + "_obs") + ":{\"label\":" + str(s.value("label").toString() + ", USGS") + ",\"unit\":" +
                 str(s.value("unit").toString()) + ",\"t\":" + numbers(ot, 10) + ",\"v\":" + numbers(ov) + "}";
        }
        b += "}}";
        if (!saveText(outDir + "/gages/" + id + ".json", b, err)) return false;
    }

    // status.json last: the viewer reloads when it changes
    QJsonObject st = status;
    st["now"] = now;
    st["now_utc"] = isoUtc(now);
    st["forecast_end"] = tf.empty() ? now : tf.back();
    st["issued_utc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    if (!st.contains("stale")) st["stale"] = false;
    return saveText(outDir + "/status.json", QJsonDocument(st).toJson(QJsonDocument::Indented), err);
}
