from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from gateway.skills import compose_system_prompt, load_skill_context


class SkillTests(unittest.TestCase):
    def test_loads_markdown_skills_in_stable_order(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "weather.md").write_text("weather rules", encoding="utf-8")
            (root / "smart-home.md").write_text("home rules", encoding="utf-8")
            context = load_skill_context(root)
        self.assertLess(context.index("smart-home"), context.index("weather"))
        self.assertIn("weather rules", context)
        self.assertIn("home rules", context)

    def test_oversized_skill_is_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "bad.md").write_text("x" * 12_001, encoding="utf-8")
            self.assertEqual(load_skill_context(root), "")

    def test_prompt_keeps_base_when_no_skill_exists(self) -> None:
        self.assertEqual(compose_system_prompt("base", ""), "base")

    def test_prompt_binds_runtime_safety_rules(self) -> None:
        prompt = compose_system_prompt("base", "## Skill: weather\nuse tool")
        self.assertIn("实时数据必须来自工具", prompt)
        self.assertIn("Skill: weather", prompt)


if __name__ == "__main__":
    unittest.main()
