# can_link Current Design

This document describes the current `can_link` implementation in
[src/can_link.c](/C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib/src/can_link.c).
It is meant to explain how the library works today, what assumptions it makes,
and where its limits still are.

## Purpose

`can_link` is a small Zephyr-native transport wrapper for CAN + ISO-TP.

The public API is intentionally narrow:

- `can_link_init()`
- `can_link_send()`
- `can_link_send_to()`
- `can_link_send_broadcast()`
- `can_link_node_id()`

The implementation chooses to keep transport ownership inside Zephyr's
`isotp_bind()`, `isotp_recv()`, and `isotp_send()` state machines rather than
building a custom ISO-TP session manager in the library.

That design keeps the code smaller and easier to reason about, but it also
means `can_link` is built around a singleton-style transport model rather than
multiple independent link instances.

## High-Level Model

At runtime, the library owns one global transport instance with these major
pieces of state:

- one global link configuration with `node_id`, `peer_id`, and `loopback`
- one precomputed unicast address set
- one precomputed broadcast address set
- one long-lived unicast receive worker
- one long-lived broadcast receive worker
- one shared asynchronous transmit slot for outbound sends

There is no per-peer session table and there is no support for multiple
simultaneous outbound transfers. The library is optimized for a small embedded
system where one logical node talks to one default peer and may also emit
functional broadcast traffic.

## Addressing And Routing

The current design uses ISO-TP fixed addressing over 29-bit CAN identifiers.

The important routing decision is not just "use fixed addressing", but "use
different CAN-ID classes for different traffic roles". The library separates:

- point-to-point data traffic
- point-to-point flow-control traffic
- functional or broadcast traffic

It does that with three base CAN identifiers:

- `LIB_DATA_CAN_ID_BASE = 0x18CE0000`
- `LIB_FC_CAN_ID_BASE = 0x18CF0000`
- `LIB_FUNCTIONAL_CAN_ID_BASE = 0x18DB0000`

The low byte of the 29-bit identifier carries the source node, and the next
byte carries the target node. In other words, the library packs:

- bits `15:8` as target node
- bits `7:0` as source node

This is done in `build_can_id()`.

The result is a routing tuple that looks like:

- traffic class in the upper CAN-ID bits
- target node in the next byte
- source node in the low byte

That lets the CAN filter layer distinguish DATA from FC before Zephyr does any
ISO-TP processing, which is the key improvement over the older shared-CAN-ID
approach.

## Why Fixed Addressing Was Chosen

The current implementation uses:

- `ISOTP_MSG_IDE`
- `ISOTP_MSG_FIXED_ADDR`

and does not use ISO-TP extended addressing.

That choice keeps the address information in the CAN identifier instead of
consuming one payload byte in every ISO-TP frame. A practical consequence is
that classical-CAN single-frame payloads can still use up to `7` bytes instead
of dropping to `6`.

## Precomputed Address Sets

During initialization, `configure_isotp_ids()` builds the library's full set of
ISO-TP message IDs once and stores them in `link_addr_cfg`.

For unicast traffic, the library keeps:

- `tx_addr`: outbound data path to the peer
- `rx_fc_addr`: returned flow-control path for an active unicast send
- `bind_rx_addr`: long-lived receive binding for inbound unicast payloads
- `bind_tx_fc_addr`: flow-control transmit address used by the unicast RX bind

For broadcast traffic, the library keeps:

- `broadcast_tx_addr`: outbound functional transmit path
- `broadcast_bind_rx_addr`: long-lived receive binding for inbound functional frames
- `broadcast_bind_tx_addr`: companion TX address used by the broadcast bind

These IDs are derived from:

- `CONFIG_CAN_LINK_NODE_ADDR`
- `CONFIG_CAN_LINK_PEER_NODE_ADDR`
- `CONFIG_CAN_LINK_LOOPBACK`

Loopback mode simply changes the effective peer so the node routes traffic back
to itself through the local CAN controller.

## Receive Path

The receive path is built around two background threads:

- one unicast worker
- one broadcast worker

Each worker owns a Zephyr `isotp_recv_ctx` and calls:

1. `isotp_bind()` once during startup
2. `isotp_recv()` repeatedly forever

The worker model is intentionally simple:

- Zephyr owns frame filtering, segmentation handling, reassembly, and FC generation
- `can_link` only waits for completed payloads
- completed payloads are delivered to the application callback

When a worker receives a full payload, it calls the user-supplied
`can_link_rx_handler_t` with:

- the payload buffer
- payload length
- source node extracted from the matched CAN ID
- a boolean telling the application whether this came from the broadcast path
- the caller's `user_data`

The source node is recovered from the low byte of the bound receive address
stored in the Zephyr receive context.

## Transmit Path

The transmit path is intentionally serialized.

`can_link` owns one `async_tx_state` object and allows only one outbound ISO-TP
transfer to be active at a time. That state contains:

- a mutex
- one Zephyr `isotp_send_ctx`
- a persistent transmit buffer
- the per-send TX and FC address pair
- flags describing the in-flight transfer

This means the library can reject a new outbound send with `-EBUSY` if another
send is already in progress.

### Outbound Send Flow

`can_link_send()` forwards to `can_link_send_to()` using the configured default
peer.

`can_link_send_to()`:

- checks initialization state
- validates the target node and payload
- calls `start_async_send()`

`can_link_send_broadcast()`:

- checks initialization state
- validates the payload
- enforces the single-frame-only broadcast policy
- calls `start_async_send()` with the broadcast route

Inside `start_async_send()` the library:

1. validates the payload size against the internal TX buffer
2. decodes the protobuf payload to read its requested priority
3. acquires the TX mutex
4. rejects the send if another one is already active
5. copies caller-owned bytes into persistent internal storage
6. builds the correct TX and RX-FC address pair for this send
7. starts Zephyr's asynchronous `isotp_send()`

The completion callback `tx_complete_cb()` clears the in-flight flag and logs
the final ISO-TP result.

## Priority Handling

The code decodes a `LinkMessage` protobuf in `get_priority_from_pb()` and clamps
the declared priority to the 3-bit CAN range `0..7`.

At the moment, that decoded priority is informational only. The value is parsed
before each send, but it is not yet inserted into the transmitted CAN-ID
priority field. The current address builder uses fixed base identifiers and
keeps the transport routing stable.

That means the design already has a hook for priority-aware routing policy, but
the actual transmitted identifiers are still using fixed library defaults.

## Broadcast Policy

Broadcast traffic is deliberately simpler than unicast traffic.

Current rules:

- broadcast uses the functional CAN-ID base
- broadcast uses the dedicated background RX bind
- broadcast send is limited to one CAN frame
- `can_link_send_broadcast()` rejects payloads longer than `7` bytes

The implementation still supplies an FC receive identifier to satisfy the
Zephyr `isotp_send()` API, even though normal functional traffic does not rely
on returned flow control.

## Initialization Sequence

`can_link_init()` performs these steps:

1. return immediately if the library is already initialized
2. verify the chosen CAN device is ready
3. precompute the library address map
4. initialize the TX mutex
5. select normal or loopback mode
6. start the CAN controller
7. store the application receive callback and user data
8. create the unicast RX thread if enabled
9. create the broadcast RX thread if enabled
10. mark the library initialized

The CAN device is obtained once from:

- `DT_CHOSEN(zephyr_canbus)`

This means the consuming application must provide a valid Zephyr CAN bus chosen
node and associated hardware configuration.

## Configuration Knobs

The implementation is controlled mainly by build-time configuration:

- `CONFIG_CAN_LINK_NODE_ADDR`
- `CONFIG_CAN_LINK_PEER_NODE_ADDR`
- `CONFIG_CAN_LINK_LOOPBACK`
- `CONFIG_CAN_LINK_DISABLE_UNICAST_RX`
- `CONFIG_CAN_LINK_DISABLE_BROADCAST_RX`

The disable switches are useful for test shaping and bring-up because they let
you remove one or both long-lived receive paths without changing the transport
API.

## Current Strengths

The current design works well for the following cases:

- simple one-peer deployments
- normal point-to-point application traffic
- functional broadcast traffic with a strict single-frame limit
- Zephyr-managed ISO-TP segmentation and reassembly
- duplex traffic patterns where the current address split is sufficient

Compared with the earlier shared-CAN-ID layout, the present design has a much
clearer routing story because DATA and FC are separated at the CAN filter
level.

## Current Limits

The current implementation still has important architectural limits:

- it is singleton-style rather than instance-based
- it supports only one active outbound transfer at a time
- it has no per-peer session table
- it relies on Zephyr's internal ISO-TP ownership model rather than library-owned transport state
- it restricts functional broadcast to single-frame payloads

Those limits are acceptable for the current use case, but they should be kept
in mind before extending the library toward larger multi-peer or high-traffic
systems.

## Mental Model For Maintenance

When changing `can_link.c`, it helps to think of the design in three layers:

1. Address construction
   This layer decides which CAN IDs and ISO-TP flags identify each traffic path.

2. Long-lived receive ownership
   This layer binds stable receive routes once and turns complete ISO-TP payloads
   into application callbacks.

3. One-shot transmit ownership
   This layer builds a send-specific TX and FC route pair, copies payload data
   into durable storage, and lets Zephyr finish the asynchronous transfer.

If a future bug appears, the first debugging question should usually be:

- is this an address-routing problem
- a long-lived RX binding problem
- or an in-flight TX state problem

That framing tends to match the actual structure of the current code.
