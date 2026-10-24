"""Compile and run the host C++ tests, then the Python tests.

Usage: python tests/run_host_tests.py   (needs g++/c++ on PATH)
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ["tests/host/test_main.cpp", "src/dsp/LungFilter.cpp",
           "src/dsp/GapConcealer.cpp", "src/audio/Wav.cpp"]


def run_cpp():
    compiler = shutil.which("g++") or shutil.which("c++")
    if not compiler:
        raise SystemExit("g++ not found on PATH")
    with tempfile.TemporaryDirectory(prefix="lungsound-host-") as tmp:
        binary = Path(tmp) / ("host-tests" + (".exe" if os.name == "nt" else ""))
        subprocess.run([compiler, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "src"), *[str(ROOT / s) for s in SOURCES],
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


def run_python():
    suite = unittest.defaultTestLoader.discover(str(ROOT / "tests"), pattern="test_*.py")
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    if not result.wasSuccessful():
        sys.exit(1)


if __name__ == "__main__":
    run_cpp()
    run_python()
