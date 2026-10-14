# ISO-TP payload size test

Hardware test using the production `can_link` wrapper at 50 kbit/s on Nucleo
F103RB PA12/TX and PA11/RX. By default node 1 transmits and node 2 receives;
the optional duplex profile makes both nodes transmit and receive.

The sender cycles through **32, 64, and 128 encoded bytes**, one message every
100 ms. These are total Protobuf lengths, including metadata, not just the
pattern field. A test-only schema retains the existing LinkMessage field numbers
and adds a bounded byte pattern; the production schema and transport are unchanged.

The receiver decodes Protobuf, validates routing and the expected size for each
sequence, regenerates the expected encoding, and compares every byte. It reports
sequence gaps and duplicate/out-of-order messages after the first observed message.
The first sequence may be arbitrary to permit starting the receiver after the sender.
Both nodes periodically sample CAN state and error counters. Successful
TX setup alone is not a pass: require completion logs and matching RX PASS records.

Build from the squidsat-dant workspace root on macOS:

```bash
export PATH="$PWD/.venv/bin:/opt/homebrew/bin:$PATH"
export CCACHE_DIR=/tmp/ccache-dant-isotp
west build -p always -b nucleo_f103rb \
  sdsl-can/sdsl-can-lib/tests/payload_size_test \
  -d /tmp/dant-isotp-size-a -- \
  -DZEPHYR_EXTRA_MODULES="$PWD/sdsl-can/sdsl-can-lib" \
  '-DCONF_FILE=prj.conf;node_a.conf'
west build -p always -b nucleo_f103rb \
  sdsl-can/sdsl-can-lib/tests/payload_size_test \
  -d /tmp/dant-isotp-size-b -- \
  -DZEPHYR_EXTRA_MODULES="$PWD/sdsl-can/sdsl-can-lib" \
  '-DCONF_FILE=prj.conf;node_b.conf'
```

Flash the receiver first, explicitly selecting each board's ST-LINK serial.
Monitor both consoles at 115200 baud. Match TX START and RX PASS by sequence
and length, and check ISO-TP completion logs. Require at least 100 messages of
**each size** in a common capture interval, no missing/duplicate sequences, no
failure logs, and healthy CAN state samples with zero TX/RX error counters.

For simultaneous bidirectional traffic, add `duplex.conf` to each build's
`CONF_FILE` (for example `prj.conf;node_a.conf;duplex.conf`) and use separate
build directories `/tmp/dant-isotp-duplex-a` and `/tmp/dant-isotp-duplex-b`.
Node A sends every 150 ms and B every 157 ms, varying the relative phase to
exercise overlapping transfers. Require the same acceptance criteria separately
in each direction and confirm RX PASS events occur between local TX START and
TX completion logs. The default one-way mode remains available.

These tests do not establish broadcast, 500 kbit/s, long-duration, fault recovery,
or radiation performance.

## Hardware result — 2026-10-14

Both original Nucleo F103RB boards were flashed and verified, then reset using
ST-LINK. Board A serial `066DFF485753667187234846` sent to Board B serial
`0671FF485753667187232019` at 50 kbit/s. The default four 56-byte ISO-TP receive
buffers were retained.

A 40-second serial capture passed with the following matched messages in the
common observation interval (sequences 90 through 469):

| Encoded bytes | Matched and byte-validated messages |
| --- | --- |
| 32 | 127 |
| 64 | 126 |
| 128 | 127 |

There were no missing, duplicate, or mismatched sequences in that interval and
no logged failures. Both boards reported 13 healthy CAN state samples with zero
TX/RX error counters. Each console captured 381 transfers; the slightly different
capture boundaries leave 380 sequences common to both logs. This is a short
integration check, not an endurance qualification.

## Bidirectional hardware result — 2026-10-14

Both boards were rebuilt with `duplex.conf`, flashed, verified, and reset.
A 60-second capture passed at 50 kbit/s using the unchanged four 56-byte
receive buffers per board:

| Encoded bytes | A to B matched | B to A matched |
| --- | --- | --- |
| 32 | 129 | 124 |
| 64 | 128 | 123 |
| 128 | 129 | 124 |

The common sequence intervals were A to B: 3009–3394 and B to A: 2907–3277.
All matched messages passed the byte comparison and Protobuf checks, with no
missing or duplicate sequences or logged errors in the capture. A logged 387
TX completions and B logged 371; capture boundaries excluded one A transfer
from the common comparison interval. Each board reported 13 healthy CAN state
samples with zero TX/RX error counters.

The logs contained 110 receptions on A and 94 on B between a local TX START
and its completion, demonstrating overlapping inbound/outbound transfers.
This validates the tested two-node workload, not arbitrary concurrent sessions
from several peers, endurance, or fault recovery.
