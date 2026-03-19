# can_link Current Design Notes

This document describes the current `can_link` behavior and the constraints
that matter for the test suite.

## Overview

`can_link` is currently a singleton-style transport wrapper around Zephyr
CAN ISO-TP.

Key properties:

- one global link configuration (`node_id`, `peer_id`, `loopback`)
- one outgoing unicast send context
- one outgoing broadcast send context
- one background unicast receive worker
- one background broadcast receive worker

The implementation lives in [src/can_link.c](/C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib/src/can_link.c).

## Addressing Model

The library uses ISO-TP fixed addressing over 29-bit CAN identifiers.

It maintains:

- a unicast transmit address
- a unicast flow-control receive address
- a unicast receive bind address
- a unicast flow-control transmit address
- separate functional/broadcast addresses

Outgoing unicast traffic uses the message priority decoded from the protobuf
payload. Flow-control traffic is kept at priority `0`.

## Current Receive Model

`can_link_init()` starts background receive workers that call `isotp_bind()`
for:

- unicast RX
- broadcast RX

Those workers are useful for normal application-level reception, but they also
mean the library installs broad receive filters on the CAN device.

## Current Multi-Frame Limitation

In the current design, the sender's general receive worker can interfere with
the sender-side ISO-TP flow-control receive filter during multi-frame unicast.

Observed symptom:

- sender logs `Reception of next FC has timed out`
- receiver logs `Timeout while waiting for CF`
- sender may log `Got unexpected frame. Ignore`

What this means in practice:

- the receiver got the first frame
- the receiver generated flow-control
- the sender's dedicated FC filter did not get the FC frame cleanly

For the current working multi-frame test setup, the sender disables the
background RX workers so only the send-side FC receive filter is active.

## Current Test Strategy

The repository now uses two complementary tests:

### `eps_single_frame_test`

Purpose:

- verify the basic two-board CAN path with a payload that always fits in one
  ISO-TP single frame

Why it exists:

- no flow-control is needed
- good baseline smoke test

### `eps_message_test`

Purpose:

- verify one-way two-board multi-frame unicast transport

Why it is one-way:

- avoids overlapping ISO-TP sessions
- works with the current singleton receive model

Why sender RX is disabled:

- prevents the sender's general RX worker from competing with the send-side
  flow-control receive filter

## Current Configuration Switches

The current implementation supports these test-oriented compile-time switches:

- `CONFIG_CAN_LINK_DISABLE_UNICAST_RX`
- `CONFIG_CAN_LINK_DISABLE_BROADCAST_RX`

These are currently used by `eps_message_test` to make multi-frame unicast
reliable in the present design.

## Future Improvement Areas

Likely next steps if `can_link` needs to support richer traffic patterns:

- separate application RX more cleanly from transport-level FC handling
- support safe bidirectional multi-frame traffic with background RX enabled
- support multiple active peers or instance-based link state
- improve test coverage for multi-priority unicast traffic
- document expected CAN ID mappings more explicitly
