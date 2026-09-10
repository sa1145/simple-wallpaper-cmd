---
name: dx12-action-workflow
description: Run this repository's structured handoff-based Terra implementation and Sol read-only review loop when the user explicitly requests an Action Item, TASK-NNN, Terra/Sol, handoff, or retry workflow. Do not use for ordinary edits, builds, reviews, questions, or Git operations.
---

# DX12 Action Workflow

Use this skill only for a user-requested structured Action Item. Ordinary repository work follows `AGENTS.md` directly.

## Roles

- The current agent is the orchestrator: analyze scope and risk, persist the task, dispatch work, interpret verdicts, and update status.
- Use exactly one `gpt-5.6-terra` implementer for an Action Item.
- After that implementer finishes, use exactly one independent `gpt-5.6-sol` reviewer in strict read-only mode.
- Keep these models unless the user explicitly overrides them for the current Action Item.
- Never run the implementer and reviewer concurrently.
- For rework, reuse the same implementer and reviewer with follow-up messages. Spawn a new pair only for the next Action Item.
- Specify the model explicitly and use `fork_turns="none"` or a limited turn count when spawning.

## Persist the task

Before dispatching implementation, create the next `.handoff/TASK-NNN.md` and `.handoff/TASK-NNN.json`.

The Markdown task must state:

1. 我要做什麼
2. 為什麼做
3. 架構
4. 限制
5. 要改哪些檔案（精確 allowlist）
6. 完成條件（可執行、可觀察）

The JSON task records at least the task ID, status, Terra/Sol models, current attempt, five-attempt limit, allowlist, acceptance criteria, review history, and runtime gate.

Do not read `.handoff/failed/`, checkpoints, or backup archives unless the user explicitly authorizes it.

## Implementation loop

1. Confirm architecture, hardware limits, and whether interactive runtime is locked or explicitly unlocked.
2. Persist the task files before spawning.
3. Spawn the single Terra implementer. Provide absolute task/handoff paths, the exact allowlist, runtime gate, build commands, check commands, and acceptance criteria.
4. Wait for Terra to finish. No overlapping repository edits are allowed.
5. Spawn the single Sol reviewer after Terra stops.
6. Require a `PASS` or `FAIL` verdict supported by `file:line`, existing artifacts, and permitted check output.
7. On `PASS`, independently confirm every acceptance criterion before marking the task `COMPLETE`.
8. On `FAIL`, create the next `TASK-NNN-REWORK-###.md` and `.json` containing only the reviewer's verified blocker and minimum acceptance test. Send it to the same Terra implementer.
9. When rework finishes, wake the same Sol reviewer for a new verdict; never reuse a previous verdict.
10. Count the initial implementation as attempt 1. Stop after five Terra attempts and mark `BLOCKED_RETRY_EXHAUSTED` if the fifth verdict is still `FAIL`.

## Local review commit

- This rule applies only to future Action Items; do not retroactively commit, re-review, or relabel earlier work.
- When the final implementation for one Action Item changes two or more files, create exactly one local commit after the final Sol `PASS` for the currently authorized scope and the orchestrator's independent acceptance check. One-file or record-only Actions do not get an automatic commit.
- Between the reviewer's stop and the commit, allow no other edits. Reinspect `git status` and the reviewed diff, then stage only the Action's implementation allowlist. If any reviewed implementation file changed, invalidate the verdict and obtain a fresh review before committing.
- The commit message records the task ID, final attempt, Sol verdict, and runtime status. The Git commit is the content identity; do not create a separate SHA-256 manifest.
- Do not include `.handoff`, `doc_handoff`, build outputs, or unrelated user changes in this commit. Never push it automatically, and do not amend or rewrite it while the Action remains open.
- Terra and Sol never commit. This narrow exception belongs only to the orchestrator and does not authorize commits for ordinary repository work.

## Implementer constraints

- Read the task and referenced handoff completely before modifying files.
- Modify only the allowlist, using local patches rather than delete/recreate rewrites.
- Preserve unrelated dirty and untracked files; do not commit.
- Prefer one shared root-cause fix using existing native mechanisms. Do not add unrelated interfaces, factories, dependencies, or features.
- Run only authorized builds/checks and report exact commands, exit codes, key output, and residual processes.
- Stop and report conflicts or required scope expansion instead of changing the task specification.

## Reviewer constraints

The Sol reviewer may read task files, source, diffs, existing artifacts, and logs; it may run already-built checks and read-only system queries.

It must not edit, format, configure, build, redirect output into the repository, create or delete files, commit, or act as the implementer. A build success is not runtime evidence. A `FAIL` verdict must identify the location, reason, required behavior, and reproducible acceptance check.

## Runtime and hardware gate

Follow the runtime and hardware rules in repository `AGENTS.md`. User authorization applies only to the requested runtime scope. Run one interactive check at a time and verify cleanup before continuing. Never claim unavailable multi-monitor, hotplug, or cross-adapter hardware validation.

## Completion report

Report only changed files, core behavior, build/check results, reviewer verdict, unverified risks, and residual resources. Mark every unrun condition explicitly.
