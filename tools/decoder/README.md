# TAP capture decoder

`tigo_tap_decoder_v2.py` decodes supported local capture formats. Captures are
intentionally excluded from the public repository because they can contain
device identifiers, network material, and household telemetry:

- original timestamp/byte-count listener logs;
- `RAW t=...` and `FRAME t=...` serial logs;
- plain and gzip-compressed logs;
- zipped MQTT JSONL captures;
- normal frame and `interesting_frame` support-logger JSONL records.

It validates the TAP CRC, decodes the `0x0148/0x0149` packet queue, power,
topology and diagnostic packets, and the active `0x0B0F/0x0B10` command
families used for radio profiles, node tables and learning.

Decode one capture:

```console
python tools/decoder/tigo_tap_decoder_v2.py /path/to/local/capture.log \
  --json-out decoded.json
```

Build the repository-wide compact index:

```console
python tools/decoder/analyze_tap_corpus.py /path/to/local/captures \
  --json-out /path/to/local/tap-corpus-index.json
```

Run the decoder regression suite:

```console
python -m unittest discover -s tools/decoder -p "test_*.py" -v
```

The indexer scans every candidate file and retains file-level results. For its
session-level aggregate it groups logger mirrors and chooses the richest valid
mirror, avoiding duplicate counts from `.raw`, `.frames`, `.interesting`, and
status artifacts from one physical capture. Session keys include their relative
directory, so unrelated same-named captures cannot collide. Mixed RAW/FRAME logs
are merged by timestamp and exact frame identity, including FRAME-only gaps.

Declared byte and payload lengths are validated. Malformed/interleaved lines are
reported under `parse_warnings` instead of silently accepted, and malformed
`0x31` packets are excluded from power counts. The index retains compact
`node_table_state_changes`, not only the final page, so pending-to-confirmed
transitions remain reproducible.

## Differential trace analysis

Compare a working CCA capture with a failing ESP capture:

```console
python tools/decoder/tigo_trace_diff.py \
  /path/to/local/reference.raw.log \
  /path/to/local/candidate.serial.log \
  --output-json trace-diff.json --output-markdown trace-diff.md
```

The analyzer normalizes dynamic TAP identities, gateway IDs, RF node addresses,
DSNs, packet cursors, and timestamps. It pairs requests and responses, retains
latencies and interframe timing, distinguishes empty TAP-local acknowledgements
from RF responses with a body, and keeps unknown or malformed `0x0149` packets
visible. The current chronological alignment uses `SequenceMatcher`; it is a
sequence-alignment heuristic rather than numerical Dynamic Time Warping. Firmware
`2026.08.04.27` and newer publishes both host TX and TAP RX frames on `raw/frame`;
older MQTT captures may contain responses only and cannot reconstruct complete
transactions by themselves. A request within five seconds of the final captured
frame is reported as `capture_end_pending` and excluded from outcome divergence,
so a live or abruptly stopped file does not create a false protocol failure.
Firmware `2026.08.04.28` additionally includes boot-local `device_ms` on both
directions. The analyzer anchors that clock to wall time after every ESP reboot
and uses it for bus latency, avoiding false multi-second delays from MQTT delivery.
Firmware `2026.08.04.29` also defers raw-frame MQTT publication until no TAP
reply is outstanding. This prevents the diagnostic stream and concurrent HTTP
status requests from delaying UART processing, while retaining the original
device timestamps and TX/RX order.

## Response-aware replay

Build a replay plan through the first electrically released sample:

```console
python tools/decoder/tigo_trace_replay.py \
  --trace /path/to/local/reference.raw.log \
  --output replay-plan.json
```

Inspect the plan before executing it. A read-only subset can be run with the ESP
scheduler like this:

```console
python tools/decoder/tigo_trace_replay.py --plan replay-plan.json \
  --blocks 2 --execute --transport esp \
  --base-url http://opentaptox-esp32c6.local
```

State-changing and active-RF steps are blocked unless the matching
`--allow-state-changing` or `--allow-active-rf` option is supplied. Unknown
semantics fail closed. The ESP scheduler inserts live gateway, DSN, cursor, and
node values, checks expected responses and baseline identity/table/profile
invariants, and aborts on a divergent state. RSD and address changes additionally
require operation-specific HTTP POST confirmations. Legacy per-step HTTP replay
rejects `0x0014` and `0x003C` entirely because it cannot guarantee an atomic
address verification; use the journaled ESP scheduler for those frames.

`ddmin()` is available for tested block minimization, and `--blocks` / `--steps`
support controlled manual delta debugging. Fully automatic physical reset and
repeated hardware ddmin are not yet unattended. Always judge release from fresh
electrical voltage/power data; a TAP acknowledgement is not sufficient.
