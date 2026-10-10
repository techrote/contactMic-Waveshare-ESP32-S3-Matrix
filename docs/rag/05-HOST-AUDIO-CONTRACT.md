# Host audio contract

## Purpose

The PC side converts the custom BLE vibration stream into conventional mono audio without hiding source quality.

Initial implementation language: Python 3.

Primary libraries may include Bleak, NumPy and python-sounddevice.

## Layering

Keep these layers separable:

1. BLE discovery/session/control;
2. binary protocol decoder;
3. loss/discontinuity accounting;
4. source-domain DSP;
5. resampler;
6. audio sink / WAV sink;
7. CLI/diagnostics.

Protocol and DSP tests must run without Bluetooth or audio hardware.

## Decoder

Decoder must:

- reject unsupported protocol-major versions;
- reject malformed frame lengths/counts;
- preserve signed 16-bit samples;
- track packet sequence and source sample index separately;
- detect exact missing source intervals from sample indexes;
- distinguish reconnect/session reset from packet loss;
- expose firmware flags/counters.

## Loss concealment

The host may conceal short output discontinuities for listening, but diagnostics must still report them.

Initial concealment policy may hold/interpolate a bounded small gap. Large gaps create silence/reset state rather than manufacturing long stretches of plausible signal.

Never rewrite diagnostic counters to match concealed output.

## DSP

Default source-domain processing:

- remove DC/gravity with a mild high-pass / DC blocker;
- configurable linear gain;
- bounded clipping or a documented soft limiter;
- no aggressive denoise or psychoacoustic processing.

Raw/WAV diagnostic capture should permit recording either pre-DSP or post-DSP data.

## Resampling

Output rate: 48,000 Hz mono float32.

The source bandwidth is below 2 kHz even at the highest QMI8658C mode, so a stateful linear interpolator is acceptable for the initial implementation if it is continuous across BLE packets and tested. A higher-quality resampler may be added later without changing BLE protocol semantics.

Source rate comes from session/frame metadata, not a constant 8 kHz assumption.

## Audio sinks

Minimum sinks:

- live `sounddevice` output to a selected device;
- WAV recording.

The project does not implement an OS kernel microphone driver. For applications requiring a microphone endpoint, route the receiver output to an installed virtual audio cable and select that cable's recording endpoint in the application.

## CLI

Minimum useful commands/options:

- list BLE devices / connect by name or address;
- list host audio devices;
- choose output device;
- choose source axis/range/rate policy;
- set gain/high-pass;
- live monitor;
- record WAV;
- diagnostics-only mode.

The CLI must print source rate, selected sensor mode, effective MTU if known, packet loss, source gaps, firmware I2C/ring errors and host audio underruns.

## Buffering and latency

Use bounded buffers.

Do not accumulate unbounded latency when the audio sink falls behind. Prefer dropping old buffered output with a diagnostic over allowing seconds of lag.

A later hardware qualification should report approximate steady-state latency and jitter; CI should stress bounded-buffer behavior deterministically.
