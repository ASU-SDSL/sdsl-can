# SDSL-CAN

SDSL-CAN is a Zephyr-based workspace for developing and testing the
`sdsl-can-lib` CAN transport module.

The main library in this repository provides a small `can_link` API for
CAN + ISO-TP communication on embedded targets. The current implementation is
Zephyr-native, uses fixed addressing over 29-bit CAN identifiers, and is set up
for practical bring-up on STM32 Nucleo hardware.

## What Is In This Repository

This repository is not just a standalone library dump. It is a working
development workspace that includes:

- `sdsl-can-lib/`
  The Zephyr module that exposes the `can_link` API.
- `sdsl-can-lib/tests/`
  Small board-level test applications used to validate loopback, single-frame,
  one-way multi-frame, and duplex multi-frame behavior.
- `zephyr/`
  A checked-out Zephyr tree used by this workspace.
- `modules/`
  Zephyr module dependencies fetched through `west`.
- `bootloader/`, `tools/`, and workspace support files
  Project-specific supporting content.

If you are here for the transport library itself, start in
[sdsl-can-lib](C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib).

## The Main Module

`sdsl-can-lib` exposes this public API:

- `can_link_init()`
- `can_link_send()`
- `can_link_send_to()`
- `can_link_send_broadcast()`
- `can_link_node_id()`

The public header is:

- [include/can_link.h](C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib/include/can_link.h)

The current implementation lives in:

- [src/can_link.c](C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib/src/can_link.c)

The current architecture is documented in:

- [docs/can_link_design.md](C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib/docs/can_link_design.md)

## Current Design Summary

Today, `can_link` is a singleton-style wrapper around Zephyr ISO-TP:

- long-lived unicast and broadcast RX paths are created with `isotp_bind()`
- outbound transfers use asynchronous `isotp_send()`
- routing uses fixed addressing with distinct CAN-ID classes for DATA, FC, and
  functional traffic
- one outbound transfer is allowed at a time

This design is intentionally small and practical for bring-up, debugging, and
two-board integration testing.

## Test Applications

The repository includes several test apps under
[sdsl-can-lib/tests](C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib/tests):

- `eps_single_frame_test`
  Basic two-board single-frame validation.
- `eps_message_test`
  One-way multi-frame messaging test.
- `eps_message_zephyr_isotp_test`
  Zephyr-native ISO-TP unicast test using the current `can_link.c`.
- `eps_message_zephyr_isotp_duplex_test`
  Two-board duplex multi-frame test using the current `can_link.c`.
- `eps_message_loopback_test`
  Local loopback-oriented transport test.
- `led_test`
  Small CAN/Zephyr bring-up test.

If you want to validate the current architecture, the duplex test is the best
high-value integration target:

- [eps_message_zephyr_isotp_duplex_test README](C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib/tests/eps_message_zephyr_isotp_duplex_test/README.md)

## Workspace Setup

This repository contains a module-local `west.yml` in
[sdsl-can-lib/west.yml](C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib/west.yml)
that pins the Zephyr revision and imports the module dependencies needed by the
library.

A typical fresh setup looks like:

```powershell
west init -l sdsl-can-lib
west update
west zephyr-export
```

If you already have this workspace checked out with `zephyr/` and `modules/`
present, you can go straight to building tests.

## Building A Test

Example: build the duplex ISO-TP test for board A on an STM32 Nucleo F103RB:

```powershell
west build -p always -b nucleo_f103rb sdsl-can-lib\tests\eps_message_zephyr_isotp_duplex_test -- "-DCONF_FILE=prj.conf;node_a.conf"
```

Build board B with:

```powershell
west build -p always -b nucleo_f103rb sdsl-can-lib\tests\eps_message_zephyr_isotp_duplex_test -- "-DCONF_FILE=prj.conf;node_b.conf"
```

Then flash each board with:

```powershell
west flash
```

More test-specific build instructions live in each test folder's README.

## Hardware Notes

The current test setup has been aimed primarily at STM32 Nucleo boards using
Zephyr CAN support and STM32CubeProgrammer-based flashing.

A few practical notes:

- the board must expose a working ST-Link debug interface for `west flash`
- the CAN transceiver and board overlay must match the target hardware wiring
- two-board ISO-TP tests should be flashed one board at a time

## Using The Module In Another App

If you want to consume `sdsl-can-lib` from another Zephyr application, see the
module README:

- [sdsl-can-lib/README.md](C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib/README.md)

That document covers:

- adding the module to another workspace
- using `ZEPHYR_EXTRA_MODULES`
- enabling `CONFIG_SDSL_CAN`
- the minimal consumer app shape

## Repository Status

This repository is currently oriented toward active embedded transport
development rather than polished package release flow. The docs and tests are
meant to make the current behavior explicit and reproducible while the design
continues to evolve.

If you are trying to understand "what is the real implementation today", the
best path is:

1. Read [sdsl-can-lib/README.md](C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib/README.md)
2. Read [docs/can_link_design.md](C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib/docs/can_link_design.md)
3. Run one of the tests under [sdsl-can-lib/tests](C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib/tests)
