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

#include <QJsonDocument>
#include <QUrl>

#include <foil.h>
#include <objects2d.h>
#include <objects3d.h>
#include <plane.h>
#include <planepolar.h>
#include <polar.h>

#include "flow5tools.h"

using namespace mcp;


namespace
{
    QString enc(std::string const &s)
    {
        return QString::fromUtf8(QUrl::toPercentEncoding(qs(s)));
    }

    QString pretty(QJsonObject const &o)
    {
        return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
    }

    QStringList filter(QStringList const &names, QString const &prefix)
    {
        QStringList out;
        for(QString const &n : names) if(n.startsWith(prefix, Qt::CaseInsensitive)) out.append(n);
        return out;
    }

    QStringList foilNames()
    {
        QStringList l;
        for(Foil const *pFoil : Objects2d::foils()) l.append(qs(pFoil->name()));
        return l;
    }

    QStringList planeNames()
    {
        QStringList l;
        for(Plane const *pPlane : Objects3d::planes()) l.append(qs(pPlane->name()));
        return l;
    }

    QStringList foilPolarNames(QString const &foil)
    {
        QStringList l;
        for(Polar const *pPolar : Objects2d::polars()) if(qs(pPolar->foilName())==foil) l.append(qs(pPolar->name()));
        return l;
    }

    QStringList planePolarNames(QString const &plane)
    {
        QStringList l;
        for(PlanePolar const *pPolar : Objects3d::planePolars()) if(qs(pPolar->planeName())==plane) l.append(qs(pPolar->name()));
        return l;
    }

    QString var(std::map<QString, QString> const &vars, QString const &key)
    {
        auto it = vars.find(key);
        return it==vars.end() ? QString() : it->second;
    }
}


namespace flow5mcp
{

void registerResources(Server &server)
{
    {
        ResourceTemplate t;
        t.uriTemplate = "flow5://project";
        t.name = "project";
        t.title = "Project summary";
        t.description = "All foils, planes and polars in memory";
        t.mimeType = "application/json";
        t.list = []{ return std::vector<ResourceInfo>{{"flow5://project", "project", "Project summary", "All foils, planes and polars in memory", "application/json"}}; };
        t.read = [](std::map<QString, QString> const &) -> ResourceContent { return {"application/json", pretty(projectJson())}; };
        server.addResourceTemplate(t);
    }

    {
        ResourceTemplate t;
        t.uriTemplate = "flow5://foils/{foil}";
        t.name = "foil";
        t.title = "Foil";
        t.description = "A foil's properties and coordinates";
        t.mimeType = "application/json";
        t.list = []{
            std::vector<ResourceInfo> l;
            for(Foil const *pFoil : Objects2d::foils())
                l.push_back({"flow5://foils/"+enc(pFoil->name()), qs(pFoil->name()), qs(pFoil->name()), "Foil properties and coordinates", "application/json"});
            return l;
        };
        t.read = [](std::map<QString, QString> const &vars) -> ResourceContent {
            return {"application/json", pretty(foilJson(findFoil(var(vars, "foil")), true))};
        };
        t.complete = [](QString const &, QString const &prefix, std::map<QString, QString> const &) { return filter(foilNames(), prefix); };
        server.addResourceTemplate(t);
    }

    {
        ResourceTemplate t;
        t.uriTemplate = "flow5://foils/{foil}/dat";
        t.name = "foil_dat";
        t.title = "Foil coordinates (.dat)";
        t.description = "A foil's coordinates in Selig .dat format";
        t.mimeType = "text/plain";
        t.read = [](std::map<QString, QString> const &vars) -> ResourceContent {
            std::string dat;
            findFoil(var(vars, "foil"))->exportFoilToDat(dat);
            return {"text/plain", qs(dat)};
        };
        t.complete = [](QString const &, QString const &prefix, std::map<QString, QString> const &) { return filter(foilNames(), prefix); };
        server.addResourceTemplate(t);
    }

    {
        ResourceTemplate t;
        t.uriTemplate = "flow5://foils/{foil}/polars/{polar}";
        t.name = "foil_polar";
        t.title = "Foil polar (CSV)";
        t.description = "A 2d polar's results as CSV";
        t.mimeType = "text/csv";
        t.list = []{
            std::vector<ResourceInfo> l;
            for(Polar const *pPolar : Objects2d::polars())
                l.push_back({"flow5://foils/"+enc(pPolar->foilName())+"/polars/"+enc(pPolar->name()),
                             qs(pPolar->foilName()+" / "+pPolar->name()), QString(),
                             QString("%1 points").arg(pPolar->dataSize()), "text/csv"});
            return l;
        };
        t.read = [](std::map<QString, QString> const &vars) -> ResourceContent {
            return {"text/csv", polarCsv(findPolar(findFoil(var(vars, "foil")), var(vars, "polar")))};
        };
        t.complete = [](QString const &v, QString const &prefix, std::map<QString, QString> const &ctx) {
            if(v=="polar") return filter(foilPolarNames(var(ctx, "foil")), prefix);
            return filter(foilNames(), prefix);
        };
        server.addResourceTemplate(t);
    }

    {
        ResourceTemplate t;
        t.uriTemplate = "flow5://planes/{plane}";
        t.name = "plane";
        t.title = "Plane";
        t.description = "A plane's geometry, inertia and reference dimensions";
        t.mimeType = "application/json";
        t.list = []{
            std::vector<ResourceInfo> l;
            for(Plane const *pPlane : Objects3d::planes())
                l.push_back({"flow5://planes/"+enc(pPlane->name()), qs(pPlane->name()), qs(pPlane->name()), "Plane definition", "application/json"});
            return l;
        };
        t.read = [](std::map<QString, QString> const &vars) -> ResourceContent {
            return {"application/json", pretty(planeJson(findPlane(var(vars, "plane"))))};
        };
        t.complete = [](QString const &, QString const &prefix, std::map<QString, QString> const &) { return filter(planeNames(), prefix); };
        server.addResourceTemplate(t);
    }

    {
        ResourceTemplate t;
        t.uriTemplate = "flow5://planes/{plane}/polars/{polar}";
        t.name = "plane_polar";
        t.title = "Plane polar (CSV)";
        t.description = "A plane polar's results as CSV";
        t.mimeType = "text/csv";
        t.list = []{
            std::vector<ResourceInfo> l;
            for(PlanePolar const *pPolar : Objects3d::planePolars())
                l.push_back({"flow5://planes/"+enc(pPolar->planeName())+"/polars/"+enc(pPolar->name()),
                             qs(pPolar->planeName()+" / "+pPolar->name()), QString(),
                             QString("%1 points").arg(pPolar->dataSize()), "text/csv"});
            return l;
        };
        t.read = [](std::map<QString, QString> const &vars) -> ResourceContent {
            return {"text/csv", planePolarCsv(findPlanePolar(findPlane(var(vars, "plane")), var(vars, "polar")))};
        };
        t.complete = [](QString const &v, QString const &prefix, std::map<QString, QString> const &ctx) {
            if(v=="polar") return filter(planePolarNames(var(ctx, "plane")), prefix);
            return filter(planeNames(), prefix);
        };
        server.addResourceTemplate(t);
    }
}


void registerPrompts(Server &server)
{
    auto userText = [](QString const &text) {
        return QJsonArray{QJsonObject{{"role", "user"}, {"content", QJsonObject{{"type", "text"}, {"text", text}}}}};
    };

    {
        Prompt p;
        p.name = "airfoil_study";
        p.title = "Airfoil study";
        p.description = "Analyze one airfoil at a given Reynolds number and summarize its performance";
        p.arguments = {{"foil", "Foil name, or a NACA designation such as 2412", true},
                       {"reynolds", "Reynolds number, e.g. 200000", true}};
        p.messages = [userText](QJsonObject const &args) {
            QString foil = args.value("foil").toString();
            QString re   = args.value("reynolds").toString();
            return userText(QString(
                "Study the airfoil \"%1\" at Re=%2 using the flow5 tools.\n"
                "1. If the foil does not exist (check foil_list), create it with foil_create_naca if it is a NACA designation.\n"
                "2. Run foil_analyze with a T1 polar at Re=%2, Mach 0, Ncrit 9, alpha from -6 to 14 deg in 0.5 deg steps.\n"
                "3. Report: zero-lift angle, lift slope, Cl max and stall angle, min Cd, max Cl/Cd and the angle where it occurs, Cm at zero lift.\n"
                "4. Flag unconverged ranges and comment on the transition locations.").arg(foil, re));
        };
        p.complete = [](QString const &arg, QString const &prefix) {
            return arg=="foil" ? filter(foilNames(), prefix) : QStringList();
        };
        server.addPrompt(p);
    }

    {
        Prompt p;
        p.name = "compare_airfoils";
        p.title = "Compare airfoils";
        p.description = "Compare several airfoils at the same Reynolds number";
        p.arguments = {{"foils", "Comma-separated foil names or NACA designations", true},
                       {"reynolds", "Reynolds number", true}};
        p.messages = [userText](QJsonObject const &args) {
            return userText(QString(
                "Compare the airfoils %1 at Re=%2 using the flow5 tools.\n"
                "Create any missing NACA foils, run the same T1 polar (alpha -5 to 15 deg, step 0.5) for each with foil_analyze, "
                "then tabulate Cl max, alpha at Cl max, min Cd, max Cl/Cd, Cm0 and thickness for each foil, "
                "and recommend one for a low-speed wing, explaining the trade-offs.").arg(args.value("foils").toString(), args.value("reynolds").toString()));
        };
        server.addPrompt(p);
    }

    {
        Prompt p;
        p.name = "plane_performance";
        p.title = "Plane performance";
        p.description = "Compute and summarize the trimmed performance and static stability of a plane";
        p.arguments = {{"plane", "Plane name", true}};
        p.messages = [userText](QJsonObject const &args) {
            return userText(QString(
                "Evaluate the plane \"%1\" with the flow5 tools.\n"
                "1. Inspect it with plane_get (mass, CoG, wings).\n"
                "2. Create a T2 viscous TRIUNIFORM plane polar with plane_polar_create, then run plane_analyze for alpha from -4 to 12 deg, step 1.\n"
                "3. From the results, find the trimmed angle (Cm=0), its speed and CL, the max glide ratio, the minimum sink speed, "
                "and the static margin from the slope of Cm vs CL.\n"
                "4. Say whether the plane is statically stable and suggest changes if it is not.").arg(args.value("plane").toString()));
        };
        p.complete = [](QString const &arg, QString const &prefix) {
            return arg=="plane" ? filter(planeNames(), prefix) : QStringList();
        };
        server.addPrompt(p);
    }

    {
        Prompt p;
        p.name = "design_glider";
        p.title = "Design a glider";
        p.description = "Design a simple conventional glider from a span and mass, then analyze it";
        p.arguments = {{"span", "Wing span in m", true}, {"mass", "Total mass in kg", true}};
        p.messages = [userText](QJsonObject const &args) {
            return userText(QString(
                "Design a conventional glider with a %1 m span and a total mass of %2 kg using the flow5 tools.\n"
                "Choose suitable foils (create NACA foils, e.g. 2412 for the wing and 0009 for the tail), "
                "size the main wing for a wing loading appropriate to the mass, size the elevator and fin with tail volumes around 0.5 and 0.03, "
                "and place point masses so that the static margin is 5-15%. "
                "Create the plane with plane_create, then run a T2 analysis and iterate on the CoG or elevator incidence until the plane trims "
                "at a positive CL near the max glide ratio. Report the final geometry and performance.").arg(args.value("span").toString(), args.value("mass").toString()));
        };
        server.addPrompt(p);
    }
}

} // namespace flow5mcp
