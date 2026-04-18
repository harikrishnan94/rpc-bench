from __future__ import annotations

import json
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path


def run(cmd: list[str], *, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    print("+", shlex.join(cmd))
    return subprocess.run(cmd, check=True, text=True, env=env)


def capture(cmd: list[str]) -> str:
    print("+", shlex.join(cmd))
    return subprocess.check_output(cmd, text=True).strip()


def write_native_file(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "\n".join(
            [
                "[built-in options]",
                "buildtype = 'debug'",
                "cpp_args = ['-fprofile-instr-generate', '-fcoverage-mapping']",
                "cpp_link_args = ['-fprofile-instr-generate']",
                "",
            ]
        ),
        encoding="utf-8",
    )


def locate_target_output(build_dir: Path, name: str) -> Path:
    targets = json.loads(capture(["meson", "introspect", str(build_dir), "--targets"]))

    for target in targets:
        if target.get("name") != name:
            continue

        filenames = target.get("filename", [])
        if isinstance(filenames, str):
            return Path(filenames)
        if filenames:
            return Path(filenames[0])

    raise RuntimeError(f"Could not locate output for Meson target '{name}'")


def locate_test_binaries(build_dir: Path) -> list[Path]:
    tests = json.loads(capture(["meson", "introspect", str(build_dir), "--tests"]))
    binaries: list[Path] = []

    for test in tests:
        command = test.get("cmd", [])
        if command:
            binaries.append(Path(command[0]))

    if not binaries:
        raise RuntimeError("Could not locate any Meson test binaries")

    return binaries


def remove_stale_profiles(profiles_dir: Path) -> None:
    if not profiles_dir.exists():
        return

    for profile in profiles_dir.glob("*.profraw"):
        profile.unlink()


def main() -> int:
    source_root = Path(
        os.environ.get("MESON_SOURCE_ROOT", Path(__file__).resolve().parents[1])
    ).resolve()
    current_build_root = Path(
        os.environ.get("MESON_BUILD_ROOT", source_root / "builddir")
    ).resolve()
    coverage_build_dir = (source_root / "builddir-coverage").resolve()

    if current_build_root == coverage_build_dir:
        raise RuntimeError(
            "Run the 'coverage' target from a non-coverage build directory so it can "
            "manage builddir-coverage safely."
        )

    if shutil.which("meson") is None:
        raise RuntimeError("Could not find 'meson' on PATH")
    if shutil.which("xcrun") is None:
        raise RuntimeError("Could not find 'xcrun' on PATH")

    llvm_cov = capture(["xcrun", "--find", "llvm-cov"])
    llvm_profdata = capture(["xcrun", "--find", "llvm-profdata"])

    native_file = source_root / ".cache" / "meson-coverage.ini"
    base_native_file = source_root / "native" / "clang22-debug.ini"
    profiles_dir = coverage_build_dir / "coverage-profiles"
    summary_path = coverage_build_dir / "meson-logs" / "coverage-summary.txt"
    html_dir = coverage_build_dir / "meson-logs" / "coverage-html"
    profdata_path = coverage_build_dir / "coverage.profdata"

    write_native_file(native_file)

    setup_cmd = [
        "meson",
        "setup",
        str(coverage_build_dir),
        str(source_root),
    ]
    if base_native_file.exists():
        setup_cmd.extend(["--native-file", str(base_native_file)])
    setup_cmd.extend(["--native-file", str(native_file)])
    if coverage_build_dir.exists() and any(coverage_build_dir.iterdir()):
        setup_cmd.insert(2, "--wipe")

    run(setup_cmd)
    run(["meson", "compile", "-C", str(coverage_build_dir)])

    profiles_dir.mkdir(parents=True, exist_ok=True)
    summary_path.parent.mkdir(parents=True, exist_ok=True)
    remove_stale_profiles(profiles_dir)

    test_env = os.environ.copy()
    test_env["LLVM_PROFILE_FILE"] = str(profiles_dir / "rpc-bench-%p.profraw")
    run(
        [
            "meson",
            "test",
            "-C",
            str(coverage_build_dir),
            "--print-errorlogs",
        ],
        env=test_env,
    )
    run(
        [
            "meson",
            "test",
            "-C",
            str(coverage_build_dir),
            "--benchmark",
            "--print-errorlogs",
        ],
        env=test_env,
    )

    raw_profiles = sorted(profiles_dir.glob("*.profraw"))
    if not raw_profiles:
        raise RuntimeError("No raw LLVM profile data was produced")

    run(
        [
            llvm_profdata,
            "merge",
            "-sparse",
            *[str(profile) for profile in raw_profiles],
            "-o",
            str(profdata_path),
        ]
    )

    test_binaries = locate_test_binaries(coverage_build_dir)
    primary_binary = test_binaries[0]
    library_binary = locate_target_output(coverage_build_dir, "rpc_bench_core")
    extra_objects = [library_binary, *test_binaries[1:]]
    ignore_regex = ".*/(subprojects|tests)/.*"

    report_cmd = [
        llvm_cov,
        "report",
        str(primary_binary),
        f"-instr-profile={profdata_path}",
        f"-ignore-filename-regex={ignore_regex}",
        *[f"-object={path}" for path in extra_objects],
    ]
    summary = capture(report_cmd)
    summary_path.write_text(summary + "\n", encoding="utf-8")
    print(summary)

    if html_dir.exists():
        shutil.rmtree(html_dir)

    run(
        [
            llvm_cov,
            "show",
            str(primary_binary),
            f"-instr-profile={profdata_path}",
            f"-ignore-filename-regex={ignore_regex}",
            *[f"-object={path}" for path in extra_objects],
            f"-output-dir={html_dir}",
            "-format=html",
        ]
    )

    print(f"Coverage summary: {summary_path}")
    print(f"Coverage HTML: {html_dir / 'index.html'}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode) from error
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
