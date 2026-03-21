# EPS Message Test

This test is a two-board multi-frame CAN/ISO-TP integration test for
`can_link`.

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

## Important Current Limitation

For this test, board A disables the background RX workers in `can_link`.
This is intentional.

With the current `can_link` design, the sender's general RX worker can compete
with the ISO-TP sender's flow-control filter. When that happens, returned FC
frames may be consumed by the wrong receive path and multi-frame transfers can
stall.

Because of that, this test uses:

- sender board: unicast RX disabled, broadcast RX disabled
- receiver board: broadcast RX disabled

This makes the multi-frame unicast path reliable for the current design.

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
<inf> can_link: Unicast RX disabled by configuration
<inf> can_link: Broadcast RX disabled by configuration
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
- one-way multi-frame transport in the current `can_link` design

## What This Test Does Not Cover

This test does not validate:

- simultaneous bidirectional multi-frame traffic
- broadcast traffic
- multiple active application RX paths on the sender node
- generalized multi-priority receive handling

Those need additional library work or separate integration tests.
