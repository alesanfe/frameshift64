# src/game — game-domain types on the port side

- `port.h` — the decomp→port entry ABI: `PortInit`/`PortShutdown`/
  `PortIsRunning`, declared extern "C" so `decomp/src/sys/main.c` can
  call the composition root.
- `fighter_registry.{cpp,h}` — fighter metadata the port layer needs
  (icons, names, CSS data mapping).

Leaf module — decomp may include these headers; nothing here includes
product code.
