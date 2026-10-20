# flow5-mcp

A [Model Context Protocol](https://modelcontextprotocol.io) server for flow5. It lets an AI assistant (Claude Code, Claude Desktop, or any MCP client) create foils and planes, run XFoil, LLT, VLM and 3d panel analyses, and read the results.

It is a console program built on flow5-lib and flow5-io-lib. It talks JSON-RPC over stdin/stdout and keeps one flow5 project in memory. Save it with `project_save` and open the `.fl5` file in the flow5 application to view it.

## Build

flow5-mcp is part of `flow5.pro`, so it builds with the rest of flow5:

```sh
qmake flow5.pro && make
```

Or build only the server, after XFoil-lib, flow5-lib and flow5-io-lib:

```sh
cd flow5-mcp && qmake && make
```

CMake also works. Point it at the directories that hold the built or installed libraries:

```sh
cmake -S flow5-mcp -B build-mcp -DFLOW5_LIB_DIRS="/path/flow5-lib;/path/flow5-io-lib;/path/XFoil-lib"
cmake --build build-mcp
```

Dependencies are the same as flow5-io-lib's: QtCore, OpenCASCADE (7.8 or later) and gmsh. JSON is handled with QtCore, so nothing else is needed.

### Docker

The `Dockerfile` at the repository root builds the libraries and the server on Debian trixie, so nothing has to be installed on the host:

```sh
docker build -t flow5-mcp .
```

Run it with `-i` so stdin stays open. Mount a directory at the same path inside the container, so that file paths in `project_save`, `project_load` and the import/export tools mean the same thing on both sides. Pass `--user` so saved files belong to you:

```sh
claude mcp add flow5 --scope user -- docker run -i --rm --user "$(id -u):$(id -g)" \
    -v "$HOME/flow5-projects:$HOME/flow5-projects" -w "$HOME/flow5-projects" flow5-mcp
```

For Claude Desktop and other clients, set `"command": "docker"` and put the same arguments in `"args"`, with `$HOME` and the ids written out.

## Configure a client

Claude Code:

```sh
claude mcp add flow5 -- /path/to/flow5-mcp
```

Claude Desktop (`claude_desktop_config.json`):

```json
{
  "mcpServers": {
    "flow5": {
      "command": "/path/to/flow5-mcp",
      "args": ["--threads", "8"]
    }
  }
}
```

If the flow5 shared libraries are not installed system-wide, add `"env": {"LD_LIBRARY_PATH": "/path/flow5-lib:/path/flow5-io-lib:/path/XFoil-lib"}` (on macOS use `DYLD_LIBRARY_PATH`; on Windows put the DLLs next to the executable).

Command-line options:

| Option | Effect |
|---|---|
| `--load <file.fl5>` | Load a project at startup |
| `--threads <n>` | Number of threads for the 3d panel solver (default: all cores) |
| `--version`, `--help` | Print and exit |

## Conventions

- I.S. units everywhere (m, kg, m/s, kg/m³, m²/s). Angles are in degrees.
- Objects are referenced by name: foils by name, foil polars by (foil, polar), planes by name, plane polars by (plane, polar).
- Axes: x points downstream, y towards the right wing, z up.
- `flow5_info` returns the polar types and analysis methods.

## Tools

| Group | Tools |
|---|---|
| Project | `flow5_info`, `project_summary`, `project_load`, `project_save`, `project_clear`, `solver_settings` |
| Foils | `foil_list`, `foil_create_naca`, `foil_import_file`, `foil_create_from_coordinates`, `foil_get`, `foil_modify` (thickness/camber, re-panel, de-rotate, normalize, TE flap hinge), `foil_rename`, `foil_delete`, `foil_export_dat` |
| 2d analysis | `foil_polar_create`, `foil_analyze` (XFoil; T1, T2, T3, T4, T6), `foil_polar_list`, `foil_polar_get`, `foil_polar_delete`, `foil_opp_get` (Cp distributions) |
| Planes | `plane_list`, `plane_create` (wings, sections, foils, point masses), `plane_import_xml`, `plane_import_mesh` (STL/OBJ), `plane_get`, `plane_export_xml`, `plane_delete` |
| 3d analysis | `plane_polar_create` (T1, T2, T3, T5, T6, T7; LLT, VLM1, VLM2, QUADS, TRIUNIFORM, TRILINEAR; viscous, ground effect, flap angles, inertia override), `plane_polar_import_xml`, `plane_polar_export_xml`, `plane_analyze`, `plane_polar_list`, `plane_polar_get`, `plane_polar_delete`, `plane_opp_list`, `plane_opp_get` (coefficients, centre of pressure, stability eigenvalues) |

Each tool returns `structuredContent` and the same JSON as text. Some also return flow5's own text report. Errors such as an unknown foil come back as tool errors (`isError: true`) and list the valid names, so the model can fix the call.

How the analyses behave:

- `foil_analyze` splits an alpha range that crosses zero into two runs that both start near zero. XFoil converges better that way.
- `plane_analyze` builds the mesh and wake for the polar the same way flow5's batch runner does.
- LLT, and viscous drag interpolated from foil polars, need 2d polars covering the wing's Reynolds range. By default `plane_analyze` computes any missing ones first (`auto_foil_polars`).
- `foil_analyze` and `plane_analyze` send progress notifications when the client passes a `progressToken`. Both stop when the client sends `notifications/cancelled`.

## Resources

| URI | Content |
|---|---|
| `flow5://project` | Project summary (JSON) |
| `flow5://foils/{foil}` | Foil properties and coordinates (JSON) |
| `flow5://foils/{foil}/dat` | Coordinates in Selig .dat format |
| `flow5://foils/{foil}/polars/{polar}` | 2d polar results (CSV) |
| `flow5://planes/{plane}` | Plane definition (JSON) |
| `flow5://planes/{plane}/polars/{polar}` | Plane polar results (CSV) |

Names are percent-encoded in URIs. Template arguments support `completion/complete`. The server sends `notifications/resources/list_changed` after every tool that changes the project.

## Prompts

`airfoil_study`, `compare_airfoils`, `plane_performance`, `design_glider`: ready-made workflows that chain the tools.

## Protocol

- MCP revisions 2025-06-18 (preferred), 2025-03-26 and 2024-11-05, over stdio with newline-delimited JSON-RPC 2.0.
- Capabilities: tools, resources (list, templates, read, list_changed), prompts, completions, logging (`logging/setLevel`, `notifications/message`), progress, cancellation and ping.
- One thread reads stdin and answers `ping` and cancellations right away. A single worker thread runs every other request in order, because flow5's object store is global.
- flow5 prints solver output to stdout. The server moves that output to stderr, so stdout only ever carries protocol messages.

## Tests

```sh
python3 flow5-mcp/test/mcp_smoke_test.py /path/to/flow5-mcp
```

The script drives a full session through the protocol: NACA foils, XFoil polars, a three-surface glider, VLM, viscous T2 panel and LLT analyses, resources, prompts, completions, progress, cancellation, project save/load and an XML round trip. It takes about 90 s.

## Limitations

- Boats and sails are not exposed.
- T4 and T8 plane polars are not supported.
- Fuselages come only from XML import or a loaded project. There is no tool for gmsh conforming fuselage meshes.
- Analyses run one at a time.
