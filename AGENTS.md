# FEX Switch 2 fork

AI-assisted development is allowed in this fork. Keep Switch 2 host code under
`Source/Switch2` where practical and preserve the existing Linux and Windows
builds. Keep generated binaries out of commits.

Build the Switch 2 probe with `make -C Source/Switch2` using the installed
ounce-devkit. Record build verification separately from console test results.

Continue working through implementation, build, and verification until the
requested outcome is delivered or concrete user action is required. Keep
working through recoverable failures and take authorized next steps without
waiting for another prompt.

Investigate and fix the root cause of a failure. Use diagnostic builds to
distinguish plausible causes, and label temporary experiments clearly.
Verify the fix against the original failure and revert experiments that
regress behavior or lack supporting evidence.
Do not take shortcuts that fake Linux behavior or bypass the requested path.
Implement the required semantics, verify them through the normal entry point,
and explicitly track any incomplete compatibility work.

## Switch 2 milestone handoff

Work on `nx2` and group commits at completed milestone boundaries. Start
or resume milestone work from the [restart checkpoint](Source/Switch2/NextMilestone.md#restart-checkpoint)
and check the branch and worktree with `git status --short --branch`.
Inspect the relevant diffs and untracked source before editing. Preserve
in-progress changes from the user and other milestones.

For a new milestone, record the outcome and observable acceptance checks in
`Source/Switch2/NextMilestone.md`. Refresh its restart checkpoint after each
verified slice and whenever the next step, blocker, or hardware evidence
changes. Include the remaining work, exact verification commands and observed
results, and matching console artifact paths when relevant. Keep decisions
that affect the next step in the handoff so a fresh session can resume from
the checkout alone. Continue only when asked if work is paused.
