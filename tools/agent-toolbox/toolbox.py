#!/usr/bin/env python3
"""Select documented tools and inspect PATH without executing external programs."""

from __future__ import annotations

import argparse
import json
import os
import shutil
from pathlib import Path


CATALOG_PATH = Path(__file__).resolve().with_name("catalog.json")


def load_catalog():
    catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
    tools = {tool["id"]: tool for tool in catalog["tools"]}
    if len(tools) != len(catalog["tools"]):
        raise ValueError("Duplicate tool IDs in catalog")
    for task, route in catalog["routes"].items():
        for tool_id in route["tools"]:
            if tool_id not in tools:
                raise ValueError(f"Unknown tool {tool_id!r} in route {task!r}")
    return catalog, tools


def discover(tool):
    found = []
    for name in tool.get("executables", []):
        path = shutil.which(name)
        if path and path not in found:
            found.append(path)
    if tool["id"] == "ghidra":
        install_dir = os.environ.get("GHIDRA_INSTALL_DIR")
        if install_dir:
            for name in ("analyzeHeadless", "analyzeHeadless.bat"):
                candidate = Path(install_dir) / "support" / name
                if candidate.is_file():
                    absolute = str(candidate.resolve())
                    if absolute not in found:
                        found.append(absolute)
    return {
        "id": tool["id"],
        "repository": tool["repository"],
        "status": (
            "path_found" if found else
            "not_found" if tool.get("executables") else
            "manual_setup"
        ),
        "paths": found,
        "setup_docs": tool["setup_docs"],
        "catalog_status": tool["status"],
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    listing = commands.add_parser("list", help="List tools or available task routes")
    listing.add_argument("--tasks", action="store_true")
    listing.add_argument("--json", action="store_true")
    planning = commands.add_parser("plan", help="Select tools for a task")
    planning.add_argument("task")
    planning.add_argument("--json", action="store_true")
    doctor = commands.add_parser("doctor", help="Inspect PATH; never run/install tools")
    doctor.add_argument("--task")
    doctor.add_argument("--strict", action="store_true")
    doctor.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    try:
        catalog, tools = load_catalog()
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(2, f"Cannot load catalog: {error}\n")

    task = getattr(args, "task", None)
    if task and task not in catalog["routes"]:
        parser.error("Unknown task; use 'list --tasks' to see available routes")

    selected = (
        [tools[tool_id] for tool_id in catalog["routes"][task]["tools"]]
        if task else list(tools.values())
    )
    if args.command == "doctor":
        results = [discover(tool) for tool in selected]
        if args.json:
            print(json.dumps({"task": task, "tools": results}, indent=2, ensure_ascii=False))
        else:
            print("PATH inspection only: MCP connections and tool execution are unverified.")
            for result in results:
                locations = ", ".join(result["paths"]) or result["setup_docs"]
                print(f"{result['id']}: {result['status']} - {locations}")
        return int(args.strict and any(r["status"] != "path_found" for r in results))

    if args.command == "list" and args.tasks:
        output = catalog["routes"]
        lines = [f"{name}: {route['description']}" for name, route in output.items()]
    else:
        output = {"reviewed_at": catalog["reviewed_at"], "tools": selected}
        if task:
            output["task"] = task
            output["description"] = catalog["routes"][task]["description"]
        lines = [
            f"{tool['id']} [{tool['status']}]: {tool['repository']}\n"
            f"  {tool['description']}\n  Docs: {tool['setup_docs']}"
            for tool in selected
        ]
    print(json.dumps(output, indent=2, ensure_ascii=False) if args.json else "\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
