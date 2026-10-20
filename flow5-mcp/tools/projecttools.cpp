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

#include <filesystem>
#include <thread>

#include <api.h>
#include <fl5core.h>
#include <flow5-io.h>
#include <llttask.h>
#include <panelanalysis.h>
#include <planetask.h>
#include <xfoiltask.h>

#include "flow5tools.h"

using namespace mcp;


namespace flow5mcp
{

namespace
{
    int s_MaxThreads = 1; /**< mirrors PanelAnalysis::s_MaxThreads which has no public getter */
}


void setMaxThreads(int n)
{
    s_MaxThreads = std::max(1, n);
    PanelAnalysis::setMaxThreadCount(s_MaxThreads);
}


void registerProjectTools(Server &server)
{
    {
        Tool t;
        t.name = "flow5_info";
        t.title = "flow5 information";
        t.description = "Returns the flow5 version, the conventions used by this server (units, polar types, analysis methods) "
                        "and the current solver settings. Call this first to learn how to use the other tools.";
        t.inputSchema = json(R"J({"type":"object","properties":{}})J");
        t.annotations = json(R"J({"readOnlyHint":true,"openWorldHint":false})J");
        t.handler = [](QJsonObject const &, Context &) -> ToolResult
        {
            QJsonObject o;
            o["version"] = qs(fl5::versionName(true));
            o["units"] = "I.S. units for all inputs and outputs: m, kg, m/s, kg/m3, m2/s; angles in degrees";
            o["foil_polar_types"] = json(R"J({
                "T1":"fixed speed: Reynolds and Mach fixed",
                "T2":"fixed lift: Re*sqrt(Cl) and Ma*sqrt(Cl) fixed",
                "T3":"rubber chord: Re*Cl fixed",
                "T4":"fixed angle of attack; the sequence variable is the Reynolds number",
                "T6":"control polar: the sequence variable is the trailing edge flap angle"})J");
            o["plane_polar_types"] = json(R"J({
                "T1":"fixed speed; sequence variable: alpha",
                "T2":"fixed lift, speed adjusted to balance the weight; sequence variable: alpha",
                "T3":"glide polar, speed and glide angle balanced; sequence variable: alpha",
                "T5":"fixed speed and alpha; sequence variable: sideslip angle beta",
                "T6":"control polar; sequence variable: control parameter",
                "T7":"stability polar; sequence variable: control parameter; returns stability derivatives and eigenvalues"})J");
            o["analysis_methods"] = json(R"J({
                "LLT":"non-linear lifting line, main wing only, viscous by design",
                "VLM1":"horseshoe vortex lattice",
                "VLM2":"ring vortex lattice",
                "QUADS":"quad panels",
                "TRIUNIFORM":"triangular panels with uniform densities (default, most robust)",
                "TRILINEAR":"triangular panels with linear densities"})J");
            o["solver_settings"] = QJsonObject{
                {"max_threads", s_MaxThreads},
                {"xfoil_max_iterations", XFoilTask::maxIterations()},
                {"llt_max_iterations", LLTTask::maxIter()},
                {"visc_loop_max_iterations", PlaneTask::maxViscIter()}};
            o["hardware_threads"] = int(std::thread::hardware_concurrency());
            return {o, QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "project_summary";
        t.title = "Project summary";
        t.description = "Lists all foils with their polars and all planes with their polars currently loaded in memory.";
        t.inputSchema = json(R"J({"type":"object","properties":{}})J");
        t.annotations = json(R"J({"readOnlyHint":true,"openWorldHint":false})J");
        t.handler = [](QJsonObject const &, Context &) -> ToolResult
        {
            return {projectJson(), QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "project_load";
        t.title = "Load project";
        t.description = "Loads a flow5 project file (.fl5). By default the current objects are cleared first.";
        t.inputSchema = json(R"J({"type":"object","properties":{
            "path":{"type":"string","description":"Absolute path to the .fl5 file"},
            "keep_existing":{"type":"boolean","default":false,"description":"If true, the loaded objects are added to the current ones"}},
            "required":["path"]})J");
        t.annotations = json(R"J({"destructiveHint":true,"idempotentHint":false,"openWorldHint":false})J");
        t.bMutates = true;
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            QString path = argString(args, "path");
            if(!std::filesystem::exists(path.toStdString())) throw ToolError("File not found: "+path);
            if(!argBool(args, "keep_existing", false)) clearProject();

            std::string log;
            if(!io::loadProject(path.toStdString(), log)) throw ToolError("Failed to load the project: "+qs(log));

            QJsonObject o = projectJson();
            o["loaded"] = path;
            return {o, qs(log)};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "project_save";
        t.title = "Save project";
        t.description = "Saves all the objects in memory to a flow5 project file (.fl5) which can be opened in the flow5 application.";
        t.inputSchema = json(R"J({"type":"object","properties":{
            "path":{"type":"string","description":"Absolute path of the .fl5 file to write"},
            "save_operating_points":{"type":"boolean","default":true,"description":"Also save foil and plane operating points (larger file)"}},
            "required":["path"]})J");
        t.annotations = json(R"J({"destructiveHint":true,"idempotentHint":true,"openWorldHint":false})J");
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            QString path = argString(args, "path");
            if(!path.endsWith(".fl5", Qt::CaseInsensitive)) path += ".fl5";
            bool bOpps = argBool(args, "save_operating_points", true);
            io::setSaveFoilOpps(bOpps);
            io::setSavePlaneOpps(bOpps);

            std::string log;
            if(!io::saveProject(path.toStdString(), log)) throw ToolError("Failed to save the project: "+qs(log));
            return {QJsonObject{{"saved", path}}, QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "project_clear";
        t.title = "Clear project";
        t.description = "Deletes all foils, planes, polars and operating points from memory.";
        t.inputSchema = json(R"J({"type":"object","properties":{}})J");
        t.annotations = json(R"J({"destructiveHint":true,"idempotentHint":true,"openWorldHint":false})J");
        t.bMutates = true;
        t.handler = [](QJsonObject const &, Context &) -> ToolResult
        {
            clearProject();
            return {QJsonObject{{"cleared", true}}, QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "solver_settings";
        t.title = "Solver settings";
        t.description = "Changes the solver settings used by subsequent analyses. Only the provided fields are changed.";
        t.inputSchema = json(R"J({"type":"object","properties":{
            "max_threads":{"type":"integer","minimum":1,"description":"Number of threads used by the 3d panel solver"},
            "xfoil_max_iterations":{"type":"integer","minimum":1,"description":"Max. XFoil viscous iterations per operating point"},
            "llt_max_iterations":{"type":"integer","minimum":1},
            "visc_loop_max_iterations":{"type":"integer","minimum":1,"description":"Max. iterations of the viscous loop in 3d analyses"}}})J");
        t.annotations = json(R"J({"idempotentHint":true,"openWorldHint":false})J");
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            if(args.contains("max_threads"))              setMaxThreads(argInt(args, "max_threads"));
            if(args.contains("xfoil_max_iterations"))     XFoilTask::setMaxIterations(std::max(1, argInt(args, "xfoil_max_iterations")));
            if(args.contains("llt_max_iterations"))       LLTTask::setMaxIter(std::max(1, argInt(args, "llt_max_iterations")));
            if(args.contains("visc_loop_max_iterations")) PlaneTask::setMaxViscIter(std::max(1, argInt(args, "visc_loop_max_iterations")));
            return {QJsonObject{
                {"max_threads", s_MaxThreads},
                {"xfoil_max_iterations", XFoilTask::maxIterations()},
                {"llt_max_iterations", LLTTask::maxIter()},
                {"visc_loop_max_iterations", PlaneTask::maxViscIter()}}, QString()};
        };
        server.addTool(t);
    }
}

} // namespace flow5mcp
