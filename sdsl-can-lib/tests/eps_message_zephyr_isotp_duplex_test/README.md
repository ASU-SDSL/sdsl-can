# EPS Message Zephyr ISO-TP Duplex Test

This test is a two-board duplex variant of
[eps_message_zephyr_isotp_test](/C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib/tests/eps_message_zephyr_isotp_test).

It builds the updated [can_link.c](/C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib/src/can_link.c),
which keeps transport ownership inside Zephyr:

- unicast RX uses `isotp_bind()` + `isotp_recv()`
- TX uses asynchronous `isotp_send()`
- ISO-TP extended addressing (`ext_addr`) is used together with distinct CAN IDs for data and FC traffic

## Goal

Have both boards:

- send multi-frame unicast messages at their own timing
- receive the peer's messages correctly

This is the right stress test for the current architecture, because both boards
will have a long-lived unicast RX bind and will also start outbound ISO-TP sends.

## Addressing Notes

The current transport uses ISO-TP extended addressing with distinct CAN-ID
bases plus separate `ext_addr` values for:

- normal payload data
- returned flow control
- broadcast data

That gives the CAN filter layer a stronger routing key than the older
shared-CAN-ID design.

## Current Expectation

This test is more likely to work than the earlier shared-CAN-ID version, but it
is still a stress test for the architecture.

Why:

- the design now separates data and FC by CAN ID base, which is a real routing
  improvement at the filter layer
- Zephyr owns the RX bind filter for `isotp_bind()`
- Zephyr also owns the internal FC filter for `isotp_send()`
- if duplex traffic still fails, the remaining issue is no longer "shared CAN ID
  between data and FC" but the broader interaction between long-lived RX binds
  and sender-owned ISO-TP contexts under load

If the test fails with FC timeouts or missing RX, that is evidence of a deeper
architectural limitation rather than just a bad test.

## Build And Flash

Build board A:

```powershell
west build -p always -b nucleo_f103rb sdsl-can-lib\tests\eps_message_zephyr_isotp_duplex_test -- "-DCONF_FILE=prj.conf;node_a.conf"
west flash
```

Build board B:

```powershell
west build -p always -b nucleo_f103rb sdsl-can-lib\tests\eps_message_zephyr_isotp_duplex_test -- "-DCONF_FILE=prj.conf;node_b.conf"
west flash
```

## What Needs To Change If This Fails

If this duplex test does not work reliably, the current architecture needs one
of these changes:

1. Change the transport ownership model.
   The cleanest fix is to stop mixing long-lived `isotp_bind()` receive contexts
   with Zephyr-managed sender FC filters for the same peer traffic.

2. Change the addressing/routing constraints.
   This implementation already separates DATA and FC by CAN ID, so any
   remaining issue would point to session/context ownership rather than just
   packet labelling.

3. Move to a fully manager-owned transport layer.
   That means classifying and routing `SF` / `FF` / `CF` / `FC` frames yourself
   instead of relying on Zephyr to privately consume FC for outbound sends.

Short version:

- if one-way tests pass and this duplex test still fails, the current
  Zephyr-native architecture is still the limiting factor even after distinct
  CAN-ID routing for DATA versus FC
- if you need reliable random bidirectional multi-frame traffic with the same
  peer, the architecture must own routing more explicitly than it does now
