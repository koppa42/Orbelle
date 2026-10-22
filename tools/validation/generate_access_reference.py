"""Reproduce the fixed Orekit/Hipparchus pass reference without using Orbelle."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--jar-dir", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
source = Path(__file__).resolve().parent / "orekit"
deps = json.loads((source / "dependencies.json").read_text())
jars = []
for dep in deps:
    path = args.jar_dir / dep["filename"]
    if hashlib.sha256(path.read_bytes()).hexdigest() != dep["sha256"]:
        raise ValueError(f"Reference hash mismatch: {path}")
    jars.append(str(path.resolve()))
args.output.mkdir(parents=True, exist_ok=False)
cp = os.pathsep.join(jars)
java = source / "OrbelleAccessReference.java"
subprocess.run(["javac", "-cp", cp, "-d", str(args.output), str(java)], check=True)
run = subprocess.run(["java", "-cp", str(args.output.resolve()) + os.pathsep + cp,
                      "OrbelleAccessReference"], text=True, capture_output=True, check=True)
output = args.output / "access_orekit.txt"
output.write_text(run.stdout, encoding="utf-8", newline="\n")
metadata = {"reference": "Orekit 13.1.2 / Hipparchus 4.0.2", "dependencies": deps,
            "source_sha256": hashlib.sha256(java.read_bytes()).hexdigest(),
            "reference_sha256": hashlib.sha256(output.read_bytes()).hexdigest()}
(args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
print(json.dumps({"output": str(output), "sha256": metadata["reference_sha256"]}))
