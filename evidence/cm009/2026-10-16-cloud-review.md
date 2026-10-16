# CM-009 cloud-only acquisition investigation checkpoint

## Scope and disposition

Parent inspected: `15d97bb2adb10c7b152d2cfd9bd1ca6f2b06b4e9`; main `0d4076131f9ff699b44a1eb3bcf8fcf95f99c93d`. Same branch `cm-009-usb-audio`, draft PR #17, issue #16. The pre-change proposal is [comment 6215982885](https://github.com/techrote/contactMic-Waveshare-ESP32-S3-Matrix/issues/16#issuecomment-6215982885).

**No supported in-scope acquisition correction capable of the unchanged MEDIUM target was established.** The sensor driver, acquisition behavior, USB adapter/core, BLE application/protocol/defaults and dependencies are unchanged by this checkpoint. Added software: offline acquisition budget/ledger audit, adversarial host regressions, production acquisition-core counter-boundary regression, and verified non-editable isolated host CI. This is not a successful MEDIUM implementation or a hardware acceptance report.

The exact successor candidate SHA and normal exact-head CI outcome are published in the issue/PR checkpoint after the commit exists. The introducing/updating commit of `docs/CM-009-CLOUD-HANDOFF.md` is the immutable operator candidate anchor. No passing CI outcome is asserted in advance by this file.

## Directly inspected versus reported

Directly inspected through GitHub: AGENTS/RAG, issue #16 and all its then-existing comments, PR #17 and review, current main/head, existing workflow, required contracts, committed qualification report and relevant production/test sources. Primary QST Rev A text and pinned Waveshare code were inspected. The PDF screenshot renderer failed, so waveform figures were not visually verified.

The prior Windows native UAC2/CDC enumeration/recovery, exact board and ELF identities, pulse polling observations, MEDIUM/HIGH preflight failures and SAFE 32/9136 misses in 10.224230 seconds are **operator-reported in the committed report/comments**, not independently reproduced here. No original workstation, physical board or ignored raw files were accessible.

Historical flashed source: `79d8734f1ea391db5f35a698334e3abed1a80512`. Historical reported ELF SHA-256: `465dbb4d0037ef575bc1d725b04ab5574bae3c2e1c7417e874407b24a19bd211`. These identifiers are references, not proof that their underlying bytes or physical provenance were verified in cloud compute.

## Missing raw evidence and dependent conclusions

The report refers to ignored `build/cm009/session-20261016/`, `preflight-pulse-safe-016/`, `preflight-pulse-medium-017/`, `preflight-pulse-high-018/` and `control-validation-019/`. Their raw telemetry/control bytes, capture metadata/summaries/manifests, recordings if present, and operator build/flash/provenance artifacts were not available here.

Independent replay of matched-window physical losses, diagnostic timing distributions, reported device identity and exact runtime ELF provenance requires those original bundles, actual ELF/bin, build logs, resolved dependencies, sdkconfig/project description, flash log and associated checksums. A hash quoted in a report does not supply any of those bytes. A summary alone cannot establish the pairing of clocks/counters or physical origin. Historical reports and worse failures are preserved unchanged.

The wire-floor analysis and software regressions do not depend on possessing those raw files. They use the manufacturer's sequence, the fixed bus ceiling and explicitly labeled reported/reference rates. Details, contradictions and the precise unresolved vendor question are in `docs/CM-009-ACQUISITION-REVIEW.md`.

## Verification and limits

Before publication, the 15 pure budget/ledger/vendor-reference tests passed in a local cloud Python 3.13.5 venv from a non-editable wheel; the new module's installed source hashes and provenance were verified. That local install contained only the new audit subset, not a full repository build. Byte compilation also passed. Full-repository host/integration/native/tooling and both firmware builds are delegated to the existing normal GitHub CI workflow at the published candidate; their result must be read from the exact-head checkpoint, not inferred from this subset run.

New integration tests retain the original evaluator and explicitly demonstrate its false pass on fabricated half-speed or inconsistent ledgers. The supplementary audit must hold them without rewriting the original verdict or evidence. The new native regression uses the real acquisition core to observe its fresh one-read-in-flight counter boundary. Synthetic lock/drop reference tests are not a implemented sensor driver, a sensor timing qualification or proof of undisclosed register behavior.

No MEDIUM 600-second evidence exists for this cloud candidate. Use the operator handoff for offline replay/build/provenance and only specifically justified short diagnostics. Do not repeat sustained testing on unchanged acquisition. The next step is the exact manufacturer clarification or a separately authorized architecture decision described in the review. #16 stays open, #17 draft; BLE hardware/#7/#15 remain held; no CM-008 work.
