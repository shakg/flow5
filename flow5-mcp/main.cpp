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

/**
 * @file flow5-mcp: a Model Context Protocol server which exposes flow5's
 * foil and plane design and analysis capabilities to AI assistants.
 * Communicates over stdio; see README.md for client configuration.
 */

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#ifdef WIN32
    #include <fcntl.h>
    #include <io.h>
    #include <Windows.h>
#else
    #include <unistd.h>
#endif

#include <gmsh.h>

#include <api.h>
#include <fl5core.h>
#include <flow5-io.h>
#include <units.h>

#include "mcp/mcpserver.h"
#include "tools/flow5tools.h"


namespace
{
#ifdef WIN32
    int dupFd(int fd)           {return _dup(fd);}
    int dup2Fd(int fd, int fd2) {return _dup2(fd, fd2);}
    int fdOf(FILE *f)           {return _fileno(f);}
#else
    int dupFd(int fd)           {return dup(fd);}
    int dup2Fd(int fd, int fd2) {return dup2(fd, fd2);}
    int fdOf(FILE *f)           {return fileno(f);}
#endif

    char const *s_Instructions =
        "flow5 is a potential flow solver for the preliminary design of wings, planes, hydrofoils and sails. "
        "This server keeps a project in memory: foils, 2d polars computed with XFoil, planes, and 3d plane polars "
        "computed with lifting line (LLT), vortex lattice (VLM) or 3d panel methods.\n"
        "Typical workflow: create or import foils -> foil_analyze -> plane_create (wings reference foils by name) "
        "-> plane_polar_create -> plane_analyze -> read results -> project_save to open the project in the flow5 application.\n"
        "All values are in I.S. units (m, kg, m/s) and angles in degrees. Objects are referenced by name. "
        "Call flow5_info for the list of polar types and analysis methods.";

    void printHelp()
    {
        std::cerr <<
            "flow5-mcp " << fl5::versionName(true) << "\n"
            "Model Context Protocol server for flow5, communicating over stdin/stdout.\n\n"
            "Usage: flow5-mcp [options]\n"
            "  --load <file.fl5>  load a project at startup\n"
            "  --threads <n>      number of threads used by the 3d panel solver (default: all cores)\n"
            "  --version          print the version and exit\n"
            "  --help             print this help and exit\n";
    }
}


int main(int argc, char *argv[])
{
#ifdef WIN32
    SetConsoleOutputCP(65001);
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    std::string loadpath;
    int nThreads = std::max(1, int(std::thread::hardware_concurrency()));

    for(int i=1; i<argc; i++)
    {
        if(!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
        {
            printHelp();
            return 0;
        }
        else if(!strcmp(argv[i], "--version"))
        {
            std::cout << fl5::versionName(true) << std::endl;
            return 0;
        }
        else if(!strcmp(argv[i], "--load") && i+1<argc)    loadpath = argv[++i];
        else if(!strcmp(argv[i], "--threads") && i+1<argc) nThreads = std::max(1, atoi(argv[++i]));
        else
        {
            std::cerr << "Unknown option: " << argv[i] << "\n\n";
            printHelp();
            return 1;
        }
    }

    // The flow5 libraries print progress to stdout, which is reserved for the protocol.
    // Keep a private copy of stdout for the protocol and redirect fd 1 to stderr.
    fflush(stdout);
    int protocolFd = dupFd(fdOf(stdout));
    if(protocolFd<0 || dup2Fd(fdOf(stderr), fdOf(stdout))<0)
    {
        std::cerr << "flow5-mcp: could not redirect stdout" << std::endl;
        return 1;
    }

    // I.S. units throughout
    Units::setLengthUnit(Units::M);
    Units::setSpeedUnit(Units::MS);
    Units::setAreaUnit(Units::M2);
    Units::setMassUnit(Units::KG);
    Units::setForceUnit(Units::N);
    Units::setMomentUnit(Units::NM);
    Units::setPressureUnit(Units::PA);
    Units::setInertiaUnit(Units::KGM2);
    Units::setFluidUnitType(Units::IS);
    globals::updateUnits();

    gmsh::initialize();
    gmsh::option::setNumber("General.Terminal", 0);
    gmsh::option::setNumber("General.NumThreads", nThreads);

    flow5mcp::setMaxThreads(nThreads);

    if(!loadpath.empty())
    {
        std::string log;
        if(!io::loadProject(loadpath, log))
            std::cerr << "flow5-mcp: failed to load " << loadpath << ": " << log << std::endl;
    }

    mcp::Server server("flow5-mcp", QString::fromStdString(fl5::versionName(false)), s_Instructions);
    flow5mcp::registerProjectTools(server);
    flow5mcp::registerFoilTools(server);
    flow5mcp::registerPlaneTools(server);
    flow5mcp::registerResources(server);
    flow5mcp::registerPrompts(server);

    int ret = server.run(protocolFd);

    globals::deleteObjects();
    gmsh::finalize();
    return ret;
}
