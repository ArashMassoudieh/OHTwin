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

#include "VizRenderer.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTextStream>
#include <QVector>
#include <QtGlobal>
#include <cmath>
#include <limits>

// ============================================================
// Internal data structures
// ============================================================
struct Binding {
    enum class Source { Block, Link, VizSource, Constant };
    Source  source   = Source::Constant;
    QString object;
    QString property;
    double  constant = 0.0;
};

struct Threshold {
    double  value   = 1.0;
    bool    isAbove = false;
    QString color;
};

struct Component {
    QString id, type, label, blockRef;
    double  x = 0, y = 0, w = 100, h = 100;
    bool    layoutOverride = false;
    double  fillMax = 1.0;
    Binding fillBinding;
    Binding intensityBinding;
    QVector<Threshold> thresholds;
    QJsonArray drawCommands;
    double  fillValue    = 0.0;
    double  fillFraction = 0.0;
    QString waterColor   = "#3B82F6";
    bool    missing      = false;
};

struct Connector {
    QString linkName;
    QString fromBlock, toBlock;
    Binding flowBinding;
    double  flowValue = 0.0;
    QJsonArray drawCommands;
    QString attachMode = "bottom-top";   // "auto" | "bottom-top"
    QString style      = "arrow";        // "arrow" | "line" | "orthogonal"
};

// ============================================================
// Forward declarations
// ============================================================
static double     stateVal             (const QJsonObject &state,
                       const QString &category,
                       const QString &name,
                       const QString &prop);
static bool       stateHasBlock        (const QJsonObject &state,
                          const QString &name);
static double     readBinding          (const QJsonObject &state,
                          const Binding &b);
static Binding    parseBinding         (const QJsonValue &v);

static QString    resolveColor         (const Component &c);
static double     resolveNumber        (const QJsonValue &v, const Component &c);
static QString    resolveColorAttr     (const QJsonValue &v, const Component &c);
static QString    resolveText          (const QJsonValue &v, const Component &c);

static double     cx                   (double nx, const Component &c);
static double     cy                   (double ny, const Component &c);
static QString    svgFill              (const QString &v);
static QString    svgStroke            (const QString &v, double w);

static QString    executeDraw          (const QJsonObject &cmd,
                           const Component &c,
                           int clipId);
static QString    executeConnectorDraw (const QJsonObject &cmd,
                                    double x1, double y1,
                                    double x2, double y2,
                                    double flowValue);
static void       computeAttachPoints  (const Component &from,
                                const Component &to,
                                const QString &mode,
                                double &x1, double &y1,
                                double &x2, double &y2);
static QString    emitStyledConnector  (const QString &style,
                                   double x1, double y1,
                                   double x2, double y2);

static bool       isNumericBindingText (const QJsonObject &cmd);

static QString    substituteTokens     (const QString &in,
                                const QMap<QString,int> &counters);
static bool       resolveCounterNumeric(const QJsonObject &o,
                                  const QMap<QString,int> &counters,
                                  const QMap<QString,int> &counterStarts,
                                  const QMap<QString,int> &counterLengths,
                                  double &outValue,
                                  QString &err);
static QJsonValue substituteCounters   (const QJsonValue &in,
                                     const QMap<QString,int> &counters,
                                     const QMap<QString,int> &counterStarts,
                                     const QMap<QString,int> &counterLengths,
                                     bool &ok,
                                     QString &err);
static QJsonArray expandRepeatArray    (const QJsonObject &vroot,
                                    const QString &arrayName,
                                    const QJsonArray &seed,
                                    bool &ok,
                                    QString &err);
static QJsonArray expandRepeatBlocks   (const QJsonObject &vroot,
                                     bool &ok,
                                     QString &err);
static QJsonArray expandRepeatConnectors(const QJsonObject &vroot,
                                         bool &ok,
                                         QString &err);

// ============================================================
// State accessors
// ============================================================
static double stateVal(const QJsonObject &state,
                       const QString &category,
                       const QString &name,
                       const QString &prop)
{
    return state[category].toObject()[name].toObject()
    ["variables"].toObject()["variables"].toObject()
        [prop].toObject()["_val"].toDouble();
}

static bool stateHasBlock(const QJsonObject &state, const QString &name)
{
    return state["blocks"].toObject().contains(name);
}

static double readBinding(const QJsonObject &state, const Binding &b)
{
    switch (b.source) {
    case Binding::Source::Block:
        return stateVal(state, "blocks",  b.object, b.property);
    case Binding::Source::Link:
        return stateVal(state, "links",   b.object, b.property);
    case Binding::Source::VizSource:
        return stateVal(state, "sources", b.object, b.property);
    case Binding::Source::Constant:
        return b.constant;
    }
    return 0.0;
}

static Binding parseBinding(const QJsonValue &v)
{
    Binding b;
    if (v.isDouble()) {
        b.source   = Binding::Source::Constant;
        b.constant = v.toDouble();
        return b;
    }
    const QJsonObject o = v.toObject();
    if (o.contains("block")) {
        b.source   = Binding::Source::Block;
        b.object   = o["block"].toString();
        b.property = o["property"].toString();
    } else if (o.contains("link")) {
        b.source   = Binding::Source::Link;
        b.object   = o["link"].toString();
        b.property = o["property"].toString();
    } else if (o.contains("source")) {
        b.source   = Binding::Source::VizSource;
        b.object   = o["source"].toString();
        b.property = o["property"].toString();
    }
    return b;
}

// ============================================================
// Color / value / text resolution
// ============================================================
static QString resolveColor(const Component &c)
{
    if (c.thresholds.isEmpty()) return "#3B82F6";
    for (const auto &t : c.thresholds) {
        if (!t.isAbove && c.fillFraction <  t.value) return t.color;
        if ( t.isAbove && c.fillFraction >= t.value) return t.color;
    }
    return c.thresholds.last().color;
}

static double resolveNumber(const QJsonValue &v, const Component &c)
{
    if (v.isDouble()) return v.toDouble();
    if (!v.isObject()) return 0.0;
    const QJsonObject o = v.toObject();

    if (o.contains("bind")) {
        const QString name = o["bind"].toString();
        if (name == "fill_fraction") return c.fillFraction;
        if (name == "fill_value")    return c.fillValue;
        if (name == "intensity")     return c.fillFraction;
        return 0.0;
    }

    if (o.contains("expr")) {
        QString expr = o["expr"].toString();
        expr.replace("fill_fraction", QString::number(c.fillFraction));
        expr.replace("fill_value",    QString::number(c.fillValue));
        expr.replace("intensity",     QString::number(c.fillFraction));

        for (const QChar op : {'+', '-', '*', '/'}) {
            const int idx = expr.indexOf(op);
            if (idx > 0) {
                const double lhs = expr.left(idx).trimmed().toDouble();
                const double rhs = expr.mid(idx+1).trimmed().toDouble();
                if (op == '+') return lhs + rhs;
                if (op == '-') return lhs - rhs;
                if (op == '*') return lhs * rhs;
                if (op == '/') return rhs != 0 ? lhs / rhs : 0.0;
            }
        }
        return expr.trimmed().toDouble();
    }
    return 0.0;
}

static QString resolveColorAttr(const QJsonValue &v, const Component &c)
{
    if (v.isString()) return v.toString();
    if (v.isObject()) {
        const QString name = v.toObject()["bind"].toString();
        if (name == "water_color") return c.waterColor;
    }
    return "#000000";
}

static QString resolveText(const QJsonValue &v, const Component &c)
{
    if (v.isString()) return v.toString();
    if (!v.isObject()) return {};
    const QJsonObject o = v.toObject();

    if (o.contains("bind")) {
        const QString name = o["bind"].toString();
        if (name == "label")         return c.label;
        if (name == "fill_value")    return QString::number(c.fillValue);
        if (name == "fill_fraction") return QString::number(c.fillFraction);
    }

    if (o.contains("expr")) {
        const double val = resolveNumber(v, c);
        const QString fmt = o.value("format").toString("%.2f");
        char buf[64];
        std::snprintf(buf, sizeof(buf), fmt.toStdString().c_str(), val);
        return QString::fromLocal8Bit(buf);
    }
    return {};
}

// ============================================================
// Coordinate / SVG helpers
// ============================================================
static double cx(double nx, const Component &c) { return c.x + nx * c.w; }
static double cy(double ny, const Component &c) { return c.y + ny * c.h; }

static QString svgFill  (const QString &v) { return QString("fill='%1'").arg(v); }
static QString svgStroke(const QString &v, double w) {
    return QString("stroke='%1' stroke-width='%2'").arg(v).arg(w);
}

// ============================================================
// Draw command executor
// ============================================================
static QString executeDraw(const QJsonObject &cmd, const Component &c,
                           int clipId)
{
    const QString shape = cmd["shape"].toString();
    const bool useClip = cmd["clip"].toBool(false);
    const QString clipAttr = useClip
                                 ? QString("clip-path='url(#comp_clip_%1)'").arg(clipId) : "";

    if (shape == "rect") {
        const double x  = cx(resolveNumber(cmd["x"], c), c);
        const double y  = cy(resolveNumber(cmd["y"], c), c);
        const double w  = resolveNumber(cmd["w"], c) * c.w;
        const double h  = resolveNumber(cmd["h"], c) * c.h;
        const double rx = cmd["rx"].toDouble(0);
        const QString fill   = resolveColorAttr(cmd["fill"],   c);
        const QString stroke = resolveColorAttr(cmd.value("stroke"), c);
        const double  sw     = cmd["stroke_width"].toDouble(1.0);

        if (w <= 0 || h <= 0) return {};

        return QString("<rect x='%1' y='%2' width='%3' height='%4' "
                       "rx='%5' %6 %7 %8/>\n")
            .arg(x).arg(y).arg(w).arg(h).arg(rx)
            .arg(svgFill(fill))
            .arg(stroke.isEmpty() ? "stroke='none'" : svgStroke(stroke, sw))
            .arg(clipAttr);
    }

    if (shape == "ellipse") {
        const double ecx = cx(resolveNumber(cmd["cx"], c), c);
        const double ecy = cy(resolveNumber(cmd["cy"], c), c);
        const double rx  = resolveNumber(cmd["rx"], c) * c.w;
        const double ry  = resolveNumber(cmd["ry"], c) * c.h;
        const QString fill   = resolveColorAttr(cmd["fill"],   c);
        const QString stroke = resolveColorAttr(cmd.value("stroke"), c);
        const double  sw     = cmd["stroke_width"].toDouble(1.0);

        return QString("<ellipse cx='%1' cy='%2' rx='%3' ry='%4' %5 %6 %7/>\n")
            .arg(ecx).arg(ecy).arg(rx).arg(ry)
            .arg(svgFill(fill))
            .arg(stroke.isEmpty() ? "stroke='none'" : svgStroke(stroke, sw))
            .arg(clipAttr);
    }

    if (shape == "polygon") {
        const QJsonArray pts = cmd["points"].toArray();
        QString pointsStr;
        for (const auto &pv : pts) {
            const QJsonArray pt = pv.toArray();
            if (pt.size() < 2) continue;
            pointsStr += QString("%1,%2 ")
                             .arg(cx(pt[0].toDouble(), c))
                             .arg(cy(pt[1].toDouble(), c));
        }
        const QString fill   = resolveColorAttr(cmd["fill"],   c);
        const QString stroke = resolveColorAttr(cmd.value("stroke"), c);
        const double  sw     = cmd["stroke_width"].toDouble(1.0);

        return QString("<polygon points='%1' %2 %3 %4/>\n")
            .arg(pointsStr.trimmed())
            .arg(svgFill(fill))
            .arg(stroke.isEmpty() ? "stroke='none'" : svgStroke(stroke, sw))
            .arg(clipAttr);
    }

    if (shape == "line") {
        const double x1 = cx(resolveNumber(cmd["x1"], c), c);
        const double y1 = cy(resolveNumber(cmd["y1"], c), c);
        const double x2 = cx(resolveNumber(cmd["x2"], c), c);
        const double y2 = cy(resolveNumber(cmd["y2"], c), c);
        const QString stroke = resolveColorAttr(cmd["stroke"], c);
        const double  sw     = cmd["stroke_width"].toDouble(1.0);
        const QString dash   = cmd["dash"].toString();
        const QString dashAttr = dash.isEmpty() ? ""
                                                : QString("stroke-dasharray='%1'").arg(dash);

        return QString("<line x1='%1' y1='%2' x2='%3' y2='%4' %5 %6/>\n")
            .arg(x1).arg(y1).arg(x2).arg(y2)
            .arg(svgStroke(stroke, sw))
            .arg(dashAttr);
    }

    if (shape == "path") {
        QString d = cmd["d"].toString();
        const QString fill   = resolveColorAttr(cmd["fill"],   c);
        const QString stroke = resolveColorAttr(cmd.value("stroke"), c);
        const double  sw     = cmd["stroke_width"].toDouble(1.0);

        return QString("<path d='%1' %2 %3 %4/>\n")
            .arg(d)
            .arg(svgFill(fill))
            .arg(stroke.isEmpty() ? "stroke='none'" : svgStroke(stroke, sw))
            .arg(clipAttr);
    }

    if (shape == "text") {
        const double  tx      = cx(resolveNumber(cmd["x"], c), c);
        const double  ty      = cy(resolveNumber(cmd["y"], c), c);
        const QString anchor  = cmd["anchor"].toString("middle");
        const QString val     = resolveText(cmd["value"], c);
        const int     fs      = cmd["font_size"].toInt(11);
        const int     fw      = cmd["font_weight"].toInt(400);
        const QString fill    = resolveColorAttr(cmd["fill"], c);

        return QString("<text x='%1' y='%2' text-anchor='%3' "
                       "font-family='sans-serif' font-size='%4' "
                       "font-weight='%5' fill='%6'>%7</text>\n")
            .arg(tx).arg(ty).arg(anchor)
            .arg(fs).arg(fw).arg(fill).arg(val);
    }

    return {};
}

// ============================================================
// Connector SVG
// ============================================================
static QString executeConnectorDraw(const QJsonObject &cmd,
                                    double x1, double y1,
                                    double x2, double y2,
                                    double flowValue)
{
    const QString shape = cmd["shape"].toString();

    if (shape == "line") {
        const QString stroke = cmd["stroke"].toString("#3B82F6");
        const double  sw     = cmd["stroke_width"].toDouble(2.0);
        const QString dash   = cmd["dash"].toString();
        const QString dashAttr = dash.isEmpty() ? ""
                                                : QString("stroke-dasharray='%1'").arg(dash);

        return QString("<line x1='%1' y1='%2' x2='%3' y2='%4' "
                       "stroke='%5' stroke-width='%6' %7 "
                       "marker-end='url(#arrowhead)'/>\n")
            .arg(x1).arg(y1).arg(x2).arg(y2)
            .arg(stroke).arg(sw).arg(dashAttr);
    }

    if (shape == "text") {
        const double  tx  = (x1 + x2) / 2 + 6;
        const double  ty  = (y1 + y2) / 2;
        const QString fmt = cmd.value("value").toObject()
                                .value("format").toString("%.1f");
        char buf[64];
        std::snprintf(buf, sizeof(buf), fmt.toStdString().c_str(), flowValue);
        const int    fs   = cmd["font_size"].toInt(9);
        const QString fill = cmd["fill"].toString("#475569");

        return QString("<text x='%1' y='%2' text-anchor='middle' "
                       "font-family='sans-serif' font-size='%3' fill='%4'>"
                       "%5</text>\n")
            .arg(tx).arg(ty).arg(fs).arg(fill)
            .arg(QString::fromLocal8Bit(buf));
    }

    return {};
}

// ============================================================
// Connector geometry & styled emitters
// ============================================================

// Compute where a connector attaches to its source and target components.
//
// mode = "bottom-top" (default/legacy):
//   Always use source bottom-center -> target top-center. Matches the
//   original convention for top-to-bottom process flows.
//
// mode = "auto":
//   Pick the pair of facing edges based on target's position relative
//   to source. Determines the dominant axis (horizontal vs. vertical
//   delta between centers) and attaches to the facing edges on that
//   axis. Works naturally for grids in any orientation.
static void computeAttachPoints(const Component &from,
                                const Component &to,
                                const QString &mode,
                                double &x1, double &y1,
                                double &x2, double &y2)
{
    if (mode == "auto") {
        const double fcx = from.x + from.w / 2;
        const double fcy = from.y + from.h / 2;
        const double tcx = to.x   + to.w   / 2;
        const double tcy = to.y   + to.h   / 2;
        const double dx  = tcx - fcx;
        const double dy  = tcy - fcy;

        if (qAbs(dx) >= qAbs(dy)) {
            // Horizontal dominant: attach to left/right edges.
            y1 = fcy;
            y2 = tcy;
            if (dx >= 0) { x1 = from.x + from.w; x2 = to.x; }
            else         { x1 = from.x;          x2 = to.x + to.w; }
        } else {
            // Vertical dominant: attach to top/bottom edges.
            x1 = fcx;
            x2 = tcx;
            if (dy >= 0) { y1 = from.y + from.h; y2 = to.y; }
            else         { y1 = from.y;          y2 = to.y + to.h; }
        }
        return;
    }

    // Default: "bottom-top"
    x1 = from.x + from.w / 2;
    y1 = from.y + from.h;
    x2 = to.x   + to.w   / 2;
    y2 = to.y;
}

// Emit SVG for a styled connector shorthand (when the viz spec uses
// the `style` field instead of an explicit `draw` array).
//   "arrow"      — straight line with arrowhead marker
//   "line"       — straight line, no arrowhead
//   "orthogonal" — L-shaped path with arrowhead, horizontal-then-vertical
static QString emitStyledConnector(const QString &style,
                                   double x1, double y1,
                                   double x2, double y2)
{
    if (style == "line") {
        return QString("<line x1='%1' y1='%2' x2='%3' y2='%4' "
                       "stroke='#3B82F6' stroke-width='2'/>\n")
            .arg(x1).arg(y1).arg(x2).arg(y2);
    }

    if (style == "orthogonal") {
        // Simple L-path: go horizontal first, then vertical.
        // Works cleanly for grid-style layouts in any direction.
        return QString("<path d='M %1 %2 L %3 %2 L %3 %4' "
                       "fill='none' stroke='#3B82F6' stroke-width='2' "
                       "marker-end='url(#arrowhead)'/>\n")
            .arg(x1).arg(y1).arg(x2).arg(y2);
    }

    // Default: "arrow"
    return QString("<line x1='%1' y1='%2' x2='%3' y2='%4' "
                   "stroke='#3B82F6' stroke-width='2' "
                   "marker-end='url(#arrowhead)'/>\n")
        .arg(x1).arg(y1).arg(x2).arg(y2);
}

// ============================================================
// Missing-component classification
// ============================================================
static bool isNumericBindingText(const QJsonObject &cmd)
{
    if (cmd["shape"].toString() != "text") return false;
    const QJsonValue v = cmd["value"];
    if (!v.isObject()) return false;
    const QJsonObject vo = v.toObject();
    if (vo.contains("expr")) return true;
    const QString b = vo["bind"].toString();
    return b == "fill_value" || b == "fill_fraction" || b == "intensity";
}

// ============================================================
// Phase 1+2: repeat-block expansion
// ============================================================

// Substitute ${name}, ${name+N}, and ${name-N} tokens in a string
// using the provided counter values. N must be a non-negative integer
// with no whitespace. Unknown counter names are left intact.
//
// Out-of-iteration arithmetic is allowed: `${j+1}` with j=2 emits "3".
// Whether the resulting reference resolves is checked at render time
// via the missing-block path.
static QString substituteTokens(const QString &in,
                                const QMap<QString,int> &counters)
{
    QString out;
    out.reserve(in.size());

    int i = 0;
    const int n = in.size();
    while (i < n) {
        if (i + 1 >= n || in[i] != '$' || in[i+1] != '{') {
            out.append(in[i]);
            ++i;
            continue;
        }

        const int close = in.indexOf('}', i + 2);
        if (close < 0) {
            out.append(in[i]);
            ++i;
            continue;
        }

        const QString body = in.mid(i + 2, close - (i + 2));

        int opPos = -1;
        QChar opChar;
        for (int k = 0; k < body.size(); ++k) {
            if (body[k] == '+' || body[k] == '-') {
                opPos = k;
                opChar = body[k];
                break;
            }
        }

        QString name;
        int     offset = 0;
        bool    parsedOk = true;

        if (opPos < 0) {
            name = body;
        } else {
            name = body.left(opPos);
            bool convOk = false;
            const int N = body.mid(opPos + 1).toInt(&convOk);
            if (!convOk || N < 0) parsedOk = false;
            else offset = (opChar == '+') ? N : -N;
        }

        if (!parsedOk || !counters.contains(name)) {
            out.append(in.mid(i, close - i + 1));
            i = close + 1;
            continue;
        }

        out.append(QString::number(counters[name] + offset));
        i = close + 1;
    }

    return out;
}

static bool resolveCounterNumeric(const QJsonObject &o,
                                  const QMap<QString,int> &counters,
                                  const QMap<QString,int> &counterStarts,
                                  const QMap<QString,int> &counterLengths,
                                  double &outValue,
                                  QString &err)
{
    const QString name = o["counter"].toString();
    if (!counters.contains(name)) {
        err = "Unknown counter reference: " + name;
        return false;
    }
    // i is the zero-based iteration index within the counter's declared
    // range, NOT the raw counter value. This keeps `range` semantics
    // ("first at a, last at b") invariant under shifts of the declared
    // start, e.g. [0,2] and [1,3] both produce first=a, last=b.
    const int raw   = counters[name];
    const int start = counterStarts.value(name, 0);
    const int i     = raw - start;
    const int n     = counterLengths.value(name, 1);

    if (o.contains("step")) {
        const double step   = o["step"].toDouble();
        const double offset = o["offset"].toDouble(0.0);
        outValue = i * step + offset;
        return true;
    }

    if (o.contains("range")) {
        const QJsonArray r = o["range"].toArray();
        if (r.size() != 2) {
            err = "Counter 'range' must have exactly two numbers";
            return false;
        }
        const double a = r[0].toDouble();
        const double b = r[1].toDouble();
        outValue = (n <= 1) ? a : a + i * (b - a) / (n - 1);
        return true;
    }

    err = "Counter object needs 'step' or 'range'";
    return false;
}

static QJsonValue substituteCounters(const QJsonValue &in,
                                     const QMap<QString,int> &counters,
                                     const QMap<QString,int> &counterStarts,
                                     const QMap<QString,int> &counterLengths,
                                     bool &ok,
                                     QString &err)
{
    if (!ok) return in;

    if (in.isString()) {
        return QJsonValue(substituteTokens(in.toString(), counters));
    }

    if (in.isObject()) {
        const QJsonObject o = in.toObject();
        if (o.contains("counter")) {
            double v = 0.0;
            if (!resolveCounterNumeric(o, counters, counterStarts,
                                       counterLengths, v, err)) {
                ok = false;
                return in;
            }
            return QJsonValue(v);
        }
        QJsonObject out;
        for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
            out.insert(it.key(),
                       substituteCounters(it.value(), counters, counterStarts,
                                          counterLengths, ok, err));
        }
        return out;
    }

    if (in.isArray()) {
        const QJsonArray a = in.toArray();
        QJsonArray out;
        for (const auto &v : a) {
            out.append(substituteCounters(v, counters, counterStarts,
                                          counterLengths, ok, err));
        }
        return out;
    }

    return in;
}

// Shared Cartesian-product expansion driver, used for both components
// and connectors. Reads vroot[arrayName] as an array of repeat blocks,
// iterates each block's counter product, and appends each substituted
// template to `seed`. Returns the merged array.
static QJsonArray expandRepeatArray(const QJsonObject &vroot,
                                    const QString &arrayName,
                                    const QJsonArray &seed,
                                    bool &ok,
                                    QString &err)
{
    QJsonArray out = seed;

    const QJsonArray repeats = vroot[arrayName].toArray();
    for (int rIdx = 0; rIdx < repeats.size(); ++rIdx) {
        const QJsonObject rblock = repeats[rIdx].toObject();
        const QJsonObject ctrDefs = rblock["counters"].toObject();
        const QJsonValue  templateVal = rblock["template"];

        if (ctrDefs.isEmpty() || !templateVal.isObject()) {
            err = QString("%1[%2]: needs non-empty 'counters' and "
                          "an object 'template'").arg(arrayName).arg(rIdx);
            ok = false;
            return out;
        }

        QVector<QString>  names;
        QVector<int>      starts;
        QVector<int>      lengths;
        QMap<QString,int> counterStarts;
        QMap<QString,int> counterLengths;
        for (auto it = ctrDefs.constBegin(); it != ctrDefs.constEnd(); ++it) {
            const QJsonObject def = it.value().toObject();
            const QJsonArray  r   = def["range"].toArray();
            if (r.size() != 2) {
                err = QString("%1[%2].counters.%3: 'range' must have "
                              "exactly two numbers")
                          .arg(arrayName).arg(rIdx).arg(it.key());
                ok = false;
                return out;
            }
            const int a = r[0].toInt();
            const int b = r[1].toInt();
            if (b < a) {
                err = QString("%1[%2].counters.%3: range end < start")
                .arg(arrayName).arg(rIdx).arg(it.key());
                ok = false;
                return out;
            }
            names.append(it.key());
            starts.append(a);
            lengths.append(b - a + 1);
            counterStarts.insert(it.key(), a);
            counterLengths.insert(it.key(), b - a + 1);
        }

        long long total = 1;
        for (int len : lengths) total *= len;

        QVector<int> idx(names.size(), 0);
        for (long long k = 0; k < total; ++k) {
            QMap<QString,int> values;
            for (int d = 0; d < names.size(); ++d)
                values.insert(names[d], starts[d] + idx[d]);

            const QJsonValue expanded =
                substituteCounters(templateVal, values, counterStarts,
                                   counterLengths, ok, err);
            if (!ok) {
                err = QString("%1[%2]: %3")
                .arg(arrayName).arg(rIdx).arg(err);
                return out;
            }
            out.append(expanded);

            for (int d = names.size() - 1; d >= 0; --d) {
                if (++idx[d] < lengths[d]) break;
                idx[d] = 0;
            }
        }
    }

    return out;
}

// components = vroot["components"] + expansions from "repeat" (legacy)
// and/or "repeat_components".
static QJsonArray expandRepeatBlocks(const QJsonObject &vroot,
                                     bool &ok,
                                     QString &err)
{
    QJsonArray out = vroot["components"].toArray();

    if (vroot.contains("repeat")) {
        out = expandRepeatArray(vroot, "repeat", out, ok, err);
        if (!ok) return out;
    }
    if (vroot.contains("repeat_components")) {
        out = expandRepeatArray(vroot, "repeat_components", out, ok, err);
    }
    return out;
}

// connectors = vroot["connectors"] + expansions from "repeat_connectors"
static QJsonArray expandRepeatConnectors(const QJsonObject &vroot,
                                         bool &ok,
                                         QString &err)
{
    QJsonArray seed = vroot["connectors"].toArray();
    if (!vroot.contains("repeat_connectors")) return seed;
    return expandRepeatArray(vroot, "repeat_connectors", seed, ok, err);
}

// ============================================================
// VizRenderer::render
// ============================================================
bool VizRenderer::render(const QString     &vizJsonPath,
                         const QJsonObject &state,
                         const QString     &svgOutputPath,
                         QString           &err)
{
    QFile vf(vizJsonPath);
    if (!vf.open(QIODevice::ReadOnly)) {
        err = "Cannot open viz.json: " + vizJsonPath;
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument vdoc = QJsonDocument::fromJson(vf.readAll(), &pe);
    if (vdoc.isNull()) {
        err = "viz.json parse error: " + pe.errorString();
        return false;
    }
    const QJsonObject vroot = vdoc.object();

    const QJsonObject canvas = vroot["canvas"].toObject();
    const double cx_viewport = canvas["x"].toDouble(0.0);
    const double cy_viewport = canvas["y"].toDouble(0.0);
    const double cw          = canvas["width"].toDouble(420);
    const double ch          = canvas["height"].toDouble(560);

    // --- expand components (hand-placed + repeat_components + legacy repeat) ---
    bool             expandOk = true;
    const QJsonArray allComponents =
        expandRepeatBlocks(vroot, expandOk, err);
    if (!expandOk) return false;

    // --- parse components ---
    QVector<Component> components;
    for (const auto &cv : allComponents) {
        const QJsonObject co = cv.toObject();
        Component c;
        c.id           = co["id"].toString();
        c.type         = co["type"].toString();
        c.label        = co["label"].toString(c.id);
        c.blockRef     = co["block"].toString();
        c.fillMax      = co["fill_max"].toDouble(1.0);
        c.drawCommands = co["draw"].toArray();

        if (co.contains("x") && co.contains("y")) {
            c.x = co["x"].toDouble();
            c.y = co["y"].toDouble();
            c.w = co["width"].toDouble(100);
            c.h = co["height"].toDouble(100);
            c.layoutOverride = true;
        }

        const QJsonObject bind = co["bind"].toObject();
        if (bind.contains("fill_value"))
            c.fillBinding = parseBinding(bind["fill_value"]);
        if (bind.contains("intensity"))
            c.intensityBinding = parseBinding(bind["intensity"]);

        for (const auto &tv : co["thresholds"].toArray()) {
            const QJsonObject to = tv.toObject();
            Threshold t;
            t.isAbove = to.contains("above");
            t.value   = t.isAbove ? to["above"].toDouble()
                                : to["below"].toDouble(1.0);
            t.color   = to["color"].toString("#3B82F6");
            c.thresholds.append(t);
        }
        components.append(c);
    }

    // --- expand connectors ---
    const QJsonArray allConnectors =
        expandRepeatConnectors(vroot, expandOk, err);
    if (!expandOk) return false;

    // --- parse connectors ---
    QVector<Connector> connectors;
    const QJsonObject stateLinks = state["links"].toObject();
    for (const auto &cv : allConnectors) {
        const QJsonObject co = cv.toObject();
        Connector cn;
        cn.linkName     = co["link"].toString();
        cn.drawCommands = co["draw"].toArray();

        // Prefer explicit from/to in the viz spec (needed for grid
        // connectors). Fall back to state.links for hand-placed
        // connectors that describe topology via the simulation.
        if (co.contains("from") && co.contains("to")) {
            cn.fromBlock = co["from"].toString();
            cn.toBlock   = co["to"].toString();
        } else {
            const QJsonObject linkObj = stateLinks[cn.linkName].toObject();
            cn.fromBlock = linkObj["s_Block_name"].toString();
            cn.toBlock   = linkObj["e_Block_name"].toString();
        }

        // Attach and style shorthands. Both optional; defaults preserve
        // legacy behavior (bottom-to-top geometry, straight arrow line).
        cn.attachMode = co.value("attach").toString("bottom-top");
        cn.style      = co.value("style").toString("arrow");

        const QJsonObject bind = co["bind"].toObject();
        if (bind.contains("flow_value"))
            cn.flowBinding = parseBinding(bind["flow_value"]);
        connectors.append(cn);
    }

    // --- resolve layout from state (or flag missing) ---
    {
        double xMin =  std::numeric_limits<double>::infinity();
        double yMin =  std::numeric_limits<double>::infinity();
        double xMax = -std::numeric_limits<double>::infinity();
        double yMax = -std::numeric_limits<double>::infinity();

        for (auto &c : components) {
            const bool hasBlock = !c.blockRef.isEmpty()
            && stateHasBlock(state, c.blockRef);

            if (!hasBlock && !c.blockRef.isEmpty()) {
                c.missing = true;
                qWarning("VizRenderer: block '%s' referenced by component "
                         "'%s' not found in state",
                         qPrintable(c.blockRef), qPrintable(c.id));
            }

            if (c.layoutOverride) continue;

            if (hasBlock) {
                c.x = stateVal(state, "blocks", c.blockRef, "x");
                c.y = stateVal(state, "blocks", c.blockRef, "y");
                c.w = stateVal(state, "blocks", c.blockRef, "_width");
                c.h = stateVal(state, "blocks", c.blockRef, "_height");
                if (c.w <= 0) c.w = 100;
                if (c.h <= 0) c.h = 100;
                xMin = qMin(xMin, c.x);
                yMin = qMin(yMin, c.y);
                xMax = qMax(xMax, c.x + c.w);
                yMax = qMax(yMax, c.y + c.h);
            } else {
                c.x = 10; c.y = 10; c.w = 80; c.h = 80;
            }
        }

        if (xMax > xMin && yMax > yMin) {
            const double pad    = 30.0;
            const double scaleX = (cw - 2*pad) / (xMax - xMin);
            const double scaleY = (ch - 2*pad) / (yMax - yMin);
            const double scale  = qMin(scaleX, scaleY);
            for (auto &c : components) {
                if (c.layoutOverride || c.missing) continue;
                c.x = pad + (c.x - xMin) * scale;
                c.y = pad + (c.y - yMin) * scale;
                c.w = c.w * scale;
                c.h = c.h * scale;
            }
        }
    }

    // --- resolve bindings ---
    for (auto &c : components) {
        if (c.missing) {
            c.fillValue    = 0.0;
            c.fillFraction = 0.0;
            c.waterColor   = "#E5E7EB";
            continue;
        }
        c.fillValue    = readBinding(state, c.fillBinding);
        c.fillFraction = (c.fillMax > 0)
                             ? qBound(0.0, c.fillValue / c.fillMax, 1.0)
                             : 0.0;
        c.waterColor   = resolveColor(c);
    }
    for (auto &cn : connectors)
        cn.flowValue = readBinding(state, cn.flowBinding);

    // --- generate SVG ---
    // Canvas x/y define the viewport's origin in *content* coordinates.
    // The rendered SVG is sized to (width, height) and its viewBox
    // starts at (x, y), so authored coordinates keep their absolute
    // values and SVG does the viewport translation. Default x,y = 0
    // preserves historical behavior.
    QString svg;
    svg += QString("<?xml version='1.0' encoding='UTF-8'?>\n"
                   "<svg xmlns='http://www.w3.org/2000/svg' "
                   "width='%1' height='%2' viewBox='%3 %4 %1 %2'>\n")
               .arg(cw).arg(ch).arg(cx_viewport).arg(cy_viewport);

    svg += "<defs>\n";
    svg += "  <marker id='arrowhead' markerWidth='8' markerHeight='6' "
           "refX='8' refY='3' orient='auto'>\n"
           "    <polygon points='0 0, 8 3, 0 6' fill='#3B82F6'/>\n"
           "  </marker>\n";
    for (int i = 0; i < components.size(); ++i) {
        const auto &c = components[i];
        svg += QString("  <clipPath id='comp_clip_%1'>"
                       "<rect x='%2' y='%3' width='%4' height='%5' rx='8'/>"
                       "</clipPath>\n")
                   .arg(i).arg(c.x).arg(c.y).arg(c.w).arg(c.h);
    }
    svg += "</defs>\n";

    svg += QString("<rect x='%1' y='%2' width='%3' height='%4' "
                   "fill='#F8FAFC'/>\n")
               .arg(cx_viewport).arg(cy_viewport).arg(cw).arg(ch);

    // connectors (behind components). Drawn whenever both endpoints
    // exist as components in the viz spec. Attach geometry and visual
    // style are controlled by the connector's `attach` and `style`
    // fields; defaults preserve legacy bottom-to-top arrow behavior.
    for (const auto &cn : connectors) {
        const Component *from = nullptr;
        const Component *to   = nullptr;
        for (const auto &c : components) {
            if (c.blockRef == cn.fromBlock) from = &c;
            if (c.blockRef == cn.toBlock)   to   = &c;
        }

        if (!from || !to) {
            qWarning("VizRenderer: connector '%s' skipped "
                     "(from='%s' %s, to='%s' %s)",
                     qPrintable(cn.linkName),
                     qPrintable(cn.fromBlock), from ? "ok" : "not in spec",
                     qPrintable(cn.toBlock),   to   ? "ok" : "not in spec");
            continue;
        }

        double x1, y1, x2, y2;
        computeAttachPoints(*from, *to, cn.attachMode, x1, y1, x2, y2);

        const bool liveBoth = !from->missing && !to->missing;

        if (!cn.drawCommands.isEmpty()) {
            // Author-specified draw commands take precedence. Skip
            // numeric text when data isn't live, same as before.
            for (const auto &cmd : cn.drawCommands) {
                const QJsonObject co = cmd.toObject();
                if (!liveBoth && co["shape"].toString() == "text") continue;
                svg += executeConnectorDraw(co, x1, y1, x2, y2, cn.flowValue);
            }
        } else {
            // Use the `style` shorthand.
            svg += emitStyledConnector(cn.style, x1, y1, x2, y2);
        }
    }

    // components
    for (int i = 0; i < components.size(); ++i) {
        const auto &c = components[i];
        for (const auto &cmd : c.drawCommands) {
            const QJsonObject co = cmd.toObject();
            if (c.missing && isNumericBindingText(co)) continue;
            svg += executeDraw(co, c, i);
        }
    }

    svg += "</svg>\n";

    // --- write output ---
    QFile out(svgOutputPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        err = "Cannot write SVG: " + svgOutputPath;
        return false;
    }
    QTextStream ts(&out);
    ts << svg;
    return true;
}
