# Mod Kit — Improvement & Defect Catalog

Working catalog of potential defects and improvement opportunities found during a
full read of the codebase. Each item is tagged with its category and, once assigned,
the worktree branch / PR that implements it. Items are implemented one per git
worktree so they can be reviewed and merged independently.

Categories: **defect** (incorrect/fragile behavior), **feature**, **enhancement**,
**ux** (interface clarity/discoverability), **qol** (quality of life).

## Assigned to worktrees

### Slice A — branch `improve/shader-editor-qol` (PR #2, MERGED to main as bcd52b4)
- **[qol] Open mod directory from the UI.** The Session tab prints the mod root path
  as text, but there is no button to reveal it in Explorer. Add an "Open mod folder"
  button next to the path (reuse `open_mod_path`). *modkit.cpp `draw_panel`.*
- **[ux] Show active draw rules inline.** Rules can only be inspected by opening
  `rules.ini` in the editor. In the "Editor / rules" tab, render a read-only summary
  of currently loaded rules (name, kind, action, shader id, count/instances) from
  `s.rules` so users see what is active without switching files. *modkit.cpp `draw_panel`.*
- **[ux] Clearer bytecode-import diagnostics.** `import_shader_bytecode` only checks
  that the imported DXBC/DXIL stage prefix matches; a version mismatch (e.g. ps_6_0
  into a ps_5_0 slot) fails later at pipeline creation with a generic message. Detect
  and report a full-profile mismatch up front. *modkit.cpp `import_shader_bytecode`.*
- **[docs] Document the above** in MODKIT.md (editor / rules section).

### Slice B — branch `harden/parsing-tests` (PR #1, MERGED to main as 024c9f8)
- **[defect/robustness] Unbounded shader macros & include dirs.** `shader_definitions`
  and the include-dir list have no cap; a crafted `.compile.ini` or pasted editor input
  can supply an unbounded number of `-D` / `-I` arguments. Add sane caps with clear
  error messages. *modkit_shader_edit.hpp.*
- **[defect/robustness] Harden option-line parsing.** `shader_option_lines` should be
  explicitly guarded against pathological lines and the caps above enforced at the
  single parse entry point. *modkit_shader_edit.hpp.*
- **[test] Add regression tests** for the new caps and validation in
  `tests/shader_edit_checks.cpp` (and `profile_checks.cpp` where relevant).

### Slice C — branch `improve/profiler-frame-ux` (PR #3, MERGED to main as 7997932)
- **[ux] Profiler/frame UX.** Improve the Profiler and Frame/pipeline tabs: clearer
  empty-state guidance, consistent column/label naming, and a visible indicator when
  the displayed frame is stale (pre-timing readback). *modkit.cpp `show_profiler`,
  `show_draws`.*
- **[docs] Document profiler/capture behavior** in MODKIT.md.

### Slice D — branch `harden/poll-files-state` (PR #4, MERGED to main as 2432968)
- **[defect/robustness] Unbounded function-local static in `poll_files`.** The
  `static std::map<std::filesystem::path, std::filesystem::file_time_type> rule_times;`
  at modkit.cpp ~1272 grows per distinct rules.ini path and is never freed on device
  destroy. Move the last-seen timestamp into `device_state` (freed with the device)
  and drop the static. *modkit.cpp `poll_files`, `device_state`.*
- **[qol] Cosmetic indentation in `poll_files`.** Lines ~1267, 1269, 1280 were
  reported as unindented (suspected merge artifact). Verified during implementation:
  those lines are already correctly indented at one tab in origin/main — no-op.

## Reviewed / deferred (noted, not yet scheduled)
- **[feature] In-editor syntax highlighting / line numbers.** The editor is an ImGui
  `InputTextMultiline`; a lightweight embedded text editor would be a large change and
  is out of scope for the current pass.

## Process
- One improvement per worktree/branch; see `.agents/skills/worktree-improvement-workflow`.
- Every PR is reviewed by role-focused subagents with clean context (code correctness,
  memory safety, UX/QoL, build/CI) — see `.agents/skills/pr-review` and the
  `reviewer-*` skills.
- Before merging all four PRs to main, an integration branch (`int/combined`) merges
  them in order (#1 → #2 → #3 → #4), resolves any cross-PR overlap (modkit.cpp,
  modkit_shader_edit.hpp, shader_edit_checks.cpp), and runs the full local + CI
  verification on the combined tree. Then each PR merges to main individually in the
  same order.

## Outcome (2026-10-21)
All four slices merged to main: #1 → 024c9f8, #2 → bcd52b4, #3 → 7997932, #4 → 2432968.
The integration draft (PR #5 on `int/combined`) ran the full Windows `.exe` suites green
on the combined tree before any merge to main; it was then closed without merging. One
cross-PR conflict (tests/shader_edit_checks.cpp — PR #1 and PR #2 both appended test
blocks before the success line) was resolved by keeping both suites, once in the
integration branch and once when PR #2 re-based onto post-#1 main. Main CI green on
bcd52b4; all worktrees removed.

## Non-blocking follow-ups (noted by reviewers, deferred)
- **[docs] One-liner in MODKIT.md Draw rules section:** "External edits to rules.ini are
  picked up automatically while Watch replacement files is enabled." (PR #4 ux-qol.)
- **[ux] House-style the pre-existing stage-mismatch import message** at modkit.cpp
  (~2357): add consequence + next step. (PR #2 ux-qol.)
- **[test] mtime-gating harness for poll_files:** inject file mtimes to exercise
  first-load / skip / re-read / retry-after-failure. Pre-existing gap, not introduced by
  PR #4. (PR #4 code-correctness.)
- **[docs] Document failure-time out-param state** of shader_definitions / parse_include_dirs
  (clear-on-entry leaves a partial fill on failure; all callers check the return first).
  (PR #1 code-correctness nit.)
