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

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>

#include <QTemporaryDir>

#include <analysisrange.h>
#include <foil.h>
#include <objects2d.h>
#include <objects2d_globals.h>
#include <objects3d.h>
#include <oppoint.h>
#include <polar.h>
#include <polarnamemaker.h>
#include <xfoiltask.h>

#include "flow5tools.h"

using namespace mcp;


namespace
{
    /** Reads a foil from a .dat file; the foil is not stored */
    Foil *readFoil(std::string const &path)
    {
        Foil *pFoil = new Foil;
        int iLineError = -1;
        if(!foil::readFoilFile(path, pFoil, iLineError) || pFoil->nNodes()<3)
        {
            delete pFoil;
            throw ToolError(QString("Failed to read the foil file %1 (error at line %2)").arg(qs(path)).arg(iLineError));
        }
        return pFoil;
    }

    void storeNewFoil(Foil *pFoil, QString const &name, bool bOverwrite)
    {
        if(!name.isEmpty()) pFoil->setName(name.toStdString());
        if(pFoil->name().empty())
        {
            delete pFoil;
            throw ToolError("The foil needs a name");
        }
        if(!bOverwrite && Objects2d::foilExists(pFoil->name()))
        {
            QString n = qs(pFoil->name());
            delete pFoil;
            throw ToolError("A foil named \""+n+"\" already exists; delete it first, choose another name, or set overwrite=true");
        }
        pFoil->setTheStyle({true, Line::SOLID, 2, fl5Color(31, 111, 231), Line::NOSYMBOL});
        Objects2d::insertThisFoil(pFoil); // deletes an existing foil with the same name and its results
    }

    /** Splits an alpha or Cl range which crosses zero into two ranges which start near zero,
     * which is the sequence that converges best with XFoil */
    std::vector<AnalysisRange> xfoilRanges(QJsonObject const &args, bool bSplitAtZero)
    {
        QJsonArray jranges;
        if(args.contains("range"))  jranges.append(args.value("range"));
        if(args.contains("ranges")) for(QJsonValue const &v : argArray(args, "ranges")) jranges.append(v);
        if(jranges.isEmpty()) throw ToolError("Provide \"range\" or \"ranges\" as {start, end, step}");

        std::vector<AnalysisRange> ranges;
        for(QJsonValue const &v : jranges)
        {
            QJsonObject r = v.toObject();
            double start = argDouble(r, "start");
            double end   = argDouble(r, "end", start);
            double step  = std::fabs(argDouble(r, "step", 1.0));
            if(step<1.e-6) step = 1.0;

            double lo = std::min(start, end), hi = std::max(start, end);
            if(bSplitAtZero && lo<-1.e-9 && hi>1.e-9)
            {
                // pivot = grid value closest to zero
                double pivot = lo + std::round((0.0-lo)/step)*step;
                ranges.push_back({true, pivot, hi, step});
                if(pivot-step>=lo-1.e-9) ranges.push_back({true, pivot-step, lo, step});
            }
            else ranges.push_back({true, start, end, step});
        }
        return ranges;
    }

    int foilPolarCount(Foil const *pFoil)
    {
        int n=0;
        for(Polar const *pPolar : Objects2d::polars()) if(pPolar->foilName()==pFoil->name()) n++;
        return n;
    }

    Polar *createPolar(Foil const *pFoil, QJsonObject const &args)
    {
        xfl::enumPolarType type = flow5mcp::parsePolarType(argString(args, "type", "T1"));
        if(type!=xfl::T1POLAR && type!=xfl::T2POLAR && type!=xfl::T3POLAR && type!=xfl::T4POLAR && type!=xfl::T6POLAR)
            throw ToolError("Foil polars must be of type T1, T2, T3, T4 or T6");

        double spec = type==xfl::T4POLAR ? argDouble(args, "alpha", 0.0) : argDouble(args, "reynolds", 100000.0);
        Polar *pPolar = Objects2d::createPolar(pFoil, type, spec,
                                               argDouble(args, "mach", 0.0),
                                               argDouble(args, "ncrit", 9.0),
                                               argDouble(args, "xtr_top", 1.0),
                                               argDouble(args, "xtr_bot", 1.0));
        if(type==xfl::T4POLAR || type==xfl::T6POLAR) pPolar->setReynolds(argDouble(args, "reynolds", 100000.0));
        if(type==xfl::T6POLAR) pPolar->setAoaSpec(argDouble(args, "alpha", 0.0));
        pPolar->setTEFlapAngle(argDouble(args, "te_flap_angle", 0.0));

        QString name = argString(args, "name", QString());
        pPolar->setName(name.isEmpty() ? PolarNameMaker::makeName(pPolar) : name.toStdString());
        if(Objects2d::polar(pFoil, pPolar->name()))
        {
            QString n = qs(pPolar->name());
            delete pPolar;
            throw ToolError("A polar named \""+n+"\" already exists for this foil");
        }
        Objects2d::insertPolar(pPolar);
        return pPolar;
    }

    char const *s_PolarSpecSchema = R"J(
        "type":{"type":"string","enum":["T1","T2","T3","T4","T6"],"default":"T1","description":"Polar type, see flow5_info"},
        "reynolds":{"type":"number","default":100000,"description":"Reynolds number; for T2 this is Re.sqrt(Cl), for T3 Re.Cl"},
        "mach":{"type":"number","default":0},
        "ncrit":{"type":"number","default":9,"description":"Transition criterion e^n; 9 for a standard wind tunnel"},
        "xtr_top":{"type":"number","default":1,"description":"Forced transition location on the upper surface, x/c"},
        "xtr_bot":{"type":"number","default":1,"description":"Forced transition location on the lower surface, x/c"},
        "alpha":{"type":"number","description":"Angle of attack for T4 and T6 polars, degrees"},
        "te_flap_angle":{"type":"number","default":0,"description":"Trailing edge flap deflection in degrees, positive down; the foil must have a flap defined"})J";
}


namespace flow5mcp
{

int runXFoil(Foil *pFoil, Polar *pPolar, std::vector<AnalysisRange> const &ranges, bool bAlpha, bool bKeepOpps, Context &ctx, std::string &log)
{
    if(pFoil->hasTEFlap())
    {
        pFoil->setTEFlapAngle(pPolar->TEFlapAngle());
        pFoil->setFlaps();
    }

    // XFoilTask embeds large XFoil arrays; keep it off the worker thread's stack
    std::unique_ptr<XFoilTask> task = std::make_unique<XFoilTask>();
    if(!task->initialize(*pFoil, pPolar, bKeepOpps)) throw ToolError("XFoil initialization failed for the foil "+qs(pFoil->name()));
    task->setAoAAnalysis(bAlpha);
    task->setAnalysisRanges(ranges);

    {
        CancelHookScope cancel(ctx, []{XFoilTask::setCancelled(true);});
        task->run();
    }
    log += task->log();

    int nOpps = 0;
    for(OpPoint *pOpp : task->operatingPoints())
    {
        Objects2d::insertOpPoint(pOpp);
        nOpps++;
    }
    return nOpps;
}


void registerFoilTools(Server &server)
{
    {
        Tool t;
        t.name = "foil_list";
        t.title = "List foils";
        t.description = "Lists the foils in memory with their main geometric properties.";
        t.inputSchema = json(R"J({"type":"object","properties":{}})J");
        t.annotations = json(R"J({"readOnlyHint":true,"openWorldHint":false})J");
        t.handler = [](QJsonObject const &, Context &) -> ToolResult
        {
            QJsonArray foils;
            for(Foil const *pFoil : Objects2d::foils()) foils.append(foilJson(pFoil, false));
            return {QJsonObject{{"foils", foils}}, QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "foil_create_naca";
        t.title = "Create NACA foil";
        t.description = "Creates a NACA 4-digit or 5-digit foil, e.g. \"2412\", \"0009\" or \"23012\".";
        t.inputSchema = json(R"J({"type":"object","properties":{
            "designation":{"type":"string","pattern":"^[0-9]{4,5}$","description":"NACA digits"},
            "name":{"type":"string","description":"Foil name; defaults to \"NACA <digits>\""},
            "panels":{"type":"integer","minimum":20,"maximum":500,"default":200,"description":"Number of panels"},
            "overwrite":{"type":"boolean","default":false}},
            "required":["designation"]})J");
        t.annotations = json(R"J({"openWorldHint":false})J");
        t.bMutates = true;
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            QString digits = args.value("designation").isDouble() ? QString::number(args.value("designation").toInt()).rightJustified(4, '0')
                                                                  : argString(args, "designation");
            static QRegularExpression const rx("^[0-9]{4,5}$");
            if(!rx.match(digits).hasMatch()) throw ToolError("Invalid NACA designation: "+digits);

            Foil *pFoil = new Foil;
            if(!Objects2d::makeNacaFoil(pFoil, digits.toInt(), argInt(args, "panels", 200)))
            {
                delete pFoil;
                throw ToolError("Unsupported NACA designation: "+digits);
            }
            storeNewFoil(pFoil, argString(args, "name", "NACA "+digits), argBool(args, "overwrite", false));
            return {foilJson(pFoil, false), QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "foil_import_file";
        t.title = "Import foil file";
        t.description = "Imports a foil from a coordinate file in Selig or Lednicer .dat format.";
        t.inputSchema = json(R"J({"type":"object","properties":{
            "path":{"type":"string","description":"Absolute path to the .dat file"},
            "name":{"type":"string","description":"Foil name; defaults to the name in the file"},
            "overwrite":{"type":"boolean","default":false}},
            "required":["path"]})J");
        t.annotations = json(R"J({"openWorldHint":false})J");
        t.bMutates = true;
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            QString path = argString(args, "path");
            if(!std::filesystem::exists(path.toStdString())) throw ToolError("File not found: "+path);
            Foil *pFoil = readFoil(path.toStdString());
            storeNewFoil(pFoil, argString(args, "name", QString()), argBool(args, "overwrite", false));
            return {foilJson(pFoil, false), QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "foil_create_from_coordinates";
        t.title = "Create foil from coordinates";
        t.description = "Creates a foil from a list of (x, y) points in Selig order: "
                        "from the trailing edge along the upper surface to the leading edge, then along the lower surface back to the trailing edge.";
        t.inputSchema = json(R"J({"type":"object","properties":{
            "name":{"type":"string"},
            "coordinates":{"type":"array","minItems":10,"items":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2}},
            "overwrite":{"type":"boolean","default":false}},
            "required":["name","coordinates"]})J");
        t.annotations = json(R"J({"openWorldHint":false})J");
        t.bMutates = true;
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            QString name = argString(args, "name");
            QJsonArray pts = argArray(args, "coordinates");

            // go through a temporary .dat file to use the same parsing and orientation logic as file imports
            QTemporaryDir dir;
            if(!dir.isValid()) throw ToolError("Could not create a temporary directory");
            std::string path = dir.filePath("foil.dat").toStdString();
            {
                std::ofstream out(path);
                out << name.toStdString() << "\n";
                for(QJsonValue const &v : pts)
                {
                    QJsonArray p = v.toArray();
                    if(p.size()!=2 || !p.at(0).isDouble() || !p.at(1).isDouble()) throw ToolError("Each coordinate must be a pair of numbers [x, y]");
                    out << p.at(0).toDouble() << " " << p.at(1).toDouble() << "\n";
                }
            }
            Foil *pFoil = readFoil(path);
            storeNewFoil(pFoil, name, argBool(args, "overwrite", false));
            return {foilJson(pFoil, false), QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "foil_get";
        t.title = "Get foil";
        t.description = "Returns the properties of a foil, optionally with its coordinates.";
        t.inputSchema = json(R"J({"type":"object","properties":{
            "name":{"type":"string"},
            "include_coordinates":{"type":"boolean","default":false}},
            "required":["name"]})J");
        t.annotations = json(R"J({"readOnlyHint":true,"openWorldHint":false})J");
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            Foil *pFoil = findFoil(argString(args, "name"));
            return {foilJson(pFoil, argBool(args, "include_coordinates", false)), qs(pFoil->properties(true))};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "foil_modify";
        t.title = "Modify foil";
        t.description = "Modifies a foil's geometry: thickness and camber scaling, de-rotation, normalization, re-paneling, trailing edge flap hinge. "
                        "Operations are applied in that order. Modifying the geometry in place deletes the foil's polars and operating points "
                        "since they become invalid; use copy_to to keep the original foil and create a modified copy instead.";
        t.inputSchema = json(R"J({"type":"object","properties":{
            "name":{"type":"string"},
            "copy_to":{"type":"string","description":"Name of a new foil to create with the modifications; the original is left unchanged"},
            "thickness":{"type":"number","description":"New max. thickness, fraction of chord, e.g. 0.12"},
            "x_thickness":{"type":"number","description":"New max. thickness location, fraction of chord"},
            "camber":{"type":"number","description":"New max. camber, fraction of chord"},
            "x_camber":{"type":"number","description":"New max. camber location, fraction of chord"},
            "derotate":{"type":"boolean","description":"Rotate the foil so that the chord line is horizontal"},
            "normalize":{"type":"boolean","description":"Scale the foil to unit chord with the leading edge at the origin"},
            "panels":{"type":"integer","minimum":20,"maximum":500,"description":"Re-panel with this number of panels"},
            "bunch":{"type":"number","minimum":0,"maximum":1,"default":0.5,"description":"Panel bunching amplitude at LE and TE when re-paneling"},
            "te_flap":{"type":"object","description":"Trailing edge flap hinge definition; the deflection is set per polar or per plane polar",
                "properties":{"enabled":{"type":"boolean","default":true},
                              "x_hinge":{"type":"number","description":"Hinge location, fraction of chord, e.g. 0.75"},
                              "y_hinge":{"type":"number","default":0.5,"description":"Hinge location, fraction of the local thickness"}}}},
            "required":["name"]})J");
        t.annotations = json(R"J({"destructiveHint":true,"openWorldHint":false})J");
        t.bMutates = true;
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            Foil *pSrc = findFoil(argString(args, "name"));
            QString copyto = argString(args, "copy_to", QString());

            Foil *pFoil = pSrc;
            if(!copyto.isEmpty())
            {
                if(Objects2d::foilExists(copyto.toStdString())) throw ToolError("A foil named \""+copyto+"\" already exists");
                pFoil = new Foil(pSrc);
                pFoil->setName(copyto.toStdString());
            }

            QStringList done;
            bool bGeom = false;
            if(args.contains("thickness") || args.contains("camber") || args.contains("x_thickness") || args.contains("x_camber"))
            {
                double xc = (pFoil->xCamber()-pFoil->baseCbLine().front().x) / std::max(1.e-9, pFoil->baseCbLine().back().x-pFoil->baseCbLine().front().x);
                double xt = (pFoil->xThickness()-pFoil->baseCbLine().front().x) / std::max(1.e-9, pFoil->baseCbLine().back().x-pFoil->baseCbLine().front().x);
                foil::scaleFoil(pFoil,
                                argDouble(args, "camber", pFoil->maxCamber()), argDouble(args, "x_camber", xc),
                                argDouble(args, "thickness", pFoil->maxThickness()), argDouble(args, "x_thickness", xt));
                pFoil->initGeometry();
                done.append("scaled thickness/camber");
                bGeom = true;
            }
            if(argBool(args, "derotate", false))  {foil::deRotate(pFoil);  done.append("de-rotated"); bGeom = true;}
            if(argBool(args, "normalize", false)) {foil::normalize(pFoil); done.append("normalized"); bGeom = true;}
            if(args.contains("panels"))
            {
                if(!pFoil->rePanel(argInt(args, "panels"), argDouble(args, "bunch", 0.5)))
                {
                    if(pFoil!=pSrc) delete pFoil;
                    throw ToolError("Re-paneling failed");
                }
                done.append("re-paneled");
                bGeom = true;
            }
            if(args.contains("te_flap"))
            {
                QJsonObject f = args.value("te_flap").toObject();
                pFoil->setTEFlapData(argBool(f, "enabled", true), argDouble(f, "x_hinge", 0.75), argDouble(f, "y_hinge", 0.5), 0.0);
                pFoil->setFlaps();
                done.append("set TE flap hinge");
                bGeom = true;
            }

            if(pFoil!=pSrc)
            {
                storeNewFoil(pFoil, copyto, false);
            }
            else if(bGeom && foilPolarCount(pFoil)>0)
            {
                Objects2d::deleteFoilResults(pFoil, true);
                done.append("deleted the foil's polars");
            }

            QJsonObject o = foilJson(pFoil, false);
            o["operations"] = QJsonArray::fromStringList(done);
            return {o, QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "foil_rename";
        t.title = "Rename foil";
        t.description = "Renames a foil; its polars and the wing sections of planes which use it are updated.";
        t.inputSchema = json(R"J({"type":"object","properties":{"name":{"type":"string"},"new_name":{"type":"string"}},"required":["name","new_name"]})J");
        t.annotations = json(R"J({"idempotentHint":true,"openWorldHint":false})J");
        t.bMutates = true;
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            Foil *pFoil = findFoil(argString(args, "name"));
            QString newname = argString(args, "new_name");
            if(newname.isEmpty()) throw ToolError("new_name is empty");
            if(Objects2d::foilExists(newname.toStdString())) throw ToolError("A foil named \""+newname+"\" already exists");
            std::string oldname = pFoil->name();
            Objects2d::renameThisFoil(pFoil, newname.toStdString());
            Objects3d::updateFoilName(oldname, pFoil->name());
            return {foilJson(pFoil, false), QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "foil_delete";
        t.title = "Delete foil";
        t.description = "Deletes a foil with its polars and operating points.";
        t.inputSchema = json(R"J({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})J");
        t.annotations = json(R"J({"destructiveHint":true,"idempotentHint":true,"openWorldHint":false})J");
        t.bMutates = true;
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            Foil *pFoil = findFoil(argString(args, "name"));
            QString name = qs(pFoil->name());
            Objects2d::deleteFoil(pFoil);
            return {QJsonObject{{"deleted", name}}, QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "foil_export_dat";
        t.title = "Export foil coordinates";
        t.description = "Returns the foil's coordinates in Selig .dat format, and optionally writes them to a file.";
        t.inputSchema = json(R"J({"type":"object","properties":{
            "name":{"type":"string"},
            "path":{"type":"string","description":"Optional absolute path of the file to write"}},
            "required":["name"]})J");
        t.annotations = json(R"J({"readOnlyHint":false,"openWorldHint":false})J");
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            Foil *pFoil = findFoil(argString(args, "name"));
            std::string dat;
            pFoil->exportFoilToDat(dat);
            QJsonObject o{{"name", qs(pFoil->name())}};
            QString path = argString(args, "path", QString());
            if(!path.isEmpty())
            {
                std::ofstream out(path.toStdString());
                if(!out) throw ToolError("Could not write the file "+path);
                out << dat;
                o["written"] = path;
            }
            return {o, qs(dat)};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "foil_polar_create";
        t.title = "Create foil polar";
        t.description = "Defines a 2d polar, i.e. a set of XFoil analysis conditions, for a foil. Run it with foil_analyze.";
        t.inputSchema = json((QString(R"J({"type":"object","properties":{
            "foil":{"type":"string"},
            "name":{"type":"string","description":"Polar name; generated from the parameters if omitted"},)J") + s_PolarSpecSchema + R"J(},
            "required":["foil"]})J").toUtf8().constData());
        t.annotations = json(R"J({"openWorldHint":false})J");
        t.bMutates = true;
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            Foil *pFoil = findFoil(argString(args, "foil"));
            return {polarJson(createPolar(pFoil, args), false), QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "foil_analyze";
        t.title = "Run XFoil analysis";
        t.description = "Runs an XFoil viscous analysis of a foil over one or more ranges and stores the results in the polar. "
                        "The polar is selected by name, or created from the given specification if it does not exist. "
                        "The sequence variable is alpha (or Cl if sequence=\"cl\") for T1-T3 polars, Re for T4 and the flap angle for T6. "
                        "Ranges crossing zero are split so that XFoil starts near zero, which improves convergence. "
                        "Supports progress notifications and cancellation.";
        t.inputSchema = json((QString(R"J({"type":"object","properties":{
            "foil":{"type":"string"},
            "polar":{"type":"string","description":"Name of an existing polar, or of the polar to create"},)J") + s_PolarSpecSchema + R"J(,
            "sequence":{"type":"string","enum":["alpha","cl"],"default":"alpha"},
            "range":{"type":"object","properties":{"start":{"type":"number"},"end":{"type":"number"},"step":{"type":"number"}},"required":["start"]},
            "ranges":{"type":"array","items":{"type":"object","properties":{"start":{"type":"number"},"end":{"type":"number"},"step":{"type":"number"}},"required":["start"]}},
            "keep_operating_points":{"type":"boolean","default":true,"description":"Store the operating points (Cp distributions) for later retrieval"},
            "include_log":{"type":"boolean","default":false}},
            "required":["foil"]})J").toUtf8().constData());
        t.annotations = json(R"J({"openWorldHint":false})J");
        t.bMutates = true;
        t.handler = [](QJsonObject const &args, Context &ctx) -> ToolResult
        {
            Foil *pFoil = findFoil(argString(args, "foil"));
            QString polarname = argString(args, "polar", QString());

            Polar *pPolar = polarname.isEmpty() ? nullptr : Objects2d::polar(pFoil, polarname.toStdString());
            bool bCreated = false;
            if(!pPolar)
            {
                QJsonObject spec = args;
                if(!polarname.isEmpty()) spec["name"] = polarname;
                pPolar = createPolar(pFoil, spec);
                bCreated = true;
            }

            bool bAlpha = argString(args, "sequence", "alpha").toLower()!="cl";
            std::vector<AnalysisRange> ranges = xfoilRanges(args, pPolar->isType123() && bAlpha);
            bool bKeepOpps = argBool(args, "keep_operating_points", true);

            int n0 = pPolar->dataSize();
            int nRanges = int(ranges.size());
            std::string log;
            int nOpps = 0;
            for(int ir=0; ir<nRanges && !ctx.isCancelled(); ir++)
            {
                ctx.progress(ir, nRanges, QString("XFoil range %1/%2").arg(ir+1).arg(nRanges));
                nOpps += runXFoil(pFoil, pPolar, {ranges.at(ir)}, bAlpha, bKeepOpps, ctx, log);
            }
            ctx.progress(nRanges, nRanges, "XFoil analysis done");
            ctx.log(DEBUG, qs(log));

            QJsonObject o = polarJson(pPolar, true);
            o["polar_created"] = bCreated;
            o["n_new_points"] = pPolar->dataSize()-n0;
            o["n_operating_points_stored"] = nOpps;
            return {o, argBool(args, "include_log", false) ? qs(log) : QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "foil_polar_list";
        t.title = "List foil polars";
        t.description = "Lists the 2d polars, optionally filtered by foil.";
        t.inputSchema = json(R"J({"type":"object","properties":{"foil":{"type":"string"}}})J");
        t.annotations = json(R"J({"readOnlyHint":true,"openWorldHint":false})J");
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            QString foilname = argString(args, "foil", QString());
            if(!foilname.isEmpty()) findFoil(foilname);
            QJsonArray polars;
            for(Polar const *pPolar : Objects2d::polars())
                if(foilname.isEmpty() || qs(pPolar->foilName())==foilname) polars.append(polarJson(pPolar, false));
            return {QJsonObject{{"polars", polars}}, QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "foil_polar_get";
        t.title = "Get foil polar";
        t.description = "Returns a 2d polar's specification, summary (Cl max, max Cl/Cd...) and data columns.";
        t.inputSchema = json(R"J({"type":"object","properties":{
            "foil":{"type":"string"},"polar":{"type":"string"},
            "include_data":{"type":"boolean","default":true}},
            "required":["foil","polar"]})J");
        t.annotations = json(R"J({"readOnlyHint":true,"openWorldHint":false})J");
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            Polar *pPolar = findPolar(findFoil(argString(args, "foil")), argString(args, "polar"));
            return {polarJson(pPolar, argBool(args, "include_data", true)), QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "foil_polar_delete";
        t.title = "Delete foil polar";
        t.description = "Deletes a 2d polar and its operating points.";
        t.inputSchema = json(R"J({"type":"object","properties":{"foil":{"type":"string"},"polar":{"type":"string"}},"required":["foil","polar"]})J");
        t.annotations = json(R"J({"destructiveHint":true,"idempotentHint":true,"openWorldHint":false})J");
        t.bMutates = true;
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            Foil *pFoil = findFoil(argString(args, "foil"));
            Polar *pPolar = findPolar(pFoil, argString(args, "polar"));
            QString name = qs(pPolar->name());
            for(int i=Objects2d::nOpPoints()-1; i>=0; i--)
            {
                OpPoint *pOpp = Objects2d::opPointAt(i);
                if(pOpp->foilName()==pFoil->name() && pOpp->polarName()==pPolar->name()) Objects2d::deleteOpp(pOpp);
            }
            Objects2d::deletePolar(pPolar);
            return {QJsonObject{{"deleted", name}}, QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "foil_opp_get";
        t.title = "Get foil operating point";
        t.description = "Returns an operating point's coefficients and its pressure coefficient distribution along the foil surface. "
                        "Requires the polar to have been run with keep_operating_points=true.";
        t.inputSchema = json(R"J({"type":"object","properties":{
            "foil":{"type":"string"},"polar":{"type":"string"},
            "alpha":{"type":"number","description":"Angle of attack (or the sequence value for T4/T6 polars); the closest stored point is returned"},
            "include_distributions":{"type":"boolean","default":true}},
            "required":["foil","polar","alpha"]})J");
        t.annotations = json(R"J({"readOnlyHint":true,"openWorldHint":false})J");
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            Foil *pFoil = findFoil(argString(args, "foil"));
            Polar *pPolar = findPolar(pFoil, argString(args, "polar"));
            double target = argDouble(args, "alpha");

            OpPoint const *pBest = nullptr;
            double best = 1.e10;
            QJsonArray available;
            for(OpPoint const *pOpp : Objects2d::operatingPoints())
            {
                if(pOpp->foilName()!=pFoil->name() || pOpp->polarName()!=pPolar->name()) continue;
                double v = pPolar->isType4() ? pOpp->Reynolds() : (pPolar->isControlPolar() ? pOpp->theta() : pOpp->aoa());
                available.append(v);
                if(std::fabs(v-target)<best) {best = std::fabs(v-target); pBest = pOpp;}
            }
            if(!pBest) throw ToolError("No operating points stored for this polar; run foil_analyze with keep_operating_points=true");

            QJsonObject o = opPointJson(pBest, argBool(args, "include_distributions", true));
            o["available_values"] = available;
            return {o, QString()};
        };
        server.addTool(t);
    }
}

} // namespace flow5mcp
