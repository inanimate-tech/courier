#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "click>=8.1",
# ]
# ///

import os
import shutil
import subprocess
import sys
from pathlib import Path

import click

# Project root (parent of tools/)
ROOT = Path(__file__).parent.parent

# Ensure ~/.platformio/penv/bin is on PATH (pio is often installed there)
_pio_bin = Path.home() / ".platformio" / "penv" / "bin"
if _pio_bin.is_dir():
    os.environ["PATH"] = str(_pio_bin) + os.pathsep + os.environ.get("PATH", "")

CPPCHECK_SUPPRESSIONS = [
    "missingIncludeSystem",
    "unmatchedSuppression",
    # Transport classes own connections and are not copyable by design
    "noCopyConstructor",
    "noOperatorEq",
    # cppcheck can't evaluate ESP-IDF version macros
    "syntaxError:*MqttTransport.cpp",
]


def run_cmd(cmd: list[str], cwd: Path | None = None, label: str = "") -> bool:
    """Run a command, return True if successful."""
    if label:
        click.echo(click.style(f"  → {label}", fg="cyan"))
    result = subprocess.run(cmd, cwd=cwd)
    return result.returncode == 0


@click.group(context_settings={"help_option_names": ["-h", "--help"]})
def cli() -> None:
    """Run tests for the Courier library."""
    pass


@cli.command("static-analysis")
def static_analysis() -> None:
    """Run cppcheck static analysis on src/."""
    click.echo(click.style("Static Analysis", fg="white", bold=True))

    cmd = ["cppcheck", "--enable=warning", "--error-exitcode=1"]
    for suppression in CPPCHECK_SUPPRESSIONS:
        cmd.append(f"--suppress={suppression}")
    cmd.append(str(ROOT / "src"))

    if run_cmd(cmd, cwd=ROOT, label="cppcheck src/"):
        click.echo(click.style("✓ Static analysis passed", fg="green"))
    else:
        click.echo(click.style("✗ Static analysis failed", fg="red"))
        sys.exit(1)


@cli.command("unit")
def unit_tests() -> None:
    """Run unit tests on native platform."""
    click.echo(click.style("Unit Tests", fg="white", bold=True))

    test_dir = ROOT / "test" / "unit"
    if not run_cmd(["pio", "test", "-e", "native"], cwd=test_dir, label="pio test -e native"):
        click.echo(click.style("✗ Unit tests failed", fg="red"))
        sys.exit(1)

    click.echo(click.style("✓ Unit tests passed", fg="green"))


@cli.command("build")
def build_verification() -> None:
    """Build-verify PlatformIO examples."""
    click.echo(click.style("Build Verification", fg="white", bold=True))
    success = True

    # PlatformIO builds
    pio_dir = ROOT / "test" / "build-platformio"
    for project in sorted(pio_dir.iterdir()):
        if not project.is_dir():
            continue
        ini = project / "platformio.ini"
        if not ini.exists():
            continue
        if not run_cmd(["pio", "run"], cwd=project, label=f"pio run ({project.name})"):
            success = False

    if success:
        click.echo(click.style("✓ All builds passed", fg="green"))
    else:
        click.echo(click.style("✗ Some builds failed", fg="red"))
        sys.exit(1)


@cli.command("all")
@click.pass_context
def run_all(ctx: click.Context) -> None:
    """Run static analysis, unit tests, and build verification."""
    ctx.invoke(static_analysis)
    click.echo()
    ctx.invoke(unit_tests)
    click.echo()
    ctx.invoke(build_verification)


if __name__ == "__main__":
    cli()
