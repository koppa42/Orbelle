import json
from pathlib import Path
import tempfile
import unittest

from check_desktop_text import read_catalogue, string_literals, validate_source


class DesktopTextValidatorTest(unittest.TestCase):
    def test_comments_character_constants_and_raw_strings(self):
        source = '''// 说明
/* 注释 "不会匹配" */
auto n = 1'000'000;
auto c = '\\n';
auto text = R"(显示文案)";
auto escaped = "\\u5173\\u95ed";
'''
        values = [value for _, value in string_literals(source)]
        self.assertEqual(values, ["显示文案", "关闭"])

    def test_literal_and_unknown_key_are_rejected(self):
        errors, used = validate_source(Path("window.cpp"),
            'auto a = ui::text("window.close"); auto b = ui::text("missing");'
            'auto c = QStringLiteral("直接写死");', {"window.close": "关闭"})
        self.assertEqual(used, {"window.close"})
        self.assertEqual(len(errors), 2)

    def test_deferred_metadata_keys_are_checked(self):
        errors, _ = validate_source(Path("units.cpp"),
            'constexpr auto label = ui::key("missing");', {"known": "已知"})
        self.assertEqual(len(errors), 1)

    def test_english_labels_are_checked_but_comments_and_frame_ids_are_not(self):
        errors, _ = validate_source(Path("window.cpp"),
            '// ui::text("not.code")\nnew QLabel("Raw label"); new QLabel("GCRF");',
            {"known": "已知"})
        self.assertEqual(len(errors), 1)

    def test_duplicate_and_empty_messages_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "text.json"
            path.write_text('{"locale":"zh-CN","messages":{"a":"one","a":"two"}}',
                            encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Duplicate"):
                read_catalogue(path)
            path.write_text(json.dumps({"locale": "zh-CN", "messages": {"a": ""}}),
                            encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Invalid"):
                read_catalogue(path)


if __name__ == "__main__":
    unittest.main()
