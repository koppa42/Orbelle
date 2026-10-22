"""Generate pinned Orekit celestial and joint-covariance references without Orbelle."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jar-dir", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--frames-output", type=Path, required=True)
    parser.add_argument("--covariance-output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = root / "tools/validation/orekit"
    jars = []
    for dependency in json.loads((source / "dependencies.json").read_text()):
        path = args.jar_dir / dependency["filename"]
        if hashlib.sha256(path.read_bytes()).hexdigest() != dependency["sha256"]:
            raise ValueError(f"Reference JAR hash mismatch: {path}")
        jars.append(str(path.resolve()))
    args.work_dir.mkdir(parents=True, exist_ok=True)
    cp = os.pathsep.join(jars)
    subprocess.run(["javac", "-cp", cp, "-d", str(args.work_dir),
                    str(source / "OrbelleCelestialFramesReference.java")], check=True)
    subprocess.run(["java", "-cp", str(args.work_dir.resolve()) + os.pathsep + cp,
                    "OrbelleCelestialFramesReference", str(args.frames_output)], check=True)
    subprocess.run([sys.executable, str(root / "tools/validation/hpop_celestial_covariance_reference.py"),
                    "--frames", str(args.frames_output), "--output", str(args.covariance_output)],
                   check=True)
    print("Frame SHA256", hashlib.sha256(args.frames_output.read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
