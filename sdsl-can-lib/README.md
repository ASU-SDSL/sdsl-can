# sdsl-can-lib

`sdsl-can-lib` is a Zephyr module that provides the `can_link` transport API for
CAN + ISO-TP communication.

## What This Module Exposes

Public header:

- `include/can_link.h`

Current public API:

- `can_link_init()`
- `can_link_send()`
- `can_link_send_to()`
- `can_link_send_broadcast()`
- `can_link_node_id()`

This module is integrated with Zephyr through:

- [zephyr/module.yml](C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib/zephyr/module.yml)
- [Kconfig](C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib/Kconfig)
- [CMakeLists.txt](C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib/CMakeLists.txt)

## Add To Another West Workspace

In another Zephyr application's `west.yml`, add `sdsl-can-lib` as a project.

Example:

```yaml
manifest:
  remotes:
    - name: zephyrproject
      url-base: https://github.com/zephyrproject-rtos
    - name: your-org
      url-base: https://github.com/your-org

  projects:
    - name: zephyr
      remote: zephyrproject
      revision: v4.3.0
      import:
        name-allowlist:
          - cmsis_6
          - hal_stm32
          - nanopb

    - name: sdsl-can-lib
      remote: your-org
      revision: main
      path: modules/lib/sdsl-can-lib
```

Then run:

```powershell
west update
```

## Alternative: Use `ZEPHYR_EXTRA_MODULES`

If you do not want to add `sdsl-can-lib` to your manifest, you can point
Zephyr at the module directory directly.

Example:

```powershell
west build -b nucleo_f103rb app -- -DZEPHYR_EXTRA_MODULES=C:/path/to/sdsl-can-lib
```

## Enable In An App

In the consuming app's `prj.conf`, enable the module:

```conf
CONFIG_SDSL_CAN=y
```

Common library settings:

```conf
CONFIG_CAN_LINK_NODE_ADDR=0x01
CONFIG_CAN_LINK_PEER_NODE_ADDR=0x02
CONFIG_CAN_LINK_LOOPBACK=n
```

Optional receive-path controls:

```conf
CONFIG_CAN_LINK_DISABLE_UNICAST_RX=n
CONFIG_CAN_LINK_DISABLE_BROADCAST_RX=n
```

## Minimal Consumer App

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 4.2)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(my_can_app)

target_sources(app PRIVATE src/main.c)
```

`prj.conf`:

```conf
CONFIG_SDSL_CAN=y
CONFIG_CAN=y
CONFIG_ISOTP=y
CONFIG_NANOPB=y
CONFIG_CAN_LINK_NODE_ADDR=0x01
CONFIG_CAN_LINK_PEER_NODE_ADDR=0x02
```

`src/main.c`:

```c
#include <stddef.h>
#include <stdint.h>

#include "can_link.h"

static void on_can_message(const uint8_t *payload, size_t len, uint8_t source_node,
                           bool is_broadcast, void *user_data)
{
    ARG_UNUSED(payload);
    ARG_UNUSED(len);
    ARG_UNUSED(source_node);
    ARG_UNUSED(is_broadcast);
    ARG_UNUSED(user_data);
}

int main(void)
{
    int ret = can_link_init(on_can_message, NULL);
    if (ret != 0) {
        return 0;
    }

    while (1) {
    }
}
```

## Notes

- The consuming application must provide a valid Zephyr CAN device setup.
- Current test applications also include generated protobuf headers such as
  `link.pb.h`. That works in this workspace, but the intended long-term public
  interface is `can_link.h`.
- This repository includes its own [west.yml](C:/Users/wayne/Documents/SDSL/SquidSat/SDSL-CAN/sdsl-can-lib/west.yml)
  for module development, but consuming projects do not need to use that file
  directly.
