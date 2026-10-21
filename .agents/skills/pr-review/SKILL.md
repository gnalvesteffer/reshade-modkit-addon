---
name: pr-review
description: Review a Mod Kit pull request with role-focused subagents (code correctness, memory safety, UX/QoL, build/CI) in clean contexts, then consolidate verdicts. Use when any PR is ready for review or re-review after fixes.
---

# Role-based PR review

Mirrors a small software team: several reviewers, each focused on one responsibility, all starting from a clean context (no prior conversation). The orchestrator dispatches them and consolidates; it does not review the diff itself beyond consolidating.

## Roles

| Role | Skill | Focus |
| --- | --- | --- |
| Code correctness | `reviewer-code-correctness` | Logic, edge cases, API misuse, test coverage of the changed behavior |
| Memory safety & resource lifetime | `reviewer-memory-safety` | Leaks, UAF, COM refcounts, buffer bounds, lock discipline in this codebase's hook/COM context |
| UX / QoL | `reviewer-ux-qol` | User-visible clarity, discoverability, status messages, docs accuracy |
| Build & CI | `reviewer-build-ci` | Cross-platform compile, check suites, packaging/CI impact, diff hygiene |

## Process

1. Get the diff: `gh pr diff <number>` (or `git diff origin/main...<branch>` locally).
2. Dispatch one subagent per role in parallel. Each prompt must contain: the PR number and URL, the full diff, the branch name, the repo worktree path to read surrounding context from, a pointer to this repo's `.agents/skills/<role-skill>/SKILL.md`, and the instruction to end with a verdict of `APPROVE`, `REQUEST_CHANGES`, or `COMMENT` plus a numbered findings list (file:line, severity, concrete fix).
3. Consolidate: dedupe overlapping findings, rank by severity (blocker > major > minor > nit).
4. Blockers/majors go back to the implementing worker as a fix list; re-run review after fixes. Comments/nits are optional. Dispatch ONE consolidated fix worker per PR covering all actionable findings — never overlapping fix workers on the same worktree (concurrent `git add`/index writes corrupt each other). If you dispatched an early narrow fixer before all verdicts landed, stop it (`delegate_task action='stop'`) once the superset fixer starts.
5. Only when all roles report `APPROVE` (or remaining items are accepted nits) and CI is green, merge with `gh pr merge <number> --squash`.

## Ground rules for reviewers

- Review the diff against the codebase as it exists on the branch; read surrounding code, not just the hunk.
- Cite file:line for every finding. No vague "consider improving X" without a concrete suggestion.
- Do not propose refactors beyond the PR's scope; flag them as `COMMENT` at most.
- Windows-only behavior (COM, DXC, ReShade runtime) cannot be executed on Linux — reason about it from headers and existing patterns, and say so in the verdict.
