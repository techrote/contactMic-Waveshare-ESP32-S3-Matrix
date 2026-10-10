# Issue execution protocol

This document supplements \`AGENTS.md\`. If they differ, \`AGENTS.md\` wins.

## Issue prompt structure

Every implementation issue must contain:

- logical task ID;
- goal;
- dependency state;
- required reading;
- exact in-scope work;
- explicit non-scope;
- required tests/evidence;
- acceptance criteria;
- branch suggestion;
- PR/CI/merge instructions;
- hardware requirements, when applicable.

References to repository RAG documents are part of the prompt; agents do not need duplicated copies of the same contract.

## Start

Agent actions:

1. reconcile live default branch;
2. inspect issue comments, branches, PRs and CI for the task ID;
3. read \`AGENTS.md\`, \`RAG.md\`, and required issue documents;
4. confirm dependencies are accepted;
5. create/reuse the scoped branch without discarding valid work.

If a dependency is not accepted, do not implement dependent behavior speculatively.

## Development

Keep hardware adapters behind narrow interfaces and keep protocol/DSP/state-machine logic host-testable.

Update RAG only when the active task legitimately changes an accepted contract.

Add tests with implementation rather than deferring basic verification.

## PR and CI

Use one PR per issue.

The PR should make the issue acceptance reviewable without requiring archaeology across unrelated branches.

After each push, inspect CI and then wait at least 120 seconds before another status check while jobs are pending. On failure, inspect logs once, fix the root cause, push, then resume 120-second polling.

## Review before merge

Before merge:

- inspect changed file list and diff;
- ensure no generated build outputs or secrets were committed;
- ensure protocol/docs were updated if behavior changed;
- ensure all required automated checks are green;
- ensure no unresolved review comments remain;
- ensure the tested PR head is still current.

Then squash-merge.

## Post-merge

After merge:

- confirm the merge landed on the default branch;
- if default-branch CI runs, wait 120 seconds before polling and use the same 120-second cadence until terminal;
- record acceptance evidence;
- close the issue only when its acceptance criteria are satisfied.

## Blockers

If blocked by unavailable physical hardware, permissions, external outage, or a contradicting authority source, document the exact blocker and preserve completed work. Do not invent evidence or weaken acceptance criteria.
