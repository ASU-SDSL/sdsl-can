# EPS Message Test

This test is a two-board multi-frame CAN/ISO-TP integration test for
`can_link`.

The library now builds the Zephyr-native session-backed implementation in
`src/can_link.c`.

It verifies:

- board A can send multi-frame unicast payloads to board B
- board B can receive and decode those payloads
- ISO-TP flow control is working across two physical boards
- protobuf encode/decode works for larger payloads

This test is intentionally configured as a one-way transfer:

- node `0x01` is the sender
- node `0x02` is the receiver

That avoids overlapping ISO-TP sessions while validating the current
`can_link` multi-frame behavior.

## Board Roles

- `node_a.conf`: sender board (`node_id=0x01`, `peer_id=0x02`)
- `node_b.conf`: receiver board (`node_id=0x02`, `peer_id=0x01`)

Behavior:

- node `0x01` sends periodic unicast messages
- node `0x02` stays RX-only at the app level and logs received messages

## Files

- `src/main.c`: multi-frame test app
- `prj.conf`: common Zephyr test configuration
- `node_a.conf`: sender node settings
- `node_b.conf`: receiver node settings
- `boards/nucleo_f103rb.overlay`: CAN device and bitrate overlay

## Build And Flash

From the workspace root (`SDSL-CAN`), build and flash board A:

```powershell
west build -p always -b nucleo_f103rb sdsl-can-lib\tests\eps_message_test -- "-DCONF_FILE=prj.conf;node_a.conf"
west flash
```

Build and flash board B:

```powershell
west build -p always -b nucleo_f103rb sdsl-can-lib\tests\eps_message_test -- "-DCONF_FILE=prj.conf;node_b.conf"
west flash
```

Note:

- if you are switching from an older layout, remove the workspace-root `build/` directory first
- keep `"-DCONF_FILE=prj.conf;node_a.conf"` and `"-DCONF_FILE=prj.conf;node_b.conf"` quoted in PowerShell
- board A and board B must share CAN ground and CANH/CANL

## Expected Logs

Sender board (`node_a.conf`):

```text
<inf> can_link: CAN start node=1 peer=2 loopback=0
<inf> eps: Satellite Link Node 1 online. TX Period: 1500ms
<inf> eps: TX sent: target=2 prio=0 len=12 seq=17
```

Receiver board (`node_b.conf`):

```text
<inf> eps: Satellite Link Node 2 online. TX Period: 1500ms
<inf> eps: Node 2 configured as RX-only peer for two-board test
<inf> eps: RX [Prio:0] src:1->dst:2 type:1 seq:17 (BC:0)
```

## What A Passing Test Means

If the test passes, the following are working:

- CAN controller setup
- two-board unicast addressing
- multi-frame ISO-TP flow control
- protobuf encode/decode for larger messages
- one-way multi-frame transport in the session-backed `can_link` design

## What This Test Does Not Cover

This test does not validate:

- simultaneous bidirectional multi-frame traffic
- broadcast traffic
- generalized multi-priority receive handling

Those need separate integration tests.
