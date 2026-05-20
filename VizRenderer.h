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

#ifndef VIZRENDERER_H
#define VIZRENDERER_H

#include <QJsonObject>
#include <QString>

// ============================================================
// VizRenderer
//
// Renders an SVG visualization from a viz-spec JSON file and a
// state snapshot. All rendering logic is self-contained; this
// class exposes a single static entry point.
//
// Usage:
//     QString err;
//     if (!VizRenderer::render("viz.json", stateObj, "out.svg", err)) {
//         qWarning() << "Render failed:" << err;
//     }
// ============================================================
class VizRenderer
{
public:
    // Reads a viz spec from `vizJsonPath`, binds it against the
    // provided `state` object, and writes an SVG to `svgOutputPath`.
    //
    // Returns true on success. On failure, returns false and sets
    // `err` to a human-readable description.
    static bool render(const QString     &vizJsonPath,
                       const QJsonObject &state,
                       const QString     &svgOutputPath,
                       QString           &err);
};

#endif // VIZRENDERER_H
