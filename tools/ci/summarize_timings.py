"""Render command timings, including failed commands and nested Python checks."""
import argparse
import html
import json
import os
from pathlib import Path


def cell(value):
    return html.escape(str(value)).replace("|", "&#124;").replace("\n", " ").replace("\r", " ")


def summarize(reports):
    lines = ["## Command timings", "",
             "Nested reports expand their parent command; durations must not be added across tables.", ""]
    files = sorted(reports.rglob("commands.json"))
    if not files:
        lines += ["No command timing reports were produced (the job may have stopped during setup).", ""]
    for path in files:
        lines += [f"### {cell(path.relative_to(reports).parent.as_posix())}", "",
                  "| Command | Duration (s) | Exit code |",
                  "| --- | ---: | ---: |"]
        for record in json.loads(path.read_text(encoding="utf-8")):
            elapsed = record.get("elapsed_seconds")
            duration = f"{elapsed:.2f}" if elapsed is not None else "unavailable"
            lines.append(f"| {cell(record['name'])} | {duration} | {cell(record['exit_code'])} |")
        lines.append("")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reports", type=Path, required=True)
    args = parser.parse_args()
    summary = summarize(args.reports)
    print(summary)
    if path := os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(path).open("a", encoding="utf-8") as stream:
            stream.write(summary)


if __name__ == "__main__":
    main()
