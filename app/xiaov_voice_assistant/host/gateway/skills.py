"""Load bounded Markdown Skills for the Xiao V cloud-agent gateway."""

from __future__ import annotations

from pathlib import Path

DEFAULT_SKILLS_DIR = Path(__file__).resolve().parents[1] / "skills"
MAX_SKILL_FILES = 16
MAX_SKILL_BYTES = 12_000


def load_skill_context(directory: Path | None = None) -> str:
    """Return a safe, deterministic prompt suffix from Markdown Skill files."""
    root = (directory or DEFAULT_SKILLS_DIR).expanduser()
    try:
        files = sorted(root.glob("*.md"))[:MAX_SKILL_FILES]
    except OSError:
        return ""
    sections: list[str] = []
    for path in files:
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError):
            continue
        if not text.strip() or len(text.encode("utf-8")) > MAX_SKILL_BYTES:
            continue
        sections.append(f"\n## Skill: {path.stem}\n{text.strip()}")
    return "\n".join(sections)


def compose_system_prompt(base_prompt: str, skill_context: str) -> str:
    """Attach Skills without changing the normal assistant personality."""
    if not skill_context.strip():
        return base_prompt
    return (
        f"{base_prompt.rstrip()}\n\n"
        "以下是本作品启用的场景 Skill。仅在用户意图匹配时遵守；"
        "实时数据必须来自工具，设备动作必须等待真实执行结果。\n"
        f"{skill_context.strip()}"
    )


__all__ = ["DEFAULT_SKILLS_DIR", "compose_system_prompt", "load_skill_context"]
