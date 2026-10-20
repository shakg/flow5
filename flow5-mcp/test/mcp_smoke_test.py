#!/usr/bin/env python3
"""
End-to-end test of flow5-mcp over stdio.

Usage: python3 mcp_smoke_test.py /path/to/flow5-mcp

Runs a complete workflow: foils -> XFoil polars -> plane -> 3d analyses -> resources,
prompts, completions, cancellation, project save/load. Exits non-zero on failure.
"""

import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import queue


class Client:
    def __init__(self, exe):
        self.proc = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.next_id = 0
        self.responses = {}
        self.notifications = []
        self.cv = threading.Condition()
        self.eof = False
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.proc.stdout:
            msg = json.loads(line)  # any non-JSON output on stdout is a protocol violation
            with self.cv:
                if "id" in msg and ("result" in msg or "error" in msg):
                    self.responses[msg["id"]] = msg
                else:
                    self.notifications.append(msg)
                self.cv.notify_all()
        with self.cv:
            self.eof = True
            self.cv.notify_all()

    def send(self, msg):
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()

    def request_async(self, method, params=None):
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            msg["params"] = params
        self.send(msg)
        return self.next_id

    def wait(self, rid, timeout=600):
        with self.cv:
            ok = self.cv.wait_for(lambda: rid in self.responses or self.eof, timeout)
            assert rid in self.responses, "server exited" if self.eof else f"timeout waiting for response {rid}"
            return self.responses.pop(rid)

    def request(self, method, params=None, timeout=600):
        return self.wait(self.request_async(method, params), timeout)

    def notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self.send(msg)

    def call(self, name, args=None, expect_error=False, meta=None):
        params = {"name": name, "arguments": args or {}}
        if meta:
            params["_meta"] = meta
        r = self.request("tools/call", params)
        assert "result" in r, f"{name}: protocol error {r}"
        res = r["result"]
        if expect_error:
            assert res["isError"], f"{name}: expected a tool error, got {res}"
            return res["content"][0]["text"]
        assert not res["isError"], f"{name}: {res['content'][0]['text']}"
        return res["structuredContent"]

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=60)
        return self.proc.returncode


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)
    print("  ok:", msg)


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else "./flow5-mcp"
    c = Client(exe)
    t0 = time.time()

    print("protocol")
    r = c.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                 "clientInfo": {"name": "smoke", "version": "1"}})
    check(r["result"]["protocolVersion"] == "2025-06-18", "protocol version negotiated")
    c.notify("notifications/initialized")
    check(c.request("ping")["result"] == {}, "ping")
    check(c.request("bogus/method")["error"]["code"] == -32601, "unknown method rejected")
    check(c.request("tools/call", {"name": "nope"})["error"]["code"] == -32602, "unknown tool rejected")
    check(c.request("logging/setLevel", {"level": "info"})["result"] == {}, "logging/setLevel")
    tools = c.request("tools/list")["result"]["tools"]
    check(len(tools) >= 30, f"{len(tools)} tools listed")
    info = c.call("flow5_info")
    check(info["version"].startswith("flow5"), "flow5_info")

    print("foils")
    f = c.call("foil_create_naca", {"designation": "2412"})
    check(abs(f["thickness"] - 0.12) < 0.005, "NACA 2412 thickness")
    c.call("foil_create_naca", {"designation": "0009"})
    c.call("foil_create_naca", {"designation": "2412"}, expect_error=True)
    msg = c.call("foil_get", {"name": "missing"}, expect_error=True)
    check("NACA 2412" in msg, "missing foil error lists available foils")
    c.call("foil_modify", {"name": "NACA 2412", "copy_to": "Thin 2412", "thickness": 0.09})
    thin = c.call("foil_get", {"name": "Thin 2412"})
    check(abs(thin["thickness"] - 0.09) < 0.005, "thickness scaled on a copy")
    c.call("foil_modify", {"name": "NACA 2412", "te_flap": {"x_hinge": 0.75}})

    print("xfoil")
    res = c.call("foil_analyze", {"foil": "NACA 2412", "polar": "T1_Re200k", "type": "T1", "reynolds": 200000,
                                  "range": {"start": -4, "end": 10, "step": 1}},
                 meta={"progressToken": "xf"})
    check(res["n_points"] >= 12, f"XFoil converged {res['n_points']} points")
    alphas = res["data"]["alpha"]
    cl = res["data"]["Cl"]
    i0 = alphas.index(min(alphas, key=abs))
    check(0.1 < cl[i0] < 0.4, f"Cl at alpha~0 = {cl[i0]:.3f}")
    check(any(n.get("method") == "notifications/progress" for n in c.notifications), "progress notifications")
    c.call("foil_analyze", {"foil": "NACA 0009", "type": "T1", "reynolds": 150000,
                            "range": {"start": -4, "end": 8, "step": 2}})
    opp = c.call("foil_opp_get", {"foil": "NACA 2412", "polar": "T1_Re200k", "alpha": 4})
    check(len(opp["Cp_viscous"]) > 100, "Cp distribution")
    check(c.call("foil_polar_list", {"foil": "NACA 2412"})["polars"][0]["name"] == "T1_Re200k", "foil_polar_list")

    print("resources, prompts, completions")
    resources = c.request("resources/list")["result"]["resources"]
    uris = [x["uri"] for x in resources]
    check("flow5://foils/NACA%202412" in uris, "foil resource listed")
    rd = c.request("resources/read", {"uri": "flow5://foils/NACA%202412/polars/T1_Re200k"})["result"]
    check(rd["contents"][0]["text"].startswith("alpha,Cl,Cd"), "polar CSV resource")
    check(c.request("resources/read", {"uri": "flow5://foils/none"})["error"]["code"] == -32002, "missing resource error")
    templates = c.request("resources/templates/list")["result"]["resourceTemplates"]
    check(any(t["uriTemplate"] == "flow5://foils/{foil}/polars/{polar}" for t in templates), "resource templates")
    comp = c.request("completion/complete", {"ref": {"type": "ref/resource", "uri": "flow5://foils/{foil}"},
                                             "argument": {"name": "foil", "value": "NACA"}})["result"]["completion"]
    check(set(comp["values"]) == {"NACA 2412", "NACA 0009"}, "completion of foil names")
    prompts = c.request("prompts/list")["result"]["prompts"]
    check(len(prompts) >= 3, "prompts listed")
    pg = c.request("prompts/get", {"name": "airfoil_study", "arguments": {"foil": "NACA 2412", "reynolds": "300000"}})
    check("Re=300000" in pg["result"]["messages"][0]["content"]["text"], "prompt rendered")

    print("plane")
    plane = c.call("plane_create", {
        "name": "Glider",
        "wings": [
            {"name": "Wing", "type": "main", "position": [0, 0, 0], "structural_mass": 0.4,
             "sections": [{"y": 0, "chord": 0.22, "foil": "NACA 2412", "ny": 9},
                          {"y": 0.7, "chord": 0.18, "offset": 0.02, "dihedral": 5, "foil": "NACA 2412", "ny": 7},
                          {"y": 1.2, "chord": 0.11, "offset": 0.07, "twist": -2, "foil": "NACA 2412"}]},
            {"name": "Elevator", "type": "elevator", "position": [0.85, 0, 0.05], "ry": -1.5, "nx": 7, "structural_mass": 0.05,
             "sections": [{"y": 0, "chord": 0.12, "foil": "NACA 0009", "ny": 5},
                          {"y": 0.25, "chord": 0.08, "offset": 0.03, "foil": "NACA 0009"}]},
            {"name": "Fin", "type": "fin", "position": [0.82, 0, 0.0], "nx": 7, "structural_mass": 0.03,
             "sections": [{"y": 0, "chord": 0.16, "foil": "NACA 0009", "ny": 5},
                          {"y": 0.18, "chord": 0.09, "offset": 0.06, "foil": "NACA 0009"}]}],
        "point_masses": [{"mass": 0.45, "position": [-0.12, 0, 0], "tag": "Nose"},
                         {"mass": 0.2, "position": [0.3, 0, 0], "tag": "Boom"}]})
    check(abs(plane["span"] - 2.4) < 0.01, f"span {plane['span']:.3f}")
    check(len(plane["wings"]) == 3, "three wings")
    check(plane["n_triangles"] > 0, f"{plane['n_triangles']} triangles")

    print("3d analyses")
    pp = c.call("plane_polar_create", {"plane": "Glider", "name": "T1 VLM2 inviscid", "type": "T1", "method": "VLM2",
                                       "velocity": 12, "viscous": False})
    check(pp["method"] == "VLM2", "VLM polar created")
    res = c.call("plane_analyze", {"plane": "Glider", "polar": "T1 VLM2 inviscid", "range": {"start": -2, "end": 6, "step": 2}},
                 meta={"progressToken": "vlm"})
    check(res["n_new_points"] == 5, f"VLM points: {res['n_new_points']}")
    cls = [o["CL"] for o in res["operating_points"]]
    check(all(b > a for a, b in zip(cls, cls[1:])), "CL increases with alpha")

    c.call("plane_polar_create", {"plane": "Glider", "name": "T2 tri visc", "type": "T2", "method": "TRIUNIFORM"})
    res = c.call("plane_analyze", {"plane": "Glider", "polar": "T2 tri visc", "values": [0, 2, 4]})
    check(res["n_new_points"] == 3, f"T2 panel points: {res['n_new_points']}")
    op = res["operating_points"][1]
    check(op["CDv"] > 0 and op["velocity"] > 0, f"viscous drag {op['CDv']:.4f}, balanced speed {op['velocity']:.2f} m/s")
    detail = c.call("plane_opp_get", {"plane": "Glider", "polar": "T2 tri visc", "value": 2})
    check(abs(detail["alpha"] - 2) < 1e-6, "plane_opp_get")

    c.call("plane_polar_create", {"plane": "Glider", "name": "T2 LLT", "type": "T2", "method": "LLT"})
    res = c.call("plane_analyze", {"plane": "Glider", "polar": "T2 LLT", "values": [1, 3]})
    check(res["n_new_points"] >= 1, f"LLT points: {res['n_new_points']}")

    rd = c.request("resources/read", {"uri": "flow5://planes/Glider/polars/T2%20tri%20visc"})["result"]
    check(len(rd["contents"][0]["text"].splitlines()) == 4, "plane polar CSV resource")
    check(any(n.get("method") == "notifications/resources/list_changed" for n in c.notifications), "list_changed notifications")

    print("cancellation")
    rid = c.request_async("tools/call", {"name": "foil_analyze", "arguments": {
        "foil": "NACA 0009", "type": "T1", "reynolds": 50000, "polar": "slow",
        "ranges": [{"start": 0, "end": 20, "step": 0.05}, {"start": 0, "end": -20, "step": 0.05}]}})
    time.sleep(0.5)
    t1 = time.time()
    check(c.request("ping", timeout=10)["result"] == {}, "ping answered during an analysis")
    c.notify("notifications/cancelled", {"requestId": rid, "reason": "test"})
    check(c.request("tools/list", timeout=120)["result"]["tools"], "server responsive after cancel")
    check(rid not in c.responses, "no response sent for the cancelled request")
    print(f"  cancel round trip {time.time()-t1:.1f}s")

    print("project")
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "smoke.fl5")
        c.call("project_save", {"path": path})
        check(os.path.getsize(path) > 0, "project saved")
        c.call("project_clear")
        check(c.call("project_summary")["foils"] == [], "project cleared")
        s = c.call("project_load", {"path": path})
        check(len(s["planes"]) == 1 and len(s["foils"]) == 3, "project reloaded")
        xml = os.path.join(d, "glider.xml")
        c.call("plane_export_xml", {"name": "Glider", "path": xml})
        c.call("plane_import_xml", {"path": xml, "name": "Glider copy"})
        check(abs(c.call("plane_get", {"name": "Glider copy"})["span"] - 2.4) < 0.01, "XML round trip")

    check(c.close() == 0, "clean exit on stdin close")
    print(f"all checks passed in {time.time()-t0:.1f}s")


if __name__ == "__main__":
    main()
