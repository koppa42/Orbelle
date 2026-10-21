"""Validate desktop copy and references without importing Qt or building the app."""
from __future__ import annotations

import argparse
import ast
import json
from pathlib import Path
import re

TOKENS = re.compile(
    r'//[^\n]*|/\*[\s\S]*?\*/'
    r'|(?:u8|u|U|L)?R"(?P<tag>[^ ()\\\t\r\n]{0,16})\([\s\S]*?\)(?P=tag)"'
    r'|(?:u8|u|U|L)?"(?:\\[\s\S]|[^"\\])*"'
    r"|(?:u8|u|U|L)?'(?:\\(?:x[0-9a-fA-F]+|u[0-9a-fA-F]{4}|U[0-9a-fA-F]{8}|[^\r\n])|[^'\\\r\n])'"
)
HAN = re.compile(r"[\u3400-\u9fff]")
KEY = re.compile(r"[a-z][a-zA-Z0-9_.]*\Z")
UI_SINK = re.compile(
    r"(?:new Q(?:Label|PushButton|ToolButton|CheckBox|RadioButton|GroupBox|Menu|Action)"
    r"|(?:setWindowTitle|setText|setToolTip|setPlaceholderText|setAccessibleName|addAction))"
    r"\(\s*$"
)
# Scientific identifiers are data rather than translatable prose.
IDENTIFIERS = {"GCRF", "ITRF", "TEME", "UTC", "TAI", "TT", "GM", "Cd", "Cr", "ID",
               "x", "y", "z", "vx", "vy", "vz"}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate catalogue key: {key}")
        result[key] = value
    return result


def read_catalogue(path):
    root = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object)
    if not isinstance(root, dict) or root.get("locale") != "zh-CN":
        raise ValueError("Expected a zh-CN text catalogue")
    messages = root.get("messages")
    if not isinstance(messages, dict) or not messages:
        raise ValueError("Expected a nonempty messages object")
    for key, value in messages.items():
        if not KEY.fullmatch(key) or not isinstance(value, str) or not value:
            raise ValueError(f"Invalid catalogue entry: {key}")
    return messages


def string_literals(source):
    """Skip comments, raw-string bodies and character constants as single tokens."""
    for match in TOKENS.finditer(source):
        raw = match.group()
        if raw.startswith(("//", "/*")):
            continue
        if 'R"' in raw[:4]:
            body_start = raw.index("(") + 1
            yield match.start(), raw[body_start:-(len(match.group("tag")) + 2)]
        elif re.match(r'(?:u8|u|U|L)?"', raw):
            # C++ digit separators and apostrophes do not consume adjacent strings.
            yield match.start(), ast.literal_eval(raw[raw.index('"'):])


def smoke_fixture_range(path, source):
    # The embedded workbench smoke driver deliberately enters Unicode user names.
    # These inputs/expected values must remain independent of translated UI labels.
    if path.name != "graphics_workbench.cpp":
        return -1, -1
    start = source.find("    void smoke_insert_dialog(")
    end = source.find("    void benchmark_input(")
    return (start, end) if start >= 0 and end > start else (-1, -1)


def validate_source(path, source, messages):
    errors, referenced = [], set()
    fixture_start, fixture_end = smoke_fixture_range(path, source)
    code = TOKENS.sub(lambda m: " " * len(m.group()) if m.group().startswith(("//", "/*"))
                     else m.group(), source)
    for offset, value in string_literals(source):
        if value in messages:
            referenced.add(value)
        if fixture_start <= offset < fixture_end:
            continue
        prefix = code[max(0, offset - 180):offset]
        if HAN.search(value) or (UI_SINK.search(prefix) and re.search("[A-Za-z]", value)
                                 and value not in IDENTIFIERS):
            line = source.count("\n", 0, offset) + 1
            errors.append(f"{path}:{line}: display copy belongs in resources/text/zh_CN.json")
        # Metadata tables mark deferred lookups with ui::key, keeping them constexpr.
        if re.search(r"ui::(?:text|utf8|key)\(\s*$", prefix) and value not in messages:
            errors.append(f"{path}: unknown text key {value}")
    return errors, referenced


def check(root):
    messages = read_catalogue(root / "apps/desktop/resources/text/zh_CN.json")
    errors, referenced = [], set()
    for path in sorted((root / "apps/desktop/src").rglob("*")):
        if path.suffix in (".cpp", ".hpp"):
            found, used = validate_source(path.relative_to(root),
                                          path.read_text(encoding="utf-8"), messages)
            errors.extend(found)
            referenced.update(used)
    errors.extend(f"Unused text key: {key}" for key in sorted(messages.keys() - referenced))
    return errors, len(messages)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    try:
        errors, count = check(args.root)
    except (OSError, ValueError, SyntaxError) as error:
        print(error)
        return 1
    if errors:
        print("\n".join(errors))
        return 1
    print(f"Desktop text: {count} messages; all references resolved; no embedded Chinese UI copy.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
