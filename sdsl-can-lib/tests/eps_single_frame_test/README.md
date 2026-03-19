# EPS Single-Frame Test

This test is a two-board CAN smoke test for the `can_link` transport path.

It verifies:

- board A can transmit a CAN/ISO-TP single-frame payload to board B
- board B can receive and decode that payload
- node addressing is correct (`node 1 -> node 2`)
- the basic CAN wiring, bitrate, and peer configuration are working

This test intentionally keeps the encoded payload within a single ISO-TP frame.
That means it does not exercise multi-frame flow control. It is meant to be a
reliable baseline test before debugging multi-frame behavior.

## Board Roles

- `node_a.conf`: sender board
- `node_b.conf`: receiver board

Behavior:

- node `0x01` transmits periodically to node `0x02`
- node `0x02` stays RX-only and logs received frames

## Files

- `src/main.c`: single-frame test app
- `prj.conf`: common Zephyr test configuration
- `node_a.conf`: sender node settings
- `node_b.conf`: receiver node settings
- `boards/nucleo_f103rb.overlay`: CAN device and bitrate overlay

## Build And Flash

Build and flash board A:

```powershell
west build -p always -b nucleo_f103rb .\sdsl-can-lib\tests\eps_single_frame_test -- "-DCONF_FILE=prj.conf;node_a.conf"
west flash
```

Build and flash board B:

```powershell
west build -p always -b nucleo_f103rb .\sdsl-can-lib\tests\eps_single_frame_test -- "-DCONF_FILE=prj.conf;node_b.conf"
west flash
```

Note:

- keep `"-DCONF_FILE=prj.conf;node_a.conf"` and `"-DCONF_FILE=prj.conf;node_b.conf"` quoted in PowerShell
- board A and board B must share CAN ground and CANH/CANL

## Expected Logs

Sender board (`node_a.conf`):

```text
<inf> eps_single_frame: Single-frame node 1 online. TX Period: 1500ms
<inf> eps_single_frame: TX sent: target=2 prio=0 len=4 seq=0
```

Receiver board (`node_b.conf`):

```text
<inf> eps_single_frame: Single-frame node 2 online. TX Period: 1500ms
<inf> eps_single_frame: Node 2 configured as RX-only peer for single-frame test
<inf> eps_single_frame: RX [Prio:0] src:1->dst:2 len:4 (BC:0)
```

## What A Passing Test Means

If the test passes, the following are working:

- CAN controller setup
- board overlay and bitrate selection
- peer node configuration
- single-frame send/receive path in `can_link`
- protobuf encode/decode for the minimal test payload

## What This Test Does Not Cover

This test does not validate:

- multi-frame ISO-TP flow control
- simultaneous bidirectional traffic
- broadcast traffic
- mixed CAN priorities

Those should be covered by separate integration tests.
