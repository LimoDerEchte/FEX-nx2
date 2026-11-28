# FEX Switch 2 fork

AI-assisted development is allowed. Goal: run unmodified x86-64 Linux
software on Switch 2 through the normal launcher.

## Rules

- **Scope.** Put Switch 2 code in `Source/Switch2`. Keep the Linux and
  Windows builds working. Never commit generated binaries.
- **Builds.** `make -C Source/Switch2` builds the JIT probe with the
  installed ounce-devkit. Use one launcher tree, `Source/Switch2/build/guest`,
  and stage the SD bundle into `build/guest/sd`. Ship one NRO per console
  run; per-launch test behavior goes in sidecar files, not build options.
- **Real semantics.** Implement Linux behavior properly, verified through
  the normal entry point. No faked behavior or bypassed paths. Track any
  incomplete compatibility explicitly.
- **Performance.** Hand-written AArch64 assembly is allowed for hot
  paths (frequent calls, dispatch, syscall entry), as FEX itself does.
  Keep a comment saying what it replaces and why, and measure the gain
  on the console before keeping it.
- **Root causes.** Fix the cause, not the symptom. Label diagnostic
  builds as experiments and revert any that regress or lack evidence.
- **Evidence.** Report host build, native Linux and Switch 2 console
  results separately. Claim only what was observed.
- **Finish the job.** Work through recoverable failures without waiting
  for prompts. Stop only when the outcome is delivered or user action is
  required (e.g. a console run). If work is paused, resume only when asked.

## Milestone workflow

- Work on `nx2`; commit once per completed milestone after its console
  acceptance passes.
- Milestone process: write a plan split into a small number of tasks.
  One Sonnet agent implements each task, one task after another, never
  in parallel. Then one Sonnet agent reviews the whole milestone. Then
  the Opus orchestrator fixes review findings, writes the test suite and
  hands the user console runs to debug until acceptance passes.
- Codex only when the user asks for it. By default, tasks go to Sonnet
  agents as above. The user used Codex for M5 Task 8 and for finishing M5;
  a Codex session that takes over reads its handoff file first, as the
  user names it.
- The orchestrator does complex work itself: crashes, hangs, root-cause
  debugging of console failures and subtle runtime bugs. Delegate only
  well-specified, mechanical tasks (UI tweaks, docs, staging) to agents.
- Milestone order: M5 `/proc`, `/sys`, `/dev` and input; M6 non-PIE
  executables; M7 Wayland compositor; M8 graphics acceleration; M9 32-bit
  guests. Plans are in `Source/Switch2/docs/Milestone<N>Plan.md`. M7 and M8
  need their own architecture plans before they start.
- Start from the [restart checkpoint](Source/Switch2/NextMilestone.md#restart-checkpoint):
  run `git status --short --branch` and review diffs and untracked files
  first. Preserve the user's in-progress changes.
- Graphics is shelved on branch `shelf/graphics` until M7 starts. Keep it
  out of `nx2` until then.
- Keep the checkpoint current after each verified slice: status,
  done-when checks, next step, blockers, exact verification commands
  with results, and console artifact paths. A fresh session must be able
  to resume from the checkout alone.
- For a console run, hand off the artifact paths and a complete checklist
  with expected results.
- Final pass of a milestone: run `final-pass-suite.elf` (Settings >
  Developer > Final Pass Suite > Run). It runs every required suite as
  its `.user` identity and prints the totals. Add each new suite to
  `FINAL_PASS_SUITES` in `tools/StageDebian.py`. Every suite prints
  `# <name> summary: P/F/S (passed/failed/skipped)` after `# failed:`
  lines, writes its counts to `$FEX_SUITE_RESULTS`, and exits 42 when
  nothing failed.
