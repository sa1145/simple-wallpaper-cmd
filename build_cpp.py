#!/usr/bin/env python3
"""Configure and build a CMake target, returning one machine-readable result."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent


def find_cmake(build_dir: Path) -> Path | None:
    """Prefer the CMake used by an existing cache, then PATH and Visual Studio."""
    cache = build_dir / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CMAKE_COMMAND:INTERNAL="):
                candidate = Path(line.split("=", 1)[1])
                if candidate.is_file():
                    return candidate

    on_path = shutil.which("cmake")
    if on_path:
        return Path(on_path)

    program_files = Path(os.environ.get("ProgramFiles", r"C:\Program Files"))
    pattern = "Microsoft Visual Studio/*/*/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
    return next((path for path in program_files.glob(pattern) if path.is_file()), None)


def run(command: list[str], cwd: Path) -> dict[str, object]:
    completed = subprocess.run(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    return {
        "command": command,
        "returncode": completed.returncode,
        "output": completed.stdout,
    }


def find_executable(build_dir: Path, target: str, config: str) -> Path | None:
    matches = list(build_dir.rglob(f"{target}.exe"))
    if not matches:
        return None
    config_lower = config.lower()
    return max(
        matches,
        key=lambda path: (config_lower in {part.lower() for part in path.parts}, path.stat().st_mtime_ns),
    )


def emit(result: dict[str, object], json_file: Path | None) -> int:
    payload = json.dumps(result, ensure_ascii=False, indent=2)
    if json_file:
        json_file.parent.mkdir(parents=True, exist_ok=True)
        json_file.write_text(payload + "\n", encoding="utf-8")
    print(payload)
    return 0 if result["status"] == "success" else 1


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build a CMake C++ target and report the exe path or failure as JSON."
    )
    parser.add_argument("--source-dir", type=Path, default=ROOT)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--target", default="DX12WallpaperEngine")
    parser.add_argument("--config", default="Release")
    parser.add_argument("--clean-first", action="store_true")
    parser.add_argument("--parallel", type=int, metavar="N")
    parser.add_argument("--json-file", type=Path, help="Also save the JSON result to this path.")
    args = parser.parse_args()

    source_dir = args.source_dir.resolve()
    build_dir = args.build_dir.resolve()
    result: dict[str, object] = {
        "status": "error",
        "phase": "setup",
        "target": args.target,
        "config": args.config,
        "executable": None,
        "steps": [],
    }

    if not (source_dir / "CMakeLists.txt").is_file():
        result["message"] = f"CMakeLists.txt not found in {source_dir}"
        return emit(result, args.json_file)

    cmake = find_cmake(build_dir)
    if cmake is None:
        result["message"] = "cmake.exe was not found in the build cache, PATH, or Visual Studio."
        return emit(result, args.json_file)

    build_dir.mkdir(parents=True, exist_ok=True)
    configure = run([str(cmake), "-S", str(source_dir), "-B", str(build_dir)], source_dir)
    result["steps"].append({"phase": "configure", **configure})
    if configure["returncode"] != 0:
        result.update(phase="configure", message="CMake configuration failed.")
        return emit(result, args.json_file)

    command = [str(cmake), "--build", str(build_dir), "--config", args.config, "--target", args.target]
    if args.clean_first:
        command.append("--clean-first")
    if args.parallel:
        command.extend(["--parallel", str(args.parallel)])
    build = run(command, source_dir)
    result["steps"].append({"phase": "build", **build})
    if build["returncode"] != 0:
        result.update(phase="build", message="C++ compilation or linking failed.")
        return emit(result, args.json_file)

    executable = find_executable(build_dir, args.target, args.config)
    if executable is None:
        result.update(phase="locate", message=f"Build succeeded but {args.target}.exe was not found.")
        return emit(result, args.json_file)

    result.update(
        status="success",
        phase="complete",
        message="Build completed successfully.",
        executable=str(executable.resolve()),
    )
    return emit(result, args.json_file)


if __name__ == "__main__":
    sys.exit(main())
