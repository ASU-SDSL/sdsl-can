# EPS Message Zephyr ISO-TP Test

This test is a Zephyr-native ISO-TP variant of `eps_message_test`.

It exercises the updated [can_link.c](/C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib/src/can_link.c),
which is now the default `can_link` implementation for the library.
That implementation keeps transport ownership inside Zephyr:

- unicast RX uses `isotp_bind()` + `isotp_recv()`
- TX uses asynchronous `isotp_send()`
- ISO-TP fixed addressing is used together with distinct CAN IDs for data and FC traffic
- the public `can_link_*` API stays the same

## What It Verifies

- board A can start a multi-frame unicast send to board B
- board B can receive and decode the payload through the `can_link` callback
- the Zephyr-native `can_link.c` path works as the library implementation

## Test Shape

This remains a two-board, one-way integration test:

- node `0x01` is the sender
- node `0x02` is the receiver
- node `0x02` stays RX-only at the app level

The goal is to validate the updated structure first before attempting more random duplex traffic.

## Addressing Notes

The current transport uses ISO-TP fixed addressing together with separate
CAN-ID routing for data and flow control:

- normal payload traffic uses a DATA CAN-ID base
- returned flow-control traffic uses a different FC CAN-ID base
- broadcast uses its own CAN-ID base

Because fixed addressing keeps the address in the 29-bit CAN ID, the
single-frame payload limit stays at `7` bytes on classical CAN.

## Files

- `src/main.c`: test application
- `prj.conf`: common Zephyr configuration
- `node_a.conf`: sender board settings
- `node_b.conf`: receiver board settings
- `boards/nucleo_f103rb.overlay`: CAN device and bitrate overlay

## Build And Flash

Build board A:

```powershell
west build -p always -b nucleo_f103rb sdsl-can-lib\tests\eps_message_zephyr_isotp_test -- "-DCONF_FILE=prj.conf;node_a.conf"
west flash
```

Build board B:

```powershell
west build -p always -b nucleo_f103rb sdsl-can-lib\tests\eps_message_zephyr_isotp_test -- "-DCONF_FILE=prj.conf;node_b.conf"
west flash
```

## Expected Logs

Sender board (`node_a.conf`):

```text
<inf> can_link: CAN start node=1 peer=2 loopback=0
<inf> can_link: ISO-TP RX ready id=...
<inf> eps_zephyr_isotp: Zephyr ISO-TP session test node 1 online. TX Period: 1500ms
<inf> eps_zephyr_isotp: TX started: target=2 prio=0 len=12 seq=1
<inf> can_link: ISO-TP TX complete target=2 broadcast=0
```

Receiver board (`node_b.conf`):

```text
<inf> can_link: CAN start node=2 peer=1 loopback=0
<inf> can_link: ISO-TP RX ready id=...
<inf> eps_zephyr_isotp: Zephyr ISO-TP session test node 2 online. TX Period: 1500ms
<inf> eps_zephyr_isotp: Node 2 configured as RX-only peer for Zephyr ISO-TP session test
<inf> eps_zephyr_isotp: RX [Prio:0] src:1->dst:2 type:1 seq:1 (BC:0)
```

## Notes

- application logging now says `TX started` because `can_link_send_to()` starts an async ISO-TP transfer and returns immediately
- completion is logged from [can_link.c](/C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib/src/can_link.c)
- this test is meant to validate the updated Zephyr-centric fixed-address transport structure with distinct CAN-ID routing, not the earlier raw session-manager prototype
