# src/core/persistence — save data

`port_save.{cpp,h}` — SRAM save image + `.meta` sidecar
(format_version stamp). Writes are temp+fsync+atomic-rename; write
failures surface as an in-game overlay. Leaf module — may log via
`observability`, nothing else.
