# logs/ — AI Coding 日志目录

存放你在开发中与 AI 工具的对话日志，和作品代码一并提交。

本提交已删除模板示例，真实日志位于 `logs/1033087886/`。

本次会话发生在 Codex Desktop，且原开发目录不位于带 `.repo/` 的 openvela 工作区，
官方 CLI hook 因隐私门控没有自动采集。为避免伪造完整采集状态，仅导出当前会话中
从“阅读专属仓 README 并提交”开始的真实消息和工具操作，并在 manifest 标记为
`vscode_extension_partial`/`degraded`。历史会话包含私有部署配置，未导出。

## 目录结构

```text
logs/
└── <github_login>/              # 你的 GitHub 用户名，一人一目录
    ├── manifest.json            # 会话清单
    └── <date>/                  # 日期 YYYY-MM-DD
        └── <tool>__<sid>.jsonl  # 一个会话一个文件（工具名与 session id 用 __ 连接）
```

- `<tool>`：`claude-code` / `opencode` / `codex` / `kiro`
- 每个 `.jsonl` 每行一个事件，由组委会提供的日志归集工具导出，**只提交 JSONL 本身**。

导出与提交的完整步骤、字段定义见[《AI Coding 日志归集与提交手册》](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_coding_log_guide.md)。
