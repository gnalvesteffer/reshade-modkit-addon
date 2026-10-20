# Repository skills

These skills give contributors and coding agents repeatable workflows for common Mod Kit tasks.

| Skill | Use it for |
| --- | --- |
| [Add-on build and release](addon-build-release/SKILL.md) | Building, packaging, checking CI and preparing a tagged release |
| [Graphics capture debugging](graphics-capture-debugging/SKILL.md) | Diagnosing missing or incorrect draw, shader, resource and timing data |
| [Shader and texture modding](shader-texture-modding/SKILL.md) | Creating, editing and validating shader or resource mods |
| [Generic game deployment](game-deployment/SKILL.md) | Installing this add-on with ReShade into a selected game and verifying or reverting it |
| [Worktree improvement workflow](worktree-improvement-workflow/SKILL.md) | Implementing one isolated improvement in its own worktree/branch, verifying it, and opening a PR |
| [PR review (role-based)](pr-review/SKILL.md) | Reviewing a PR with parallel role-focused subagents and consolidating verdicts |

Reviewer roles used by the PR review workflow: [code correctness](reviewer-code-correctness/SKILL.md), [memory safety](reviewer-memory-safety/SKILL.md), [UX / QoL](reviewer-ux-qol/SKILL.md), [build & CI](reviewer-build-ci/SKILL.md).

Use the narrowest matching skill. For changes that affect implementation, read the relevant source and `MODKIT.md` as well; these skills do not replace the API's runtime limitations documented there. Improvement candidates are tracked in `IMPROVEMENTS.md`.
