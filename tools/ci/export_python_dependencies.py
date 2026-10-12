"""Transfer compiled headless dependencies between test jobs, without SDK packaging."""
import argparse
from pathlib import Path
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "packaging"))
from package_sdk import copy_dependencies
sys.path.pop(0)


def export(prefix, archive):
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="python-dependencies-", dir=archive.parent) as temporary:
        dependencies = Path(temporary) / "dependencies"
        copy_dependencies(prefix, dependencies)
        with tarfile.open(archive, "x:gz") as bundle:
            bundle.add(dependencies, arcname="dependencies")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dependency-prefix", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    export(args.dependency_prefix.resolve(strict=True), args.output.resolve())


if __name__ == "__main__":
    main()
