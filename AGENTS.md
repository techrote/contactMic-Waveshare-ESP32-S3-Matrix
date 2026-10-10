# Agent operating contract

This file is mandatory reading for every implementation issue in this repository.

## Authority and scope

Resolve conflicts in this order:

1. current accepted files on the repository default branch;
2. the current issue body and later issue comments that explicitly amend it;
3. the RAG documents indexed by \`RAG.md\`;
4. upstream vendor documentation linked from \`docs/rag/00-SOURCES.md\`;
5. implementation assumptions.

Do not silently override accepted repository contracts. If an upstream source contradicts an accepted repository contract, stop, document the conflict on the issue, and make the smallest evidence-backed contract update required.

Implement only the active issue. Do not absorb work assigned to dependent or parallel issues unless the active issue cannot be made correct without it; in that case record the dependency explicitly.

## Required start-of-task reconciliation

Before editing:

- fetch/reconcile the current default branch;
- read the complete active issue body and comments;
- inspect existing branches and PRs for the logical task ID;
- inspect current CI state;
- read \`RAG.md\` and every document named in the issue's Required reading section;
- preserve valid work already present if the repository advanced after the prompt was written.

Never assume hashes, branch names, issue state, or dependency state are unchanged.

## Branch, commits and pull request

Use one dedicated branch per issue, normally:

\`cm-###-short-description\`

Keep commits reviewable and scoped. Do not mix unrelated cleanup.

Before opening the PR, run all checks that can run locally.

Open one PR to the default branch. Use a short, factual body containing:

- logical task ID and GitHub issue number;
- what changed;
- verification performed;
- known limitations, if any;
- exactly one closing directive for the active issue, e.g. \`Closes #12\`.

Keep the PR body minimal. Do not add unrelated issue-closing syntax.

## CI monitoring: mandatory 120-second cadence

After pushing a commit or opening/updating a PR:

- query CI status;
- if checks are pending/running, **wait at least 120 seconds before the next CI status query**;
- repeat at no shorter interval;
- do not tight-poll individual jobs, runs, checks, logs, mergeability, or commit status as a substitute;
- if a job fails, inspect the failed job/logs, fix the cause, push the correction, and restart the same 120-second monitoring cadence.

The 120-second rule applies equally to post-merge verification.

## Merge gate

Do not merge while any required automated check is pending, cancelled, skipped unexpectedly, or failing.

When all required checks are green:

1. re-read the PR diff for scope and accidental changes;
2. confirm the PR head still matches the tested head;
3. squash-merge unless repository policy says otherwise;
4. verify the merge result;
5. wait at least 120 seconds before the first post-merge CI status query if a default-branch workflow is expected;
6. verify default-branch checks are green;
7. record concise completion evidence on the issue if the PR/CI state alone does not make acceptance obvious.

Do not claim completion merely because code was pushed.

## Hardware-gated work

Most CI is host/simulator based. An issue may explicitly require a Waveshare ESP32-S3-Matrix attached to a runner or operator workstation.

For hardware-gated issues:

- do not fabricate hardware observations;
- do not substitute simulator results for required physical measurements;
- use the issue-specified qualification scripts and capture machine-readable evidence where possible;
- if required hardware is unavailable, record the exact blocker and leave the issue open rather than weakening its acceptance criteria.

## Engineering rules

- ESP-IDF is the firmware authority unless a later accepted architecture decision changes it.
- Pin numbers and sensor capabilities must come from \`docs/rag/02-HARDWARE-CONTRACT.md\`, not copied from third-party examples.
- Do not call the stream a Bluetooth HFP microphone or LE Audio microphone.
- Preserve protocol backward compatibility within a protocol major version.
- Acquisition must never block on BLE transmission.
- All loss/overflow/error paths must be observable through counters or protocol flags.
- Hardware-dependent claims require evidence.
- Routine CI must not upload large build artifacts or enable unnecessary long-lived caches.
