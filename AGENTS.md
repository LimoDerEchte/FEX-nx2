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
- Graphics is in no milestone: it needs its own
  architecture planning first.
- Start from the [restart checkpoint](Source/Switch2/NextMilestone.md#restart-checkpoint):
  run `git status --short --branch` and review diffs and untracked files
  first. Preserve the user's in-progress changes.
- Graphics is shelved on branch `shelf/graphics` until the kernel syscall
  milestones are complete. Keep it out of `nx2`.
- Keep the checkpoint current after each verified slice: status,
  done-when checks, next step, blockers, exact verification commands
  with results, and console artifact paths. A fresh session must be able
  to resume from the checkout alone.
- For a console run, hand off the artifact paths and a complete checklist
  with expected results.
