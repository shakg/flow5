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

#include <atomic>
#include <cmath>
#include <filesystem>
#include <memory>
#include <set>
#include <thread>

#include <QFile>

#include <flow5-io.h>
#include <llttask.h>
#include <objects2d.h>
#include <objects3d.h>
#include <planeopp.h>
#include <planepolar.h>
#include <planepolarnamemaker.h>
#include <planestl.h>
#include <planetask.h>
#include <planexfl.h>
#include <polar.h>
#include <polarnamemaker.h>
#include <wingxfl.h>
#include <xmlplanewriter.h>
#include <xmlplanepolarwriter.h>

#include "flow5tools.h"

using namespace mcp;


namespace
{
    Vector3d argVec3(QJsonObject const &args, QString const &key, Vector3d const &defaultValue)
    {
        if(!args.contains(key)) return defaultValue;
        QJsonArray a = argArray(args, key);
        if(a.size()!=3 || !a.at(0).isDouble() || !a.at(1).isDouble() || !a.at(2).isDouble())
            throw ToolError(key+" must be an array of 3 numbers [x, y, z]");
        return {a.at(0).toDouble(), a.at(1).toDouble(), a.at(2).toDouble()};
    }

    xfl::enumDistribution parseDistribution(QString const &name, xfl::enumDistribution defaultValue)
    {
        if(name.isEmpty()) return defaultValue;
        QString n = name.toUpper();
        if(n=="UNIFORM")  return xfl::UNIFORM;
        if(n=="COSINE")   return xfl::COSINE;
        if(n=="SINE")     return xfl::SINE;
        if(n=="INV_SINE") return xfl::INV_SINE;
        if(n=="TANH")     return xfl::TANH;
        if(n=="EXP")      return xfl::EXP;
        if(n=="INV_EXP")  return xfl::INV_EXP;
        if(n=="INV_SINH") return xfl::INV_SINH;
        throw ToolError("Invalid panel distribution: "+name);
    }

    void appendPointMasses(Inertia &inertia, QJsonArray const &masses)
    {
        for(QJsonValue const &v : masses)
        {
            QJsonObject pm = v.toObject();
            inertia.appendPointMass(argDouble(pm, "mass"), argVec3(pm, "position", Vector3d()), argString(pm, "tag", QString()).toStdString());
        }
    }

    void defineWing(PlaneXfl *pPlane, QJsonObject const &w)
    {
        QString type = argString(w, "type", "main").toLower();
        xfl::enumType wingtype = xfl::Main;
        if     (type=="main")     wingtype = xfl::Main;
        else if(type=="elevator") wingtype = xfl::Elevator;
        else if(type=="fin")      wingtype = xfl::Fin;
        else if(type=="other")    wingtype = xfl::OtherWing;
        else throw ToolError("Invalid wing type: "+type);

        WingXfl *pWing = pPlane->addWing(wingtype);
        switch(wingtype)
        {
            case xfl::Elevator: pWing->makeDefaultStab(); break;
            case xfl::Fin:      pWing->makeDefaultFin();  break;
            default:            pWing->makeDefaultWing(); break;
        }
        pWing->setName(argString(w, "name", type).toStdString());

        pPlane->setWingPosition(pWing, argVec3(w, "position", Vector3d()));
        pPlane->setRxAngle(pWing, argDouble(w, "rx", wingtype==xfl::Fin ? -90.0 : 0.0));
        pPlane->setRyAngle(pWing, argDouble(w, "ry", 0.0));
        if(w.contains("two_sided"))         pWing->setTwoSided(argBool(w, "two_sided", true));
        if(w.contains("closed_inner_side")) pWing->setClosedInnerSide(argBool(w, "closed_inner_side", false));
        else if(wingtype==xfl::Fin)         pWing->setClosedInnerSide(true);

        if(w.contains("structural_mass")) pWing->inertia().setStructuralMass(argDouble(w, "structural_mass"));
        if(w.contains("point_masses"))    appendPointMasses(pWing->inertia(), argArray(w, "point_masses"));

        QJsonArray sections = argArray(w, "sections");
        if(sections.size()<2) throw ToolError("Each wing needs at least 2 sections (root and tip)");

        int nx = argInt(w, "nx", 13);
        xfl::enumDistribution xdist = parseDistribution(argString(w, "x_distribution", QString()), xfl::TANH);

        pWing->clearSections();
        double yprev = -1.e10;
        for(int is=0; is<sections.size(); is++)
        {
            QJsonObject s = sections.at(is).toObject();
            WingSection ws;
            ws.setYPosition(argDouble(s, "y"));
            if(ws.yPosition()<=yprev) throw ToolError("Section y positions must be strictly increasing from root to tip");
            yprev = ws.yPosition();
            ws.setChord(argDouble(s, "chord"));
            ws.setXOffset(argDouble(s, "offset", 0.0));
            ws.setDihedral(argDouble(s, "dihedral", 0.0));
            ws.setTwist(argDouble(s, "twist", 0.0));

            QString foil  = argString(s, "foil", QString());
            QString left  = argString(s, "left_foil", foil);
            QString right = argString(s, "right_foil", foil);
            if(left.isEmpty() || right.isEmpty()) throw ToolError("Each section needs a foil (or left_foil and right_foil)");
            flow5mcp::findFoil(left);
            flow5mcp::findFoil(right);
            ws.setFoilNames(left.toStdString(), right.toStdString());

            ws.setNX(nx); // must be the same for all sections
            ws.setXDistType(xdist);
            ws.setNY(argInt(s, "ny", 11));
            ws.setYDistType(parseDistribution(argString(s, "y_distribution", QString()), xfl::UNIFORM));
            pWing->appendSection(ws);
        }
    }

    PlaneXfl *planeXfl(Plane *pPlane)
    {
        return dynamic_cast<PlaneXfl*>(pPlane);
    }

    /** Builds the plane's mesh and wake for the polar, as flow5's batch executor does before a panel analysis */
    void preparePlane(Plane *pPlane, PlanePolar const *pPolar)
    {
        if(pPolar->isLLTMethod()) return;

        bool bThickSurfaces = !(pPolar->isVLM() || pPolar->bThinSurfaces());

        PlaneXfl *pPlaneXfl = planeXfl(pPlane);
        if(pPlaneXfl)
        {
            if(!pPlaneXfl->isInitialized()) pPlaneXfl->makePlane(bThickSurfaces, pPolar->bIgnoreBodyPanels(), true);

            if(pPolar->isQuadMethod())
            {
                pPlaneXfl->makeQuadMesh(bThickSurfaces, pPolar->bIgnoreBodyPanels());
                if(!pPolar->bVortonWake())
                    pPlaneXfl->refQuadMesh().makeWakePanels(pPolar->NXWakePanel4(), pPolar->wakePanelFactor(), pPolar->wakeLength(), Vector3d(1.0,0,0), true);
                else
                    pPlaneXfl->refQuadMesh().makeWakePanels(3, 1, pPolar->bufferWakeLength(), Vector3d(1.0,0,0), false);
            }
            if(pPolar->isTriangleMethod())
            {
                pPlaneXfl->makeTriMesh(bThickSurfaces);
                pPlaneXfl->refTriMesh().makeWakePanels(pPolar->NXWakePanel4(), pPolar->wakePanelFactor(), pPolar->wakeLength(), Vector3d(1.0,0,0), true);
            }
        }
        pPlane->restoreMesh();
    }

    /** LLT and interpolated viscous drag need 2d polars for each wing foil over the operating Reynolds range.
     * Computes T1 polars for the foils which have none. Returns the names of the polars created. */
    QStringList ensureFoilPolars(PlaneXfl const *pPlane, PlanePolar const *pPolar, Context &ctx, std::string &log)
    {
        std::set<std::string> foilnames;
        double cmin = 1.e10, cmax = 0.0;
        for(WingXfl const *pWing : pPlane->wings())
        {
            for(WingSection const &sec : pWing->sections())
            {
                foilnames.insert(sec.leftFoilName());
                foilnames.insert(sec.rightFoilName());
                cmin = std::min(cmin, sec.chord());
                cmax = std::max(cmax, sec.chord());
            }
        }

        // estimate the speed range, then the Reynolds range
        double vmin = pPolar->velocity(), vmax = pPolar->velocity();
        if(pPolar->isFixedLiftPolar() || pPolar->isGlidePolar())
        {
            double wS = pPolar->mass()*9.81 / std::max(1.e-6, pPolar->referenceArea());
            if(wS>0.0)
            {
                vmin = std::sqrt(2.0*wS/pPolar->density()/1.4);
                vmax = std::sqrt(2.0*wS/pPolar->density()/0.05);
            }
            else {vmin = 5.0; vmax = 50.0;}
        }
        double nu = pPolar->viscosity();
        double remin = std::max(20000.0, 0.5*vmin*cmin/nu);
        double remax = std::max(remin*2.0, 1.5*vmax*cmax/nu);

        // foils whose T1 polars do not cover the Reynolds range
        std::vector<Foil*> missing;
        for(std::string const &name : foilnames)
        {
            Foil *pFoil = Objects2d::foil(name);
            if(!pFoil) continue;
            double plrmin = 1.e12, plrmax = 0.0;
            for(Polar const *pP : Objects2d::polars())
            {
                if(pP->foilName()!=name || !pP->isType1() || !pP->hasData()) continue;
                plrmin = std::min(plrmin, pP->Reynolds());
                plrmax = std::max(plrmax, pP->Reynolds());
            }
            if(plrmin>remin*1.01 || plrmax<remax*0.99) missing.push_back(pFoil);
        }
        if(missing.empty()) return {};

        int nRe = 7;
        std::vector<double> reynolds;
        for(int i=0; i<nRe; i++) reynolds.push_back(std::round(remin*std::pow(remax/remin, double(i)/double(nRe-1))/1000.0)*1000.0);
        reynolds.front() = std::floor(remin/1000.0)*1000.0; // so that the polars cover the range after rounding
        reynolds.back()  = std::ceil(remax/1000.0)*1000.0;

        QStringList created;
        int nTotal = int(missing.size())*nRe, nDone = 0;
        for(Foil *pFoil : missing)
        {
            for(double re : reynolds)
            {
                if(ctx.isCancelled()) return created;
                ctx.progress(nDone++, nTotal, QString("Computing the foil polar %1 at Re=%2").arg(qs(pFoil->name())).arg(re));
                Polar *pFoilPolar = Objects2d::createPolar(pFoil, xfl::T1POLAR, re, 0.0, pPolar->NCrit(), pPolar->XTrTop(), pPolar->XTrBot());
                pFoilPolar->setName(PolarNameMaker::makeName(pFoilPolar));
                if(Objects2d::polar(pFoil, pFoilPolar->name()))
                {
                    delete pFoilPolar;
                    continue;
                }
                Objects2d::insertPolar(pFoilPolar);
                flow5mcp::runXFoil(pFoil, pFoilPolar, {{true, 0.0, 18.0, 0.5}, {true, -0.5, -8.0, 0.5}}, true, false, ctx, log);
                created.append(qs(pFoil->name()+" / "+pFoilPolar->name()));
            }
        }
        return created;
    }


    void applyPolarArgs(PlanePolar *pPolar, Plane const *pPlane, QJsonObject const &args)
    {
        if(args.contains("velocity"))       pPolar->setVelocity(argDouble(args, "velocity"));
        if(args.contains("alpha"))          pPolar->setAlphaSpec(argDouble(args, "alpha"));
        if(args.contains("beta"))           pPolar->setBetaSpec(argDouble(args, "beta"));
        if(args.contains("bank_angle"))     pPolar->setPhiSpec(argDouble(args, "bank_angle"));
        if(args.contains("viscous"))        pPolar->setViscous(argBool(args, "viscous", true));
        if(args.contains("viscous_on_the_fly")) pPolar->setViscOnTheFly(argBool(args, "viscous_on_the_fly", true));
        if(args.contains("thick_surfaces")) pPolar->setThickSurfaces(argBool(args, "thick_surfaces", false));
        if(args.contains("density"))        pPolar->setDensity(argDouble(args, "density"));
        if(args.contains("viscosity"))      pPolar->setViscosity(argDouble(args, "viscosity"));
        if(args.contains("ground_height"))
        {
            pPolar->setGroundEffect(true);
            pPolar->setGroundHeight(argDouble(args, "ground_height"));
        }
        if(args.contains("ignore_body_panels")) pPolar->setIgnoreBodyPanels(argBool(args, "ignore_body_panels", false));

        if(args.contains("mass") || args.contains("cog"))
        {
            pPolar->setAutoInertia(false);
            if(args.contains("mass")) pPolar->setMass(argDouble(args, "mass"));
            if(args.contains("cog"))  pPolar->setCoG(argVec3(args, "cog", Vector3d()));
        }

        QString refdim = argString(args, "reference_dimensions", "projected").toLower();
        if(refdim=="planform")
        {
            pPolar->setReferenceDim(xfl::PLANFORM);
            pPolar->setReferenceArea(pPlane->planformArea(pPolar->bIncludeOtherWingAreas()));
            pPolar->setReferenceSpanLength(pPlane->planformSpan());
        }
        else if(refdim=="projected")
        {
            pPolar->setReferenceDim(xfl::PROJECTED);
            pPolar->setReferenceArea(pPlane->projectedArea(pPolar->bIncludeOtherWingAreas()));
            pPolar->setReferenceSpanLength(pPlane->projectedSpan());
        }
        else if(refdim=="custom")
        {
            pPolar->setReferenceDim(xfl::CUSTOM);
            pPolar->setReferenceArea(argDouble(args, "reference_area"));
            pPolar->setReferenceSpanLength(argDouble(args, "reference_span"));
        }
        else throw ToolError("reference_dimensions must be one of planform, projected, custom");
        pPolar->setReferenceChordLength(argDouble(args, "reference_chord", pPlane->mac()));

        if(args.contains("flap_angles"))
        {
            PlaneXfl const *pPlaneXfl = dynamic_cast<PlaneXfl const*>(pPlane);
            if(!pPlaneXfl) throw ToolError("Flap angles only apply to xfl-type planes");
            QJsonArray wings = argArray(args, "flap_angles");
            if(wings.size()>pPolar->nFlapCtrls()) throw ToolError(QString("flap_angles has %1 entries but the plane has %2 wings").arg(wings.size()).arg(pPolar->nFlapCtrls()));
            for(int iw=0; iw<wings.size(); iw++)
            {
                QJsonArray flaps = wings.at(iw).toArray();
                if(flaps.size()>pPolar->nFlapCtrls(iw))
                    throw ToolError(QString("Wing %1 has %2 flaps but %3 angles were given").arg(iw).arg(pPolar->nFlapCtrls(iw)).arg(flaps.size()));
                for(int jf=0; jf<flaps.size(); jf++) pPolar->setFlapAngleValue(iw, jf, flaps.at(jf).toDouble());
            }
        }
    }

    /** Pops the task's log messages; returns the number of end-of-operating-point reports */
    int drainTaskLog(Task3d *pTask, std::string &log)
    {
        int nEnd = 0;
        std::unique_lock<std::mutex> lck(pTask->m_mtx);
        while(!pTask->m_QueueVPW.empty())
        {
            TaskReport const &r = pTask->m_QueueVPW.front();
            log += r.m_Msg;
            if(r.m_bEndOpp) nEnd++;
            pTask->m_QueueVPW.pop();
        }
        return nEnd;
    }

    char const *s_PlanePolarSchema = R"J(
        "velocity":{"type":"number","description":"Freestream velocity in m/s; used by T1, T5, T6 and T7 polars"},
        "alpha":{"type":"number","description":"Angle of attack in degrees for T5 polars"},
        "beta":{"type":"number","description":"Sideslip angle in degrees"},
        "bank_angle":{"type":"number","description":"Bank angle in degrees"},
        "viscous":{"type":"boolean","default":true,"description":"Add the viscous drag computed from the wing foils"},
        "viscous_on_the_fly":{"type":"boolean","default":true,"description":"Compute the viscous drag with XFoil on the fly; if false, interpolate on existing foil polars (which must cover the Re range)"},
        "thick_surfaces":{"type":"boolean","default":false,"description":"Model wings as thick surfaces (panel method) instead of thin surfaces"},
        "density":{"type":"number","default":1.225,"description":"Fluid density, kg/m3"},
        "viscosity":{"type":"number","default":1.5e-5,"description":"Fluid kinematic viscosity, m2/s"},
        "ground_height":{"type":"number","description":"If set, enables ground effect at this height, m"},
        "ignore_body_panels":{"type":"boolean","default":false},
        "mass":{"type":"number","description":"Overrides the plane's mass, kg; used by T2/T3/T7 polars"},
        "cog":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"Overrides the centre of gravity [x,y,z], m; moments are computed about this point"},
        "reference_dimensions":{"type":"string","enum":["projected","planform","custom"],"default":"projected"},
        "reference_area":{"type":"number","description":"For custom reference dimensions, m2"},
        "reference_span":{"type":"number","description":"For custom reference dimensions, m"},
        "reference_chord":{"type":"number","description":"Defaults to the main wing's mean aerodynamic chord, m"},
        "flap_angles":{"type":"array","description":"Flap deflections per wing, in degrees, positive down: [[wing0 flap0, flap1...], [wing1...]]; flaps are numbered from left tip to right tip","items":{"type":"array","items":{"type":"number"}}})J";

    char const *s_WingSchema = R"J({"type":"object","properties":{
        "name":{"type":"string"},
        "type":{"type":"string","enum":["main","elevator","fin","other"],"default":"main"},
        "position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"Root leading edge position [x,y,z], m"},
        "rx":{"type":"number","description":"Rotation about x in degrees; defaults to -90 for fins"},
        "ry":{"type":"number","default":0,"description":"Incidence (rotation about y) in degrees"},
        "two_sided":{"type":"boolean","description":"Mirror the wing about the xz plane; true by default except for fins"},
        "closed_inner_side":{"type":"boolean"},
        "structural_mass":{"type":"number","description":"kg"},
        "point_masses":{"type":"array","items":{"$ref":"#/$defs/pointMass"}},
        "nx":{"type":"integer","default":13,"description":"Number of chordwise panels"},
        "x_distribution":{"type":"string","enum":["UNIFORM","COSINE","SINE","INV_SINE","TANH","EXP","INV_EXP","INV_SINH"],"default":"TANH"},
        "sections":{"type":"array","minItems":2,"description":"Sections from root (y=0) to tip","items":{"type":"object","properties":{
            "y":{"type":"number","description":"Spanwise position, m; for a two-sided wing this is the half-span position"},
            "chord":{"type":"number","description":"m"},
            "offset":{"type":"number","default":0,"description":"Leading edge x offset relative to the root, m"},
            "dihedral":{"type":"number","default":0,"description":"Dihedral of the panel outboard of this section, degrees"},
            "twist":{"type":"number","default":0,"description":"degrees"},
            "foil":{"type":"string","description":"Foil name, used for both sides"},
            "left_foil":{"type":"string"},"right_foil":{"type":"string"},
            "ny":{"type":"integer","default":11,"description":"Number of spanwise panels between this section and the next"},
            "y_distribution":{"type":"string","enum":["UNIFORM","COSINE","SINE","INV_SINE","TANH","EXP","INV_EXP","INV_SINH"],"default":"UNIFORM"}},
            "required":["y","chord"]}}},
        "required":["sections"]})J";
}


namespace flow5mcp
{

void registerPlaneTools(Server &server)
{
    {
        Tool t;
        t.name = "plane_list";
        t.title = "List planes";
        t.description = "Lists the planes in memory with their main properties.";
        t.inputSchema = json(R"J({"type":"object","properties":{}})J");
        t.annotations = json(R"J({"readOnlyHint":true,"openWorldHint":false})J");
        t.handler = [](QJsonObject const &, Context &) -> ToolResult
        {
            QJsonArray planes;
            for(Plane const *pPlane : Objects3d::planes())
            {
                QJsonObject o = planeJson(pPlane);
                o.remove("wings");
                o.remove("point_masses");
                planes.append(o);
            }
            return {QJsonObject{{"planes", planes}}, QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "plane_create";
        t.title = "Create plane";
        t.description = "Creates an xfl-type plane from wing definitions. The foils referenced by the sections must exist. "
                        "Axes: x downstream, y towards the right wing, z up; positions in m. "
                        "Typical layout: main wing at the origin, elevator and fin positioned aft.";
        QString schema = QString(R"J({"type":"object","$defs":{
                "pointMass":{"type":"object","properties":{"mass":{"type":"number"},"position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"tag":{"type":"string"}},"required":["mass","position"]},
                "wing":%1},
            "properties":{
                "name":{"type":"string"},
                "description":{"type":"string"},
                "wings":{"type":"array","minItems":1,"items":{"$ref":"#/$defs/wing"}},
                "point_masses":{"type":"array","items":{"$ref":"#/$defs/pointMass"},"description":"Additional point masses, e.g. battery, payload"},
                "overwrite":{"type":"boolean","default":false}},
            "required":["name","wings"]})J").arg(s_WingSchema);
        t.inputSchema = json(schema.toUtf8().constData());
        t.annotations = json(R"J({"openWorldHint":false})J");
        t.bMutates = true;
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            QString name = argString(args, "name");
            if(name.isEmpty()) throw ToolError("The plane needs a name");
            Plane *pOld = Objects3d::plane(name.toStdString());
            if(pOld && !argBool(args, "overwrite", false)) throw ToolError("A plane named \""+name+"\" already exists; set overwrite=true to replace it");

            PlaneXfl *pPlane = new PlaneXfl;
            try
            {
                pPlane->setName(name.toStdString());
                pPlane->setDescription(argString(args, "description", QString()).toStdString());
                for(QJsonValue const &w : argArray(args, "wings")) defineWing(pPlane, w.toObject());
                if(args.contains("point_masses")) appendPointMasses(pPlane->inertia(), argArray(args, "point_masses"));
                pPlane->makePlane(false, false, true);
            }
            catch(...)
            {
                delete pPlane;
                throw;
            }

            if(pOld) Objects3d::deletePlane(pOld, true);
            pPlane->setTheStyle({true, Line::SOLID, 2, fl5Color(71, 171, 231), Line::NOSYMBOL});
            Objects3d::insertPlane(pPlane);

            std::string log;
            QJsonObject o = planeJson(pPlane);
            if(!pPlane->checkFoils(log)) o["warnings"] = qs(log);
            return {o, QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "plane_import_xml";
        t.title = "Import plane from XML";
        t.description = "Imports an xfl-type plane from a flow5/xflr5 XML plane file. The foils it uses must already be loaded.";
        t.inputSchema = json(R"J({"type":"object","properties":{
            "path":{"type":"string","description":"Absolute path to the .xml file"},
            "name":{"type":"string","description":"Overrides the plane name in the file"}},
            "required":["path"]})J");
        t.annotations = json(R"J({"openWorldHint":false})J");
        t.bMutates = true;
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            QString path = argString(args, "path");
            if(!std::filesystem::exists(path.toStdString())) throw ToolError("File not found: "+path);
            std::string log;
            PlaneXfl *pPlane = io::importPlaneFromXML(path.toStdString(), log);
            if(!pPlane) throw ToolError(qs(log));
            QString name = argString(args, "name", QString());
            if(!name.isEmpty()) pPlane->setName(name.toStdString());
            if(Objects3d::planeExists(pPlane->name()))
            {
                QString n = qs(pPlane->name());
                delete pPlane;
                throw ToolError("A plane named \""+n+"\" already exists");
            }
            std::string foillog;
            if(!pPlane->checkFoils(foillog))
            {
                delete pPlane;
                throw ToolError("Missing foils: "+qs(foillog)+"Load or create them first.");
            }
            pPlane->makePlane(false, false, true);
            Objects3d::insertPlane(pPlane);
            return {planeJson(pPlane), qs(log)};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "plane_import_mesh";
        t.title = "Import plane from mesh";
        t.description = "Imports a plane from an STL or OBJ triangular mesh. Trailing edges are guessed from the angle between adjacent triangles. "
                        "STL planes can only be analyzed with inviscid triangle methods and thick surfaces.";
        t.inputSchema = json(R"J({"type":"object","properties":{
            "path":{"type":"string"},
            "name":{"type":"string"},
            "format":{"type":"string","enum":["stl","obj"],"default":"stl"},
            "units_to_meter":{"type":"number","default":1,"description":"Scale factor from file units to meters, e.g. 0.001 for mm"},
            "reference_area":{"type":"number","description":"m2"},
            "reference_chord":{"type":"number","description":"m"},
            "reference_span":{"type":"number","description":"m"},
            "te_angle":{"type":"number","default":19,"description":"Max. angle between triangles identified as trailing edge pairs, degrees"},
            "mass":{"type":"number","description":"kg"},
            "cog":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},
            "required":["path","name","reference_area","reference_chord","reference_span"]})J");
        t.annotations = json(R"J({"openWorldHint":false})J");
        t.bMutates = true;
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            QString path = argString(args, "path");
            if(!std::filesystem::exists(path.toStdString())) throw ToolError("File not found: "+path);
            QString name = argString(args, "name");
            if(Objects3d::planeExists(name.toStdString())) throw ToolError("A plane named \""+name+"\" already exists");

            std::string log;
            io::enumMeshFileType type = argString(args, "format", "stl").toLower()=="obj" ? io::OBJ : io::STL;
            PlaneSTL *pPlane = io::importPlaneFromMesh(path.toStdString(), type, argDouble(args, "units_to_meter", 1.0), log);
            if(!pPlane) throw ToolError("Mesh import failed: "+qs(log));

            pPlane->setName(name.toStdString());
            pPlane->setRefArea(argDouble(args, "reference_area"));
            pPlane->setRefChord(argDouble(args, "reference_chord"));
            pPlane->setRefSpan(argDouble(args, "reference_span"));
            if(args.contains("mass")) pPlane->setStructuralMass(argDouble(args, "mass"));
            if(args.contains("cog"))  pPlane->setCoG_s(argVec3(args, "cog", Vector3d()));
            pPlane->makePlane(false, false, false);

            if(!pPlane->connectTriMesh(true, true))
            {
                delete pPlane;
                throw ToolError("Error connecting the mesh triangles");
            }
            std::string telog;
            if(!pPlane->guessTEPanels(float(argDouble(args, "te_angle", 19.0)), telog))
            {
                delete pPlane;
                throw ToolError("Error guessing the trailing edge: "+qs(telog));
            }
            Objects3d::insertPlane(pPlane);
            return {planeJson(pPlane), qs(log+telog)};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "plane_get";
        t.title = "Get plane";
        t.description = "Returns a plane's geometry, wings and sections, inertia and reference dimensions.";
        t.inputSchema = json(R"J({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})J");
        t.annotations = json(R"J({"readOnlyHint":true,"openWorldHint":false})J");
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            Plane *pPlane = findPlane(argString(args, "name"));
            return {planeJson(pPlane), qs(pPlane->planeData(false))};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "plane_export_xml";
        t.title = "Export plane to XML";
        t.description = "Writes an xfl-type plane definition to an XML file which can be edited and re-imported, or imported in flow5.";
        t.inputSchema = json(R"J({"type":"object","properties":{
            "name":{"type":"string"},
            "path":{"type":"string","description":"Absolute path of the .xml file"},
            "include_foils":{"type":"boolean","default":true,"description":"Embed the foil coordinates in the file"}},
            "required":["name","path"]})J");
        t.annotations = json(R"J({"destructiveHint":true,"idempotentHint":true,"openWorldHint":false})J");
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            Plane *pPlane = findPlane(argString(args, "name"));
            PlaneXfl *pPlaneXfl = planeXfl(pPlane);
            if(!pPlaneXfl) throw ToolError("Only xfl-type planes can be exported to XML");
            QString path = argString(args, "path");
            QFile file(path);
            if(!file.open(QIODevice::WriteOnly | QIODevice::Text)) throw ToolError("Could not write the file "+path);
            XmlPlaneWriter writer(file);
            writer.writeXMLPlane(*pPlaneXfl, argBool(args, "include_foils", true));
            file.close();
            return {QJsonObject{{"written", path}}, QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "plane_delete";
        t.title = "Delete plane";
        t.description = "Deletes a plane with its polars and operating points.";
        t.inputSchema = json(R"J({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})J");
        t.annotations = json(R"J({"destructiveHint":true,"idempotentHint":true,"openWorldHint":false})J");
        t.bMutates = true;
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            Plane *pPlane = findPlane(argString(args, "name"));
            QString name = qs(pPlane->name());
            Objects3d::deletePlane(pPlane, true);
            return {QJsonObject{{"deleted", name}}, QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "plane_polar_create";
        t.title = "Create plane polar";
        t.description = "Defines a 3d analysis (plane polar) for a plane. Inertia defaults to the plane's own; reference dimensions default to the projected ones. "
                        "Run it with plane_analyze.";
        QString schema = QString(R"J({"type":"object","properties":{
            "plane":{"type":"string"},
            "name":{"type":"string","description":"Polar name; generated from the parameters if omitted"},
            "type":{"type":"string","enum":["T1","T2","T3","T5","T6","T7"],"default":"T2","description":"See flow5_info"},
            "method":{"type":"string","enum":["LLT","VLM1","VLM2","QUADS","TRIUNIFORM","TRILINEAR"],"default":"TRIUNIFORM"},%1},
            "required":["plane"]})J").arg(s_PlanePolarSchema);
        t.inputSchema = json(schema.toUtf8().constData());
        t.annotations = json(R"J({"openWorldHint":false})J");
        t.bMutates = true;
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            Plane *pPlane = findPlane(argString(args, "plane"));
            xfl::enumPolarType type = parsePolarType(argString(args, "type", "T2"));
            if(type==xfl::T4POLAR || type==xfl::T8POLAR) throw ToolError("T4 and T8 plane polars are not supported by this server");
            xfl::enumAnalysisMethod method = parseAnalysisMethod(argString(args, "method", "TRIUNIFORM"));

            PlaneXfl *pPlaneXfl = planeXfl(pPlane);
            if(!pPlaneXfl)
            {
                if(method!=xfl::TRIUNIFORM && method!=xfl::TRILINEAR) throw ToolError("STL planes can only be analyzed with TRIUNIFORM or TRILINEAR");
                if(argBool(args, "viscous", false)) throw ToolError("STL planes have no foils and can only be analyzed inviscid");
            }
            if(method==xfl::LLT && (type!=xfl::T1POLAR && type!=xfl::T2POLAR && type!=xfl::T3POLAR))
                throw ToolError("LLT only supports T1, T2 and T3 polars");

            PlanePolar *pPolar = new PlanePolar;
            try
            {
                pPolar->setDefaultSpec(pPlane);
                pPolar->setPlaneName(pPlane->name());
                pPolar->setType(type);
                pPolar->setAnalysisMethod(method);
                pPolar->setViscous(pPlaneXfl!=nullptr);
                pPolar->setViscOnTheFly(true);
                pPolar->setThinSurfaces(pPlaneXfl!=nullptr && method!=xfl::QUADS);
                if(method==xfl::VLM1 || method==xfl::VLM2) pPolar->setThinSurfaces(true);
                if(!pPlaneXfl) pPolar->setThickSurfaces(true);
                if(pPlaneXfl) pPolar->resizeFlapCtrls(pPlaneXfl);
                applyPolarArgs(pPolar, pPlane, args);
                if((method==xfl::VLM1 || method==xfl::VLM2) && pPolar->bThickSurfaces())
                    throw ToolError("VLM methods require thin surfaces");
            }
            catch(...)
            {
                delete pPolar;
                throw;
            }

            QString name = argString(args, "name", QString());
            pPolar->setName(name.isEmpty() ? PlanePolarNameMaker::makeName(pPlane, pPolar) : name.toStdString());
            if(Objects3d::wPolar(pPlane, pPolar->name()))
            {
                QString n = qs(pPolar->name());
                delete pPolar;
                throw ToolError("A polar named \""+n+"\" already exists for this plane");
            }
            pPolar->setTheStyle({true, Line::SOLID, 2, fl5Color(239, 51, 153), Line::NOSYMBOL});
            Objects3d::insertPlanePolar(pPolar);

            std::string props;
            pPolar->getProperties(props, pPlane);
            return {planePolarJson(pPolar, false), qs(props)};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "plane_polar_import_xml";
        t.title = "Import plane polar from XML";
        t.description = "Imports a plane analysis definition from a flow5 XML analysis file and attaches it to a plane.";
        t.inputSchema = json(R"J({"type":"object","properties":{
            "path":{"type":"string"},"plane":{"type":"string"},
            "name":{"type":"string","description":"Overrides the polar name"}},
            "required":["path","plane"]})J");
        t.annotations = json(R"J({"openWorldHint":false})J");
        t.bMutates = true;
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            Plane *pPlane = findPlane(argString(args, "plane"));
            QString path = argString(args, "path");
            if(!std::filesystem::exists(path.toStdString())) throw ToolError("File not found: "+path);
            std::string log;
            PlanePolar *pPolar = io::importAnalysisFromXML(path.toStdString(), log);
            if(!pPolar) throw ToolError(qs(log));
            pPolar->setPlaneName(pPlane->name());
            QString name = argString(args, "name", QString());
            if(!name.isEmpty()) pPolar->setName(name.toStdString());
            if(pPolar->bAutoInertia()) pPolar->retrieveInertia(pPlane);
            if(Objects3d::wPolar(pPlane, pPolar->name()))
            {
                QString n = qs(pPolar->name());
                delete pPolar;
                throw ToolError("A polar named \""+n+"\" already exists for this plane");
            }
            Objects3d::insertPlanePolar(pPolar);
            return {planePolarJson(pPolar, false), qs(log)};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "plane_polar_export_xml";
        t.title = "Export plane polar to XML";
        t.description = "Writes a plane analysis definition to a flow5 XML analysis file.";
        t.inputSchema = json(R"J({"type":"object","properties":{"plane":{"type":"string"},"polar":{"type":"string"},"path":{"type":"string"}},"required":["plane","polar","path"]})J");
        t.annotations = json(R"J({"destructiveHint":true,"idempotentHint":true,"openWorldHint":false})J");
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            PlanePolar *pPolar = findPlanePolar(findPlane(argString(args, "plane")), argString(args, "polar"));
            QString path = argString(args, "path");
            QFile file(path);
            if(!file.open(QIODevice::WriteOnly | QIODevice::Text)) throw ToolError("Could not write the file "+path);
            XmlPlanePolarWriter writer(file);
            writer.writeXMLWPolar(pPolar);
            file.close();
            return {QJsonObject{{"written", path}}, QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "plane_analyze";
        t.title = "Run plane analysis";
        t.description = "Runs a 3d analysis of a plane for a list of operating points and stores the results in the plane polar. "
                        "The operating point variable is alpha (degrees) for T1-T3, beta (degrees) for T5 and the control parameter for T6/T7. "
                        "Returns the polar data and the operating point coefficients. Supports progress notifications and cancellation.";
        t.inputSchema = json(R"J({"type":"object","properties":{
            "plane":{"type":"string"},
            "polar":{"type":"string"},
            "values":{"type":"array","items":{"type":"number"}},
            "range":{"type":"object","properties":{"start":{"type":"number"},"end":{"type":"number"},"step":{"type":"number"}},"required":["start"]},
            "ranges":{"type":"array","items":{"type":"object","properties":{"start":{"type":"number"},"end":{"type":"number"},"step":{"type":"number"}},"required":["start"]}},
            "compute_derivatives":{"type":"boolean","default":false,"description":"Compute stability derivatives (always done for T7)"},
            "auto_foil_polars":{"type":"boolean","default":true,"description":"For LLT and interpolated viscous drag: compute T1 polars over the expected Reynolds range for wing foils which have none"},
            "keep_operating_points":{"type":"boolean","default":true},
            "include_log":{"type":"boolean","default":false}},
            "required":["plane","polar"]})J");
        t.annotations = json(R"J({"openWorldHint":false})J");
        t.bMutates = true;
        t.handler = [](QJsonObject const &args, Context &ctx) -> ToolResult
        {
            Plane *pPlane = findPlane(argString(args, "plane"));
            PlanePolar *pPolar = findPlanePolar(pPlane, argString(args, "polar"));
            std::vector<double> values = parseValues(args);
            bool bKeepOpps = argBool(args, "keep_operating_points", true);
            int nValues = int(values.size());

            if(pPolar->bAutoInertia()) pPolar->retrieveInertia(pPlane);
            preparePlane(pPlane, pPolar);

            QStringList autopolars;
            std::string foillog;
            if(planeXfl(pPlane) && (pPolar->isLLTMethod() || (pPolar->isViscous() && pPolar->isViscInterpolated()))
               && argBool(args, "auto_foil_polars", true))
                autopolars = ensureFoilPolars(planeXfl(pPlane), pPolar, ctx, foillog);

            int n0 = pPolar->dataSize();
            std::string log;
            std::vector<PlaneOpp*> opps;

            Task3d::setCancelled(false);
            ctx.progress(0, nValues, "Starting the analysis");

            if(pPolar->isLLTMethod())
            {
                PlaneXfl *pPlaneXfl = planeXfl(pPlane);
                if(!pPlaneXfl) throw ToolError("LLT is only available for xfl-type planes");
                // the tasks are large objects; keep them off the worker thread's stack
                std::unique_ptr<LLTTask> pTask = std::make_unique<LLTTask>();
                LLTTask &task = *pTask;
                task.setKeepOpps(bKeepOpps);
                task.outputToStdIO(false);
                task.setObjects(pPlaneXfl, pPolar);
                task.setLLTRange(values);
                {
                    CancelHookScope cancel(ctx, [&task]{Task3d::setCancelled(true); task.cancelTask();});
                    task.initializeAnalysis();
                    task.run();
                }
                {
                    std::unique_lock<std::mutex> lck(task.m_mtx);
                    while(!task.m_theOppQueue.empty())
                    {
                        log += task.m_theOppQueue.front().message();
                        task.m_theOppQueue.pop();
                    }
                }
                for(PlaneOpp *pPOpp : task.planeOppList())
                {
                    Objects3d::insertPlaneOpp(pPOpp);
                    opps.push_back(pPOpp);
                }
                if(task.hasErrors()) log += "\nThe LLT analysis reported errors or unconverged points.\n";
            }
            else
            {
                std::unique_ptr<PlaneTask> pTask = std::make_unique<PlaneTask>();
                PlaneTask &task = *pTask;
                task.setKeepOpps(bKeepOpps);
                task.outputToStdIO(false);
                task.setObjects(pPlane, pPolar);
                task.setComputeDerivatives(argBool(args, "compute_derivatives", false) || pPolar->isStabilityPolar());
                switch(pPolar->type())
                {
                    case xfl::T6POLAR: task.setCtrlOppList(values); pPolar->resetAngleRanges(pPlane); break;
                    case xfl::T7POLAR: task.setStabOppList(values); break;
                    default:           task.setOppList(values);     break;
                }

                // relay the solver's log and progress while it runs
                std::atomic<bool> bDone{false};
                std::thread monitor([&]{
                    int nDone = 0;
                    while(!bDone.load())
                    {
                        std::string chunk;
                        int n = drainTaskLog(&task, chunk);
                        if(!chunk.empty()) {log += chunk; ctx.log(DEBUG, qs(chunk));}
                        if(n>0)
                        {
                            nDone += n;
                            ctx.progress(nDone, nValues, QString("Computed %1/%2 operating points").arg(nDone).arg(nValues));
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    }
                });
                try
                {
                    CancelHookScope cancel(ctx, [&task]{task.cancelTask();});
                    task.run();
                }
                catch(...)
                {
                    bDone = true;
                    monitor.join();
                    throw;
                }
                bDone = true;
                monitor.join();
                drainTaskLog(&task, log);

                opps = task.planeOppList(); // already stored if bKeepOpps
                if(task.hasErrors()) log += "\nThe analysis reported errors.\n";
            }
            Task3d::setCancelled(false);
            ctx.progress(nValues, nValues, "Analysis done");

            QJsonObject o = planePolarJson(pPolar, true);
            o["n_new_points"] = pPolar->dataSize()-n0;
            if(!autopolars.isEmpty()) o["foil_polars_computed"] = QJsonArray::fromStringList(autopolars);
            if(bKeepOpps)
            {
                QJsonArray jopps;
                for(PlaneOpp const *pPOpp : opps) jopps.append(planeOppJson(pPOpp));
                o["operating_points"] = jopps;
            }
            if(pPolar->dataSize()==n0) o["warning"] = "No operating point converged; check the log (include_log=true)";
            return {o, argBool(args, "include_log", false) || pPolar->dataSize()==n0 ? qs(log) : QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "plane_polar_list";
        t.title = "List plane polars";
        t.description = "Lists the plane polars, optionally filtered by plane.";
        t.inputSchema = json(R"J({"type":"object","properties":{"plane":{"type":"string"}}})J");
        t.annotations = json(R"J({"readOnlyHint":true,"openWorldHint":false})J");
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            QString planename = argString(args, "plane", QString());
            if(!planename.isEmpty()) findPlane(planename);
            QJsonArray polars;
            for(PlanePolar const *pPolar : Objects3d::planePolars())
                if(planename.isEmpty() || qs(pPolar->planeName())==planename) polars.append(planePolarJson(pPolar, false));
            return {QJsonObject{{"polars", polars}}, QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "plane_polar_get";
        t.title = "Get plane polar";
        t.description = "Returns a plane polar's specification and its result columns (CL, CD, Cm, efficiency, speeds, ...).";
        t.inputSchema = json(R"J({"type":"object","properties":{
            "plane":{"type":"string"},"polar":{"type":"string"},
            "include_data":{"type":"boolean","default":true}},
            "required":["plane","polar"]})J");
        t.annotations = json(R"J({"readOnlyHint":true,"openWorldHint":false})J");
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            Plane *pPlane = findPlane(argString(args, "plane"));
            PlanePolar *pPolar = findPlanePolar(pPlane, argString(args, "polar"));
            std::string props;
            pPolar->getProperties(props, pPlane);
            return {planePolarJson(pPolar, argBool(args, "include_data", true)), qs(props)};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "plane_polar_delete";
        t.title = "Delete plane polar";
        t.description = "Deletes a plane polar and its operating points.";
        t.inputSchema = json(R"J({"type":"object","properties":{"plane":{"type":"string"},"polar":{"type":"string"}},"required":["plane","polar"]})J");
        t.annotations = json(R"J({"destructiveHint":true,"idempotentHint":true,"openWorldHint":false})J");
        t.bMutates = true;
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            PlanePolar *pPolar = findPlanePolar(findPlane(argString(args, "plane")), argString(args, "polar"));
            QString name = qs(pPolar->name());
            Objects3d::deletePlanePolar(pPolar);
            return {QJsonObject{{"deleted", name}}, QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "plane_opp_list";
        t.title = "List plane operating points";
        t.description = "Lists the stored operating points of a plane polar with their main coefficients.";
        t.inputSchema = json(R"J({"type":"object","properties":{"plane":{"type":"string"},"polar":{"type":"string"}},"required":["plane","polar"]})J");
        t.annotations = json(R"J({"readOnlyHint":true,"openWorldHint":false})J");
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            Plane *pPlane = findPlane(argString(args, "plane"));
            PlanePolar *pPolar = findPlanePolar(pPlane, argString(args, "polar"));
            QJsonArray opps;
            for(PlaneOpp const *pPOpp : Objects3d::planeOpps())
                if(pPOpp->planeName()==pPlane->name() && pPOpp->polarName()==pPolar->name()) opps.append(planeOppJson(pPOpp));
            return {QJsonObject{{"operating_points", opps}}, QString()};
        };
        server.addTool(t);
    }

    {
        Tool t;
        t.name = "plane_opp_get";
        t.title = "Get plane operating point";
        t.description = "Returns the detailed results of a plane operating point: coefficients, centre of pressure, stability eigenvalues for T7 polars, "
                        "and flow5's full text report.";
        t.inputSchema = json(R"J({"type":"object","properties":{
            "plane":{"type":"string"},"polar":{"type":"string"},
            "value":{"type":"number","description":"Operating point variable (alpha for T1-T3, beta for T5, ctrl for T6/T7); the closest stored point is returned"}},
            "required":["plane","polar","value"]})J");
        t.annotations = json(R"J({"readOnlyHint":true,"openWorldHint":false})J");
        t.handler = [](QJsonObject const &args, Context &) -> ToolResult
        {
            Plane *pPlane = findPlane(argString(args, "plane"));
            PlanePolar *pPolar = findPlanePolar(pPlane, argString(args, "polar"));
            double target = argDouble(args, "value");

            PlaneOpp const *pBest = nullptr;
            double best = 1.e10;
            for(PlaneOpp const *pPOpp : Objects3d::planeOpps())
            {
                if(pPOpp->planeName()!=pPlane->name() || pPOpp->polarName()!=pPolar->name()) continue;
                double v = pPolar->isBetaPolar() ? pPOpp->beta() : (pPolar->isControlPolar() || pPolar->isStabilityPolar() ? pPOpp->ctrl() : pPOpp->alpha());
                if(std::fabs(v-target)<best) {best = std::fabs(v-target); pBest = pPOpp;}
            }
            if(!pBest) throw ToolError("No operating points stored for this polar; run plane_analyze with keep_operating_points=true");

            std::string props;
            pBest->getProperties(pPlane, pPolar, props);
            return {planeOppJson(pBest), qs(props)};
        };
        server.addTool(t);
    }
}

} // namespace flow5mcp
