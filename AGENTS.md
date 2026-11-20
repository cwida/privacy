# AGENTS.md

Guidance for coding agents working in this repository.

The project instructions are maintained in one place, [`CLAUDE.md`](CLAUDE.md). Read it in full before making
changes: it covers how to work with the user, the development rules, what the extension does, how to build and test,
the compilation pipeline, naming conventions, key source files, DDL examples and code style.

One rule in addition to those in `CLAUDE.md`: never weaken a test to match current behaviour. If a test exposes a
bug, keep the test and fix the bug.
