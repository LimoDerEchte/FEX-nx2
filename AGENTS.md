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

Use `Source/Switch2/NextMilestone.md` as the durable handoff for the active
Switch 2 milestone. Read it after compaction or resuming the project. Update it
when the milestone or its hardware verification changes. Work on the `nx2`
branch and group commits at completed milestone boundaries.
