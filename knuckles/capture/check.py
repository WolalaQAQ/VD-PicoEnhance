"""Build and run only owned Windows mock inputs; no OpenVR runtime is loaded."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


HERE = Path(__file__).resolve().parent
PROJECT = HERE.parents[1]
WORKSPACE = PROJECT.parent
BUILD = HERE / "build"
COMPILER = Path(os.environ['ZIG'])
SDK = Path(os.environ['OPENVR_ROOT']) / 'headers'
SOURCE = PROJECT / "pc/driver/src"
SCENARIOS = ("shared", "two-tables", "only003", "only004", "missing",
             "historical", "failed-and-path", "stop", "drain", "stress",
             "external-before", "external-after", "clone-table", "pin",
             "stop-before-start", "only005", "three-tables", "shared005004",
             "shared005003", "shared005004-then003", "shared005003-with004",
             "shared004003-with005")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", action="append", choices=SCENARIOS,
                        help="Run one scenario; repeat to select several. Default: all.")
    parser.add_argument("--output-dir", type=Path, default=BUILD)
    args = parser.parse_args()
    build = args.output_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    for key, leaf in (("TEMP", "tmp"), ("TMP", "tmp"),
                      ("ZIG_LOCAL_CACHE_DIR", "zig-local"),
                      ("ZIG_GLOBAL_CACHE_DIR", "zig-global")):
        directory = build / leaf
        directory.mkdir(exist_ok=True)
        env[key] = str(directory)

    commands = []

    def run(name: str, arguments: list[str | Path], *,
            allow_failure: bool = False) -> subprocess.CompletedProcess[str]:
        command = [str(item) for item in arguments]
        result = subprocess.run(command, cwd=HERE, env=env, capture_output=True,
                                text=True, encoding="utf-8", errors="strict", timeout=240)
        log = build / f"{name}.log"
        suffix = 1
        while log.exists():
            suffix += 1
            log = build / f"{name}-{suffix}.log"
        stdout_log = log.with_suffix(".stdout.log")
        stderr_log = log.with_suffix(".stderr.log")
        stdout_log.write_text(result.stdout, encoding="utf-8")
        stderr_log.write_text(result.stderr, encoding="utf-8")
        log.write_text(
            subprocess.list2cmdline(command) + "\n" + result.stdout + result.stderr,
            encoding="utf-8")
        commands.append({"label": name, "command": command, "returncode": result.returncode,
                         "stdout": str(stdout_log), "stderr": str(stderr_log)})
        (build / "commands.json").write_text(json.dumps(commands, indent=2) + "\n", encoding="utf-8")
        if result.returncode and not allow_failure:
            raise RuntimeError(f"{name} failed ({result.returncode}):\n{result.stdout}{result.stderr}")
        return result

    common = [COMPILER, 'c++', "-target", "x86_64-windows-gnu", "-O2", "-g0",
              "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-I", SOURCE, "-isystem", SDK,
              "-I", HERE]
    run("build-probe", common + ["-x", "c", "-std=c11", "-c", SOURCE / "input_probe.c",
                                 "-o", build / "input_probe.o"])
    cpp = common + ["-std=c++17", "-fno-exceptions", "-fno-rtti"]
    library = build / "input_capture_mock.dll"
    run("build-capture-dll", cpp + ["-shared", HERE / "input_capture.cpp",
                                    HERE / "checks/bridge.cpp", build / "input_probe.o",
                                    "-o", library])
    executable = build / "capture_mock.exe"
    run("build-mock", cpp + [HERE / "checks/mock.cpp", "-o", executable])
    results = []
    for scenario in args.scenario or SCENARIOS:
        result = run(f"test-{scenario}", [executable, library, scenario], allow_failure=True)
        if result.returncode:
            record = {"scenario": scenario, "ok": False, "returncode": result.returncode,
                      "stdout": result.stdout, "stderr": result.stderr}
            print(json.dumps(record), flush=True)
        else:
            record = json.loads(result.stdout)
            record["returncode"] = result.returncode
            print(result.stdout.strip(), flush=True)
        results.append(record)
    summary = {"ok": all(item["ok"] for item in results), "scenarios": len(results),
               "checks": sum(item.get("checks", 0) for item in results),
               "checks_scope": "successful scenarios only", "results": results,
               "failed_scenarios": [item["scenario"] for item in results if not item["ok"]],
               "compiler": str(COMPILER), "target": "x86_64-windows-gnu",
               "sdk_header": str(SDK / "openvr_driver.h"),
               "official_binary_loaded": False, "device_operations": False}
    (build / "results.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: summary[key] for key in ("ok", "scenarios", "checks")}))
    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
