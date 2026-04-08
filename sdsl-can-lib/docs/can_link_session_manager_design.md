# can_link Session Manager Design

This document describes the proposed redesign of `can_link` to support
bidirectional, peer-to-peer CAN communication where either board can start
transmitting at any time.

The implementation target is [src/can_link.c](/C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib/src/can_link.c).

## Problem Statement

The current `can_link` implementation is built around a singleton-style
transport model:

- one global unicast send context
- one global broadcast send context
- one global unicast receive context
- one global broadcast receive context
- background receive workers based on `isotp_bind()`

That model works for:

- single-frame traffic
- one-way multi-frame traffic
- tightly controlled test cases

It does not reliably support this target behavior:

- two boards connected to the same CAN bus
- either board can transmit at any time
- neither board has a fixed sender or receiver role
- both boards may initiate multi-frame ISO-TP transfers independently

## Root Cause

For unicast fixed addressing, incoming application payload traffic and incoming
flow-control traffic can overlap in the current design.

The current implementation relies on Zephyr ISO-TP contexts created through
`isotp_bind()` and outbound sends created through `isotp_send()`.

The key issue is:

- outbound multi-frame sends need returned flow-control (`FC`) frames
- background unicast receive workers also consume traffic from the same peer
- the generic receive path can consume frames needed by the active send session

This leads to unreliable duplex multi-frame behavior.

## Design Goal

Introduce a session manager that owns transport state explicitly and supports:

- bidirectional peer-to-peer communication
- inbound and outbound sessions per peer
- dynamic session creation when new traffic is detected
- correct routing of incoming ISO-TP frames
- safe handling of simultaneous or overlapping traffic

## High-Level Design

`can_link` should move from a context-per-role model to a manager-owned
session model.

The manager should:

- install raw CAN receive filters once
- inspect incoming CAN frames at the transport layer
- classify ISO-TP PCI type
- create and manage per-peer sessions
- route frames to the correct session
- handle inbound reassembly
- handle outbound segmentation and flow-control processing
- deliver fully reassembled payloads to the application callback

## Why A Manager Is Needed

The current design delegates most ISO-TP ownership to Zephyr's internal
`isotp_bind()` and `isotp_send()` state machines.

That is not sufficient for true random duplex multi-frame traffic because:

- frame routing decisions need to consider more than CAN ID alone
- traffic from a peer may represent:
  - a new single frame
  - a first frame starting a new inbound transfer
  - a consecutive frame continuing an inbound transfer
  - a flow-control frame for an active outbound transfer
- with fixed addressing, those distinctions must be made by inspecting frame
  payload PCI type and session state

The manager therefore needs to operate below the current `isotp_bind()` receive
worker model.

## Proposed Architecture

### Manager Object

The library should own a single manager instance with shared transport state.

Suggested structure:

```c
struct can_link_manager {
    const struct device *can_dev;
    struct k_mutex lock;

    can_link_rx_handler_t app_rx_cb;
    void *app_rx_user_data;

    struct can_link_session sessions[CONFIG_CAN_LINK_MAX_SESSIONS];

    int unicast_filter_id;
    int broadcast_filter_id;

    struct k_work manager_work;
};
```

Responsibilities:

- install and remove CAN filters
- serialize session table access
- dispatch inbound frames
- manage timeouts and cleanup
- invoke application callbacks when complete payloads are available

### Session Object

The manager should create explicit peer sessions rather than relying on a
single global send or receive context.

Suggested structure:

```c
enum can_link_session_dir {
    CAN_LINK_SESSION_RX,
    CAN_LINK_SESSION_TX,
};

enum can_link_session_state {
    CAN_LINK_SESSION_IDLE,
    CAN_LINK_SESSION_WAIT_CF,
    CAN_LINK_SESSION_WAIT_FC,
    CAN_LINK_SESSION_SENDING_CF,
    CAN_LINK_SESSION_COMPLETE,
    CAN_LINK_SESSION_ERROR,
};

struct can_link_session {
    bool in_use;
    uint8_t peer_node;
    bool is_broadcast;
    enum can_link_session_dir dir;
    enum can_link_session_state state;

    uint8_t priority;
    uint8_t next_sn;
    uint8_t block_size;
    uint8_t block_count;
    uint8_t stmin;

    uint32_t total_len;
    uint32_t transferred;

    int64_t last_activity_ms;

    uint8_t buffer[CONFIG_CAN_LINK_SESSION_BUF_SIZE];
};
```

Responsibilities:

- track per-peer inbound or outbound transfer state
- store reassembly or segmentation progress
- track sequence numbers and flow-control state
- provide timeout and cleanup metadata

## Receive Path

### Raw CAN Filter Installation

Instead of binding long-lived ISO-TP receive contexts with `isotp_bind()`,
the manager should install raw CAN receive filters once at initialization.

The manager should receive:

- physical frames addressed to the local node
- optional functional or broadcast traffic if needed

### Frame Classification

Each inbound frame should be classified by:

- source node
- target node
- broadcast vs unicast
- ISO-TP PCI type

Relevant PCI types:

- `SF` single frame
- `FF` first frame
- `CF` consecutive frame
- `FC` flow control

### Dispatch Rules

When a frame arrives from peer `P`, the manager should decide:

- if it is `FC`, route it to an active outbound session for peer `P`
- if it is `FF`, create or update an inbound receive session for peer `P`
- if it is `CF`, continue an existing inbound receive session for peer `P`
- if it is `SF`, deliver it immediately as a complete payload

This routing must happen before any generic application receive path consumes
the frame.

## Transmit Path

### Session Creation

When the application calls `can_link_send_to()`:

- locate or allocate a TX session for the peer
- reject or serialize if policy disallows overlap
- encode and segment the payload according to ISO-TP rules

### Single Frame

If payload length is within single-frame size:

- emit one frame
- mark the session complete

### Multi-Frame

If payload length exceeds single-frame size:

- send `FF`
- move the session to `WAIT_FC`
- when `FC CTS` arrives, start sending `CF`
- obey `BS` and `STmin`
- complete when all bytes are sent

## Inbound Reassembly

For inbound multi-frame traffic:

- `FF` allocates or resets an RX session
- session records expected total payload length
- manager sends `FC CTS`
- each `CF` updates transfer progress and validates sequence number
- on completion, assembled payload is delivered to application callback

## Timeout Handling

The manager must own timeout logic explicitly.

Timeouts should cover:

- waiting for first `FC` after `FF`
- waiting for additional `FC` after block completion
- waiting for next `CF`
- idle session expiration

When a timeout occurs:

- mark the session as error
- release resources
- optionally report transport error via logs or callback

## Concurrency Policy

The redesign must define how simultaneous traffic is handled.

Recommended initial policy:

- allow one active outbound unicast session per peer
- allow one active inbound unicast session per peer
- allow outbound and inbound sessions to coexist for the same peer
- reject additional overlapping outbound sends to the same peer with `-EBUSY`

This gives deterministic behavior while still allowing full duplex.

## Broadcast Policy

Broadcast traffic should remain separate from unicast session handling.

Recommended initial behavior:

- keep broadcast as single-frame only
- do not mix broadcast into the duplex multi-frame session state machine

This keeps the unicast session manager focused and reduces complexity.

## Public API Impact

The existing public API can remain mostly unchanged:

- `can_link_init()`
- `can_link_send()`
- `can_link_send_to()`
- `can_link_send_broadcast()`
- `can_link_node_id()`

The main change is internal:

- `can_link_send_to()` becomes a session-creation or enqueue operation
- receive processing becomes manager-owned instead of worker-owned

## Configuration Additions

The session manager should introduce explicit sizing controls:

```kconfig
config CAN_LINK_MAX_SESSIONS
    int "Maximum active can_link sessions"
    default 4

config CAN_LINK_SESSION_BUF_SIZE
    int "Per-session payload buffer size"
    default 256

config CAN_LINK_MANAGER_STACK_SIZE
    int "Session manager stack size"
    default 2048
```

Additional timeout tuning may also be useful later.

## Code Areas To Replace

The following parts of the current implementation should be redesigned or
removed:

- global `send_ctx`
- global `recv_ctx`
- `rx_worker`
- `rx_thread()`
- long-lived unicast receive workers created in `can_link_init()`
- direct `isotp_send()` ownership in `can_link_send_to()`

## Suggested Function Breakdown

The rewrite should be split into small transport-specific helpers.

Suggested internal functions:

- `session_alloc()`
- `session_release()`
- `session_find_tx()`
- `session_find_rx()`
- `manager_handle_sf()`
- `manager_handle_ff()`
- `manager_handle_cf()`
- `manager_handle_fc()`
- `manager_send_fc()`
- `manager_send_sf()`
- `manager_send_ff()`
- `manager_send_next_cf()`
- `manager_check_timeouts()`

This structure will make the implementation easier to test and reason about.

## Recommended Implementation Order

Implement in phases:

1. Add manager object, session table, and raw CAN RX callback.
2. Support outbound and inbound single-frame traffic.
3. Add inbound multi-frame reassembly and `FC` generation.
4. Add outbound multi-frame segmentation and `FC` handling.
5. Add timeout handling and cleanup.
6. Add a duplex random-traffic integration test.

## Non-Goals For The First Iteration

The initial session manager should not try to solve everything at once.

Keep these out of scope initially:

- multiple simultaneous outbound sessions to the same peer
- broadcast multi-frame transport
- more than one active inbound transfer per peer
- advanced fairness or scheduling across many peers

## Expected Outcome

If this design is implemented, `can_link` should be able to support:

- two boards on the same bus
- either board initiating communication at any time
- random independent send timing
- reliable duplex single-frame traffic
- a path toward reliable duplex multi-frame traffic

This redesign is larger than a small bug fix. It is a transport ownership
change intended to make `can_link` behave as a real peer-to-peer communication
layer rather than a singleton wrapper around static ISO-TP contexts.
