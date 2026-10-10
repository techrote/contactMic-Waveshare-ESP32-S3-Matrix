# Repository RAG index

This file is the retrieval map for agents working in this repository. Read it after \`AGENTS.md\`.

## Core authority

| Document | Purpose |
| --- | --- |
| \`docs/rag/00-SOURCES.md\` | Primary vendor/upstream source URLs and source precedence |
| \`docs/rag/01-PROJECT-CHARTER.md\` | Product goal, non-goals, architecture and quality targets |
| \`docs/rag/02-HARDWARE-CONTRACT.md\` | Waveshare board wiring and QMI8658C hardware constraints |
| \`docs/rag/03-SAMPLING-CONTRACT.md\` | Sensor modes, acquisition model, loss accounting and fallback policy |
| \`docs/rag/04-BLE-PROTOCOL-CONTRACT.md\` | Custom GATT service and v1 wire contract |
| \`docs/rag/05-HOST-AUDIO-CONTRACT.md\` | PC receiver, DSP, resampling and virtual-microphone integration |
| \`docs/rag/06-VERIFICATION-AND-CI.md\` | Automated test layers, hardware qualification and CI gates |
| \`docs/rag/07-EXECUTION-PROTOCOL.md\` | Issue/PR execution workflow and completion evidence |

## Programme

\`docs/programme/IMPLEMENTATION-PROGRAMME.md\` defines logical task IDs, dependencies, parallel lanes and the acceptance sequence.

## Retrieval rule

An issue need not paste every contract in full if it references this index and names its required reading. The referenced documents are part of the issue prompt.

If the live repository has advanced beyond text quoted in an issue, current accepted repository files outrank stale duplicated text. Agents must reconcile before implementation.
