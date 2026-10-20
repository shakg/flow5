/****************************************************************************

    flow5 application
    Copyright © 2025 André Deperrois

    This file is part of flow5.

    flow5 is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License,
    or (at your option) any later version.

    flow5 is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty
    of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
    See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with flow5.
    If not, see <https://www.gnu.org/licenses/>.


*****************************************************************************/

#pragma once

/**
 * @file Registration of the flow5 tools, resources and prompts,
 * and the helpers shared between them.
 * All values exchanged with the client are in I.S. units, angles in degrees.
 */

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <analysisrange.h>
#include <enums_objects.h>

#include "../mcp/mcpserver.h"

class Foil;
class Polar;
class OpPoint;
class Plane;
class PlanePolar;
class PlaneOpp;

namespace flow5mcp
{
    void registerProjectTools(mcp::Server &server);
    void registerFoilTools(mcp::Server &server);
    void registerPlaneTools(mcp::Server &server);
    void registerResources(mcp::Server &server);
    void registerPrompts(mcp::Server &server);

    /** Object lookups; throw mcp::ToolError with the list of valid names if not found */
    Foil       *findFoil(QString const &name);
    Polar      *findPolar(Foil const *pFoil, QString const &name);
    Plane      *findPlane(QString const &name);
    PlanePolar *findPlanePolar(Plane const *pPlane, QString const &name);

    QJsonObject foilJson(Foil const *pFoil, bool bCoordinates);
    QJsonObject polarJson(Polar const *pPolar, bool bData);
    QJsonObject opPointJson(OpPoint const *pOpp, bool bDistributions);
    QJsonObject planeJson(Plane const *pPlane);
    QJsonObject planePolarJson(PlanePolar const *pPolar, bool bData);
    QJsonObject planeOppJson(PlaneOpp const *pPOpp);
    QJsonObject projectJson();

    /** Polar data as CSV text */
    QString polarCsv(Polar const *pPolar);
    QString planePolarCsv(PlanePolar const *pPolar);

    QString polarTypeName(xfl::enumPolarType type);
    xfl::enumPolarType parsePolarType(QString const &name);
    QString analysisMethodName(xfl::enumAnalysisMethod method);
    xfl::enumAnalysisMethod parseAnalysisMethod(QString const &name);

    /** Builds the sorted list of values from an argument which is either
     * "values": [..] or "range": {start, end, step}, or "ranges": [{...}] */
    std::vector<double> parseValues(QJsonObject const &args);

    /** Runs XFoil on the foil for the polar's conditions; stores the operating points if bKeepOpps.
     * Returns the number of operating points created. */
    int runXFoil(Foil *pFoil, Polar *pPolar, std::vector<AnalysisRange> const &ranges, bool bAlpha, bool bKeepOpps,
                 mcp::Context &ctx, std::string &log);

    /** Sets the number of threads used by the 3d panel solver */
    void setMaxThreads(int n);

    /** Clears all flow5 objects */
    void clearProject();
}
