# Repository RAG index

This file is the retrieval map for agents working in this repository. Read it after `AGENTS.md`.

## Core authority

| Document | Purpose |
| --- | --- |
| `docs/rag/00-SOURCES.md` | Primary vendor/upstream source URLs and source precedence |
| `docs/rag/01-PROJECT-CHARTER.md` | Product goal, non-goals, architecture and quality targets |
| `docs/rag/02-HARDWARE-CONTRACT.md` | Waveshare board wiring and QMI8658C hardware constraints |
| `docs/rag/03-SAMPLING-CONTRACT.md` | Sensor modes, acquisition model, loss accounting and fallback policy |
| `docs/rag/04-BLE-PROTOCOL-CONTRACT.md` | Custom GATT service and v1 wire contract |
| `docs/rag/05-HOST-AUDIO-CONTRACT.md` | PC receiver, DSP, resampling and virtual-microphone integration |
| `docs/rag/06-VERIFICATION-AND-CI.md` | Automated test layers, hardware qualification and CI gates |
| `docs/rag/07-EXECUTION-PROTOCOL.md` | Issue/PR execution workflow and completion evidence |

## Implementation details

- `docs/CM-002-DRIVER.md`: register policy, vendor discrepancies and driver lifecycle.
- `docs/CM-003-ACQUISITION.md`: DRDY/task ownership, bounded SPSC ring, source-index
  gap semantics, measured-rate window and explicit rate-policy transitions.
- `docs/CM-004-WIRE-V1.md`: exact descriptor/control/status bytes, session handshake,
  nonblocking source interface and canonical vectors. Completes previously unspecified
  v1 fields without changing the fixed RAG audio layout.
- `docs/CM-005-HOST.md`: installation, receiver lifecycle, DSP, bounded buffers,
  loss/recording semantics, diagnostics and virtual-audio routing.
- `docs/CM-006-INTEGRATION.md`: current integrated source/session execution authority,
  nonblocking generation handoff, calibration/publication boundary, AUTO policy and
  deterministic end-to-end qualification. Supersedes earlier milestone notes saying
  the app has no acquisition source; unchanged v1 bytes and physical gates remain.
- `docs/CM-009-USB-AUDIO-CONTRACT.md`: additive native UAC2/CDC route, isolated USB
  firmware, on-device DSP/clock reconciliation, controls, recovery, USB-only evidence
  runner and physical acceptance boundary. Earlier BLE-only host/metadata statements
  remain authoritative for BLE, not as restrictions on the separately authorized USB route.
- `docs/CM-009-ACQUISITION-REVIEW.md`: required CM-009 qualification addendum and
  primary-source supplement: rejected coherency waiver, lock/FIFO loss and timing
  bounds, mandatory offline source-ledger audit, and unresolved vendor question.
  It does not change accepted acquisition/BLE behavior or the MEDIUM target.
- `docs/CM-009-CLOUD-HANDOFF.md`: exact-candidate anchor, operator build/provenance,
  fresh enumeration, short diagnostic/audit procedure and held sustained gates.
- `evidence/cm009/2026-10-16-cloud-review.md`: inspected versus reported evidence,
  missing raw artifacts and cloud-only checkpoint scope; historical reports stay sealed.
- `docs/DEVELOPMENT.md`: firmware/host build and test commands; CM-006 adds the
  integration test commands linked above. CM-009's isolated project commands are in
  its USB contract; do not reuse the BLE sdkconfig/build directory for USB.

## Programme

`docs/programme/IMPLEMENTATION-PROGRAMME.md` defines logical task IDs, dependencies, parallel lanes and the acceptance sequence.

## Retrieval rule

An issue need not paste every contract in full if it references this index and names its required reading. The referenced documents are part of the issue prompt.

If the live repository has advanced beyond text quoted in an issue, current accepted repository files outrank stale duplicated text. Agents must reconcile before implementation.
