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

#include <algorithm>
#include <cmath>

#include <api.h>
#include <foil.h>
#include <objects2d.h>
#include <objects3d.h>
#include <oppoint.h>
#include <plane.h>
#include <planeopp.h>
#include <planepolar.h>
#include <planexfl.h>
#include <trimesh.h>
#include <polar.h>
#include <wingxfl.h>

#include "flow5tools.h"

using namespace mcp;


namespace
{
    QJsonArray toArray(std::vector<double> const &v)
    {
        QJsonArray a;
        for(double d : v) a.append(std::isfinite(d) ? QJsonValue(d) : QJsonValue(QJsonValue::Null));
        return a;
    }

    QJsonArray vec3(Vector3d const &v)
    {
        return QJsonArray{v.x, v.y, v.z};
    }

    QString csvField(std::string const &s)
    {
        QString f = qs(s);
        if(f.contains(',') || f.contains('"')) f = '"' + f.replace("\"", "\"\"") + '"';
        return f;
    }

    struct PolarColumn
    {
        char const *name;
        std::vector<double> Polar::*data;
    };

    PolarColumn const s_PolarColumns[] =
    {
        {"alpha",    &Polar::m_Alpha},
        {"Cl",       &Polar::m_Cl},
        {"Cd",       &Polar::m_Cd},
        {"Cdp",      &Polar::m_Cdp},
        {"Cm",       &Polar::m_Cm},
        {"Cl/Cd",    &Polar::m_ClCd},
        {"Cl^1.5/Cd",&Polar::m_Cl32Cd},
        {"XTr_top",  &Polar::m_XTrTop},
        {"XTr_bot",  &Polar::m_XTrBot},
        {"Cp_min",   &Polar::m_Cpmn},
        {"HMom",     &Polar::m_HMom},
        {"XCp",      &Polar::m_XCp},
        {"Re",       &Polar::m_Re},
        {"Ctrl",     &Polar::m_Control},
    };
}


namespace flow5mcp
{

Foil *findFoil(QString const &name)
{
    Foil *pFoil = Objects2d::foil(name.toStdString());
    if(pFoil) return pFoil;

    QStringList names;
    for(Foil const *pF : Objects2d::foils()) names.append(qs(pF->name()));
    throw ToolError("Foil not found: \""+name+"\". Available foils: "+(names.isEmpty() ? "none" : names.join(", ")));
}


Polar *findPolar(Foil const *pFoil, QString const &name)
{
    Polar *pPolar = Objects2d::polar(pFoil, name.toStdString());
    if(pPolar) return pPolar;

    QStringList names;
    for(Polar const *pP : Objects2d::polars())
        if(pP->foilName()==pFoil->name()) names.append(qs(pP->name()));
    throw ToolError("Polar not found: \""+name+"\" for foil \""+qs(pFoil->name())+"\". Available polars: "+(names.isEmpty() ? "none" : names.join(", ")));
}


Plane *findPlane(QString const &name)
{
    Plane *pPlane = Objects3d::plane(name.toStdString());
    if(pPlane) return pPlane;

    QStringList names;
    for(Plane const *pP : Objects3d::planes()) names.append(qs(pP->name()));
    throw ToolError("Plane not found: \""+name+"\". Available planes: "+(names.isEmpty() ? "none" : names.join(", ")));
}


PlanePolar *findPlanePolar(Plane const *pPlane, QString const &name)
{
    PlanePolar *pPolar = Objects3d::wPolar(pPlane, name.toStdString());
    if(pPolar) return pPolar;

    QStringList names;
    for(PlanePolar const *pP : Objects3d::planePolars())
        if(pP->planeName()==pPlane->name()) names.append(qs(pP->name()));
    throw ToolError("Plane polar not found: \""+name+"\" for plane \""+qs(pPlane->name())+"\". Available polars: "+(names.isEmpty() ? "none" : names.join(", ")));
}


QString polarTypeName(xfl::enumPolarType type)
{
    switch(type)
    {
        case xfl::T1POLAR: return "T1";
        case xfl::T2POLAR: return "T2";
        case xfl::T3POLAR: return "T3";
        case xfl::T4POLAR: return "T4";
        case xfl::T5POLAR: return "T5";
        case xfl::T6POLAR: return "T6";
        case xfl::T7POLAR: return "T7";
        case xfl::T8POLAR: return "T8";
        case xfl::BOATPOLAR: return "BOAT";
        case xfl::EXTERNALPOLAR: return "EXTERNAL";
    }
    return "UNKNOWN";
}


xfl::enumPolarType parsePolarType(QString const &name)
{
    QString n = name.toUpper();
    if(n=="T1") return xfl::T1POLAR;
    if(n=="T2") return xfl::T2POLAR;
    if(n=="T3") return xfl::T3POLAR;
    if(n=="T4") return xfl::T4POLAR;
    if(n=="T5") return xfl::T5POLAR;
    if(n=="T6") return xfl::T6POLAR;
    if(n=="T7") return xfl::T7POLAR;
    if(n=="T8") return xfl::T8POLAR;
    throw ToolError("Invalid polar type: "+name);
}


QString analysisMethodName(xfl::enumAnalysisMethod method)
{
    switch(method)
    {
        case xfl::LLT:        return "LLT";
        case xfl::VLM1:       return "VLM1";
        case xfl::VLM2:       return "VLM2";
        case xfl::QUADS:      return "QUADS";
        case xfl::TRILINEAR:  return "TRILINEAR";
        case xfl::TRIUNIFORM: return "TRIUNIFORM";
        case xfl::NOMETHOD:   return "NONE";
    }
    return "NONE";
}


xfl::enumAnalysisMethod parseAnalysisMethod(QString const &name)
{
    QString n = name.toUpper();
    if(n=="LLT")        return xfl::LLT;
    if(n=="VLM1")       return xfl::VLM1;
    if(n=="VLM2")       return xfl::VLM2;
    if(n=="QUADS")      return xfl::QUADS;
    if(n=="TRILINEAR")  return xfl::TRILINEAR;
    if(n=="TRIUNIFORM") return xfl::TRIUNIFORM;
    throw ToolError("Invalid analysis method: "+name);
}


std::vector<double> parseValues(QJsonObject const &args)
{
    std::vector<double> values;

    if(args.contains("values"))
    {
        for(QJsonValue const &v : argArray(args, "values"))
        {
            if(!v.isDouble()) throw ToolError("values must be an array of numbers");
            values.push_back(v.toDouble());
        }
    }

    QJsonArray ranges;
    if(args.contains("range"))  ranges.append(args.value("range"));
    if(args.contains("ranges")) for(QJsonValue const &v : argArray(args, "ranges")) ranges.append(v);

    for(QJsonValue const &rv : ranges)
    {
        QJsonObject r = rv.toObject();
        double start = argDouble(r, "start");
        double end   = argDouble(r, "end", start);
        double step  = std::fabs(argDouble(r, "step", 1.0));
        if(step<1.e-9 || std::fabs(end-start)<1.e-9)
        {
            values.push_back(start);
            continue;
        }
        int n = int(std::floor(std::fabs(end-start)/step + 1.e-6))+1;
        if(n>10000) throw ToolError("Range contains too many values");
        double dir = end>start ? 1.0 : -1.0;
        for(int i=0; i<n; i++) values.push_back(start + dir*double(i)*step);
    }

    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end(), [](double a, double b){return std::fabs(a-b)<1.e-6;}), values.end());

    if(values.empty()) throw ToolError("No operating point values specified: provide \"values\", \"range\" or \"ranges\"");
    return values;
}


QJsonObject foilJson(Foil const *pFoil, bool bCoordinates)
{
    QJsonObject o;
    o["name"]          = qs(pFoil->name());
    if(!pFoil->description().empty()) o["description"] = qs(pFoil->description());
    o["thickness"]     = pFoil->maxThickness();
    o["x_thickness"]   = pFoil->xThickness();
    o["camber"]        = pFoil->maxCamber();
    o["x_camber"]      = pFoil->xCamber();
    o["te_gap"]        = pFoil->TEGap();
    o["n_panels"]      = pFoil->nNodes()-1;
    o["has_te_flap"]   = pFoil->hasTEFlap();
    if(pFoil->hasTEFlap())
    {
        o["te_flap"] = QJsonObject{{"x_hinge", pFoil->TEXHinge()}, {"y_hinge", pFoil->TEYHinge()}, {"angle", pFoil->TEFlapAngle()}};
    }
    o["has_le_flap"]   = pFoil->hasLEFlap();

    int nPolars=0;
    for(Polar const *pPolar : Objects2d::polars()) if(pPolar->foilName()==pFoil->name()) nPolars++;
    o["n_polars"] = nPolars;

    if(bCoordinates)
    {
        QJsonArray pts;
        for(int i=0; i<pFoil->nNodes(); i++) pts.append(QJsonArray{pFoil->x(i), pFoil->y(i)});
        o["coordinates"] = pts;
    }
    return o;
}


QJsonObject polarJson(Polar const *pPolar, bool bData)
{
    QJsonObject o;
    o["name"]     = qs(pPolar->name());
    o["foil"]     = qs(pPolar->foilName());
    o["type"]     = polarTypeName(pPolar->type());
    if(pPolar->isType4()) o["alpha"] = pPolar->aoaSpec();
    else                  o["reynolds"] = pPolar->Reynolds();
    o["mach"]     = pPolar->Mach();
    o["ncrit"]    = pPolar->NCrit();
    o["xtr_top"]  = pPolar->XTripTop();
    o["xtr_bot"]  = pPolar->XTripBot();
    o["te_flap_angle"] = pPolar->TEFlapAngle();
    o["n_points"] = pPolar->dataSize();

    if(pPolar->hasData())
    {
        double amin=0, amax=0, clmin=0, clmax=0;
        pPolar->getAlphaLimits(amin, amax);
        pPolar->getClLimits(clmin, clmax);
        int imax = int(std::max_element(pPolar->m_ClCd.begin(), pPolar->m_ClCd.end()) - pPolar->m_ClCd.begin());
        o["summary"] = QJsonObject{
            {"alpha_min", amin}, {"alpha_max", amax},
            {"cl_min", clmin}, {"cl_max", clmax},
            {"max_cl_cd", pPolar->m_ClCd.at(imax)}, {"alpha_at_max_cl_cd", pPolar->m_Alpha.at(imax)}
        };
    }

    if(bData)
    {
        QJsonObject data;
        for(PolarColumn const &c : s_PolarColumns) data[c.name] = toArray(pPolar->*c.data);
        o["data"] = data;
    }
    return o;
}


QString polarCsv(Polar const *pPolar)
{
    QStringList header;
    for(PolarColumn const &c : s_PolarColumns) header.append(c.name);
    QString csv = header.join(',') + '\n';
    for(int i=0; i<pPolar->dataSize(); i++)
    {
        QStringList row;
        for(PolarColumn const &c : s_PolarColumns)
        {
            std::vector<double> const &v = pPolar->*c.data;
            row.append(i<int(v.size()) ? QString::number(v.at(i), 'g', 8) : QString());
        }
        csv += row.join(',') + '\n';
    }
    return csv;
}


QJsonObject opPointJson(OpPoint const *pOpp, bool bDistributions)
{
    QJsonObject o;
    o["foil"]      = qs(pOpp->foilName());
    o["polar"]     = qs(pOpp->polarName());
    o["alpha"]     = pOpp->aoa();
    o["reynolds"]  = pOpp->Reynolds();
    o["Cl"]        = pOpp->m_Cl;
    o["Cd"]        = pOpp->m_Cd;
    o["Cm"]        = pOpp->m_Cm;
    o["XTr_top"]   = pOpp->m_XTrTop;
    o["XTr_bot"]   = pOpp->m_XTrBot;
    o["Cp_min"]    = pOpp->m_Cpmn;
    o["viscous"]   = pOpp->m_bViscResults;

    if(bDistributions)
    {
        Foil const *pFoil = Objects2d::foil(pOpp->foilName());
        if(pFoil && pFoil->nNodes()==pOpp->nPoints())
        {
            QJsonArray x, y;
            for(int i=0; i<pFoil->nNodes(); i++) {x.append(pFoil->x(i)); y.append(pFoil->y(i));}
            o["x"] = x;
            o["y"] = y;
        }
        o["Cp_viscous"]   = toArray(pOpp->m_Cpv);
        o["Cp_inviscid"]  = toArray(pOpp->m_Cpi);
    }
    return o;
}


QJsonObject planeJson(Plane const *pPlane)
{
    QJsonObject o;
    o["name"]           = qs(pPlane->name());
    if(!pPlane->description().empty()) o["description"] = qs(pPlane->description());
    o["kind"]           = pPlane->isXflType() ? "xfl" : "stl";
    o["mac"]            = pPlane->mac();
    o["span"]           = pPlane->span();
    o["planform_area"]  = pPlane->planformArea();
    o["projected_area"] = pPlane->projectedArea();
    o["aspect_ratio"]   = pPlane->aspectRatio();
    o["taper_ratio"]    = pPlane->taperRatio();
    o["total_mass"]     = pPlane->totalMass();
    o["cog"]            = vec3(pPlane->CoG_t());
    o["tail_volume_h"]  = pPlane->tailVolumeHorizontal();
    o["tail_volume_v"]  = pPlane->tailVolumeVertical();
    o["n_triangles"]    = pPlane->triMesh().nPanels();

    PlaneXfl const *pPlaneXfl = dynamic_cast<PlaneXfl const*>(pPlane);
    if(pPlaneXfl)
    {
        QJsonArray wings;
        for(int iw=0; iw<pPlaneXfl->nWings(); iw++)
        {
            WingXfl const *pWing = pPlaneXfl->wingAt(iw);
            QJsonObject w;
            w["name"]     = qs(pWing->name());
            switch(pWing->wingType())
            {
                case xfl::Main:      w["type"] = "main";      break;
                case xfl::Elevator:  w["type"] = "elevator";  break;
                case xfl::Fin:       w["type"] = "fin";       break;
                default:             w["type"] = "other";     break;
            }
            w["position"]       = vec3(pWing->position());
            w["rx"]             = pWing->rx();
            w["ry"]             = pWing->ry();
            w["planform_span"]  = pWing->planformSpan();
            w["planform_area"]  = pWing->planformArea();
            w["mac"]            = pWing->MAC();
            w["aspect_ratio"]   = pWing->aspectRatio();
            w["n_flaps"]        = pWing->nFlaps();
            w["two_sided"]      = pWing->isTwoSided();
            QJsonArray sections;
            for(int is=0; is<pWing->nSections(); is++)
            {
                WingSection const &sec = pWing->section(is);
                sections.append(QJsonObject{
                    {"y", sec.yPosition()}, {"chord", sec.chord()}, {"offset", sec.offset()},
                    {"dihedral", sec.dihedral()}, {"twist", sec.twist()},
                    {"left_foil", qs(sec.leftFoilName())}, {"right_foil", qs(sec.rightFoilName())},
                    {"nx", sec.nXPanels()}, {"ny", sec.nYPanels()}
                });
            }
            w["sections"] = sections;
            wings.append(w);
        }
        o["wings"] = wings;
        o["n_fuselages"] = pPlaneXfl->nFuse();
    }

    QJsonArray masses;
    for(PointMass const &pm : pPlane->pointMassList())
        masses.append(QJsonObject{{"tag", qs(pm.tag())}, {"mass", pm.mass()}, {"position", vec3(pm.position())}});
    o["point_masses"] = masses;

    int nPolars=0;
    for(PlanePolar const *pPolar : Objects3d::planePolars()) if(pPolar->planeName()==pPlane->name()) nPolars++;
    o["n_polars"] = nPolars;
    return o;
}


QJsonObject planePolarJson(PlanePolar const *pPolar, bool bData)
{
    QJsonObject o;
    o["name"]          = qs(pPolar->name());
    o["plane"]         = qs(pPolar->planeName());
    o["type"]          = polarTypeName(pPolar->type());
    o["method"]        = analysisMethodName(pPolar->analysisMethod());
    o["velocity"]      = pPolar->velocity();
    o["alpha"]         = pPolar->alphaSpec();
    o["beta"]          = pPolar->betaSpec();
    o["mass"]          = pPolar->mass();
    o["cog"]           = vec3(pPolar->CoG());
    o["viscous"]       = pPolar->isViscous();
    o["viscous_on_the_fly"] = pPolar->isViscOnTheFly();
    o["thin_surfaces"] = pPolar->bThinSurfaces();
    o["density"]       = pPolar->density();
    o["viscosity"]     = pPolar->viscosity();
    o["ground_effect"] = pPolar->bGroundEffect();
    o["reference_area"]  = pPolar->referenceArea();
    o["reference_span"]  = pPolar->referenceSpanLength();
    o["reference_chord"] = pPolar->referenceChordLength();
    o["n_points"]      = pPolar->dataSize();

    if(bData)
    {
        QJsonObject data;
        for(int iv=0; iv<PlanePolar::variableCount(); iv++)
        {
            QJsonArray col;
            for(int i=0; i<pPolar->dataSize(); i++)
            {
                double d = pPolar->getVariable(iv, i);
                col.append(std::isfinite(d) ? QJsonValue(d) : QJsonValue(QJsonValue::Null));
            }
            data[qs(PlanePolar::variableName(iv))] = col;
        }
        o["data"] = data;
    }
    return o;
}


QString planePolarCsv(PlanePolar const *pPolar)
{
    QStringList header;
    for(int iv=0; iv<PlanePolar::variableCount(); iv++) header.append(csvField(PlanePolar::variableName(iv)));
    QString csv = header.join(',') + '\n';
    for(int i=0; i<pPolar->dataSize(); i++)
    {
        QStringList row;
        for(int iv=0; iv<PlanePolar::variableCount(); iv++) row.append(QString::number(pPolar->getVariable(iv, i), 'g', 8));
        csv += row.join(',') + '\n';
    }
    return csv;
}


QJsonObject planeOppJson(PlaneOpp const *pPOpp)
{
    AeroForces const &af = pPOpp->aeroForces();
    QJsonObject o;
    o["name"]     = qs(pPOpp->name());
    o["plane"]    = qs(pPOpp->planeName());
    o["polar"]    = qs(pPOpp->polarName());
    o["alpha"]    = pPOpp->alpha();
    o["beta"]     = pPOpp->beta();
    o["phi"]      = pPOpp->phi();
    o["ctrl"]     = pPOpp->ctrl();
    o["velocity"] = pPOpp->QInf();
    o["mass"]     = pPOpp->mass();
    o["CL"]       = af.CL();
    o["CD"]       = af.CD();
    o["CDi"]      = af.CDi();
    o["CDv"]      = af.CDv();
    o["CY"]       = af.CSide();
    o["Cl"]       = af.Cli();
    o["Cm"]       = af.Cm();
    o["Cn"]       = af.Cn();
    o["CL_CD"]    = std::fabs(af.CD())>1.e-12 ? af.CL()/af.CD() : 0.0;
    o["centre_of_pressure"] = vec3(af.centreOfPressure());
    o["viscous_interpolation_out_of_range"] = pPOpp->isOut();

    if(pPOpp->isType7())
    {
        QJsonArray modes;
        for(int i=0; i<8; i++)
        {
            std::complex<double> ev = pPOpp->eigValue(i);
            modes.append(QJsonObject{{"re", ev.real()}, {"im", ev.imag()}, {"group", i<4 ? "longitudinal" : "lateral"}});
        }
        o["eigenvalues"] = modes;
    }
    return o;
}


QJsonObject projectJson()
{
    QJsonArray foils;
    for(Foil const *pFoil : Objects2d::foils())
    {
        QJsonArray polars;
        for(Polar const *pPolar : Objects2d::polars())
        {
            if(pPolar->foilName()!=pFoil->name()) continue;
            polars.append(QJsonObject{{"name", qs(pPolar->name())}, {"type", polarTypeName(pPolar->type())}, {"n_points", pPolar->dataSize()}});
        }
        foils.append(QJsonObject{{"name", qs(pFoil->name())}, {"polars", polars}});
    }

    QJsonArray planes;
    for(Plane const *pPlane : Objects3d::planes())
    {
        QJsonArray polars;
        for(PlanePolar const *pPolar : Objects3d::planePolars())
        {
            if(pPolar->planeName()!=pPlane->name()) continue;
            polars.append(QJsonObject{{"name", qs(pPolar->name())}, {"type", polarTypeName(pPolar->type())},
                                      {"method", analysisMethodName(pPolar->analysisMethod())}, {"n_points", pPolar->dataSize()}});
        }
        planes.append(QJsonObject{{"name", qs(pPlane->name())}, {"kind", pPlane->isXflType() ? "xfl" : "stl"}, {"polars", polars}});
    }

    return QJsonObject{
        {"foils", foils},
        {"planes", planes},
        {"n_foil_operating_points", Objects2d::nOpPoints()},
        {"n_plane_operating_points", Objects3d::nPOpps()}
    };
}


void clearProject()
{
    globals::deleteObjects();
}

} // namespace flow5mcp
