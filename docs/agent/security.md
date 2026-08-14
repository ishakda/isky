# Security Model

## Security levels

Every `AgentTool` carries an intrinsic `SecurityLevel` (`agent/tools/tool.h`),
fixed at registration time — not model- or config-controlled:

| Level | Meaning | Example tools |
|---|---|---|
| `SEC_SAFE` | Read-only, no side effects | `read_file`, `list_directory`, `search_files`, `git_status`, `git_diff`, `git_log`, `git_branch` |
| `SEC_LOW_RISK` | Creates/edits inside the sandbox | `write_file`, `edit_file`, `git_commit` |
| `SEC_MEDIUM_RISK` | Runs commands / makes network calls | `shell_execute`, `web_fetch`, `web_search` |
| `SEC_HIGH_RISK` | Deletes, destructive filesystem ops | `delete_file` |
| `SEC_CRITICAL` | System configuration | reserved; no built-in tool currently registers at this level |

## Autonomy levels (0-4)

Set via `config/agent.json`'s `agent.autonomy_level` (default `3`) or `--autonomy`
on the CLI, clamped to `[0,4]` by `agent_config_load`. `permissions_autonomy_allows()`
(`agent/security/permissions.c`) determines what runs **without** confirmation at
each level:

| Level | Name (per code comment) | Auto-allows |
|---|---|---|
| 0 | chat | Nothing auto-runs (`return 0` unconditionally) |
| 1 | suggest | Nothing auto-runs (model may propose, human must approve everything) |
| 2 | execute safe tools | `SEC_SAFE` only |
| 3 (default) | autonomous multi-step | `SEC_SAFE` through `SEC_MEDIUM_RISK` — filesystem writes and shell commands auto-run; destructive/high-risk still gated |
| 4 | autonomous coding/research | `SEC_SAFE` through `SEC_HIGH_RISK` — still never auto-runs `SEC_CRITICAL` |

No autonomy level ever auto-allows `SEC_CRITICAL`.

## Approval modes

`ApprovalMode` (`agent/core/config.h`): `APPROVAL_ALWAYS`, `APPROVAL_RISKY`
(default), `APPROVAL_NEVER`. `permissions_needs_confirmation()` combines mode,
autonomy, per-tool override, and security level:

- **`always`** — every tool call needs confirmation, full stop.
- **`never`** — no prompt is shown for anything below `SEC_CRITICAL`.
  `permissions_needs_confirmation()` enforces a hard backstop: a `SEC_CRITICAL`
  action still returns `1` (requires confirmation) even under `never`, so a
  system-configuration action can never auto-run unattended. Below CRITICAL,
  `never` fully trusts the configured tool set and autonomy level.
- **`risky`** (default) — confirmation is required when: a per-tool override sets
  `require_confirmation: true`; OR the tool's security level exceeds what the
  current autonomy level auto-allows; OR the level is `MEDIUM_RISK` or higher and
  autonomy is below 4; OR the level is `HIGH_RISK` or higher (unconditionally).

Independent of all of the above, `AgentTool.requires_confirmation` (baked into the
tool's registration — `delete_file`, `git_commit`, `web_fetch`) is OR'd in by
`tool_registry_execute`, so those three tools always at least attempt confirmation
regardless of policy computation.

When confirmation is required but no `ctx->confirm` hook is attached (e.g. an
embedding context that never wired one up), `tool_registry_execute` **fails
closed** — refusing the call rather than defaulting to allow or deny-with-retry.

## The sandbox

`agent/security/sandbox.c` confines all file and working-directory access to a
single workspace root (`config/agent.json`'s `security.workspace`, default
`./workspace`). `sandbox_create()` creates the directory if missing and
canonicalizes it via `fs_realpath`. `sandbox_resolve(path)`:

1. Joins relative paths onto the root (absolute paths pass through as-is).
2. Canonicalizes the result with `fs_realpath` (resolves `..`, symlinks).
3. If the sandbox is enabled, checks the canonical result is within the canonical
   root (`within()` — exact match or the next character after the root prefix is
   a `/`). Any escape (including via `..` traversal or a symlink pointing outside)
   is rejected with a logged warning.
4. When the sandbox is disabled (`security.sandbox: false`), only canonicalization
   happens — no confinement check.

Every filesystem tool (`read_file`, `write_file`, `edit_file`, `list_directory`,
`delete_file`, `search_files`) and `shell_execute`'s `working_directory` argument
go through `sandbox_resolve`. Git tools use `sandbox_root()` with `git -C` instead
(and additionally block shell metacharacters in git arguments).

## Backups and rollback

`sandbox_backup(abspath)` copies a file to
`<root>/.agent/backups/<basename>.<seq>.<unix_ts>.bak` before it is modified or
deleted; the sequence number plus timestamp keep every version distinguishable.
`write_file` (when overwriting an existing file), `edit_file`, and `delete_file`
all call this before mutating. `sandbox_rollback(abspath)` finds the
newest-by-mtime backup whose name is prefixed `<basename>.` and copies it back
over the live file — this is how a `delete_file` call is described as "recoverable
via rollback" in its tool output, though note that no built-in tool currently
*exposes* rollback as a callable action; it is a security primitive the runtime
retains, not yet wired to a `rollback_file` tool for the model to invoke.

## The "never fake capabilities" honesty contract (§39)

Encoded directly in `agent/prompts/system.txt` ("Never claim that an action was
performed unless a tool actually performed it. Never invent files, tool results,
sources, commands, or successful outcomes. If a tool did not run, or failed, say
so plainly") and enforced structurally, not just by instruction:

- `web_search` has no configured backend and **always fails** with an explicit
  message rather than fabricating search results (`agent/tools/web.c`).
- `web_fetch` records the real `source_url` in its structured result and reports
  curl failures verbatim rather than inventing page content.
- Final verification (`verify_step` before accepting an `ACT_FINAL`) means a
  model's self-reported "done" is checked against real filesystem/exit-code state
  before the loop accepts it — the model cannot simply claim success.
- Tool execution failures propagate their real error text (`res->error`) into the
  observation the model sees next, rather than being smoothed over.

This is a design commitment, not merely a prompt: even if the model ignored the
instruction and claimed a false success, `verify_check` and the registry's
validation chain are the actual backstop.
