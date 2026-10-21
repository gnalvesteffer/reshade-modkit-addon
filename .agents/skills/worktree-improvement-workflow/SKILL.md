---
name: worktree-improvement-workflow
description: Implement one isolated improvement to the Mod Kit add-on in its own git worktree and branch, verify it locally, and open a PR for role-based review. Use when making any feature, fix, or QoL change that should land as an independent PR.
---

# Worktree-per-improvement workflow

Each improvement gets its own worktree so changes stay isolated, parallelizable, and independently reviewable.

## Set up the worktree

```sh
cd /home/xorberax/dev/reshade-modkit-addon
git fetch origin
git worktree add /home/xorberax/dev/modkit-worktrees/<short-name> -b <branch> origin/main
```

Branch naming: `fix/<topic>`, `improve/<topic>`, or `harden/<topic>`. One logical improvement per branch — if a change grows into two, split it.

## Rules while implementing

- Read the relevant source and `MODKIT.md` first; match the existing style (tabs, no spaces for indentation, brace style, `report(s, ...)` for user-facing status).
- Keep diffs minimal: no drive-by reformatting, no unrelated cleanups, never touch `vendor/`, `build-mingw/`, or CI unless the change requires it.
- Add or update tests when behavior changes; the two check suites are `tests/profile_checks.cpp` (portable) and `tests/shader_edit_checks.cpp` (Windows).
- Update `MODKIT.md` when user-visible behavior changes, and note the item in `IMPROVEMENTS.md`.

## Verify before opening a PR

```sh
# Portable checks run on Linux:
c++ -std=c++17 -Wall -Wextra -Werror -pedantic -Isource tests/profile_checks.cpp -o /tmp/pchk && /tmp/pchk
# Full Windows cross-build (add-on + both check executables):
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=mingw64.cmake -DCMAKE_BUILD_TYPE=Release -DMODKIT_BUILD_CHECKS=ON
cmake --build build --parallel 8
```

Report exactly which checks ran and which could not run locally (the two `.exe` check suites need Windows/CI to execute; a successful cross-compile is compile coverage only).

## Open the PR

```sh
git push -u origin <branch>
gh pr create --base main --head <branch> --title "<imperative summary>" --body-file /tmp/pr-body.md
```

PR body: what changed, why, which checks ran locally, and anything the reviewer must verify on Windows. Do not merge your own PR — role-based review (see `pr-review`) decides when it is ready.

## Closing out a multi-PR campaign (integration gate)

When several slices are reviewed and approved but all touch overlapping files (e.g.
`modkit.cpp`, shared headers, the same test file), do NOT merge them to main one by
one blind — prove the combined state first:

1. Build an integration branch from `origin/main`: merge each slice in a sensible
   order (`git merge --no-ff <branch>`). Expect conflicts where two PRs appended to
   the same region (typical: test blocks before a file's final line) — resolve by
   keeping **both** sides, in a deliberate order.
2. Verify the combined tree locally (portable checks + full MinGW cross-build), push
   the branch, and open it as a **draft PR to main** so CI runs the Windows `.exe`
   suites on the merged state. (Plain branch pushes do not trigger this repo's CI —
   only `push: branches [main]`, tags, and `pull_request: branches [main]`.)
3. Once the draft is green, merge the individual PRs to main in the same order. A PR
   whose shared-file conflict was already resolved on the integration branch will hit
   it again when merging against a main that now contains the earlier PR — resolve
   identically (rebase the branch onto `origin/main`, fix, push, re-merge).
4. Close the draft without merging; delete all slice branches and worktrees; record
   the outcome in `IMPROVEMENTS.md`.

GitHub's stacked-PR feature is the alternative when slices are genuinely dependent
(each layer needs the one below to build) or the campaign keeps growing over time; for
a one-shot batch of independent, already-reviewed slices the integration gate above is
simpler and avoids re-basing every branch.
