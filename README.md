# CreatorX

CreatorX 是基于 LeviLamina 的基岩版快速建造模组。当前开发版实现了长方体选区，命令默认需要 OP 权限。

站在第一个角点执行 `/cx pos1`，走到对角执行 `/cx pos2`。`/cx size` 显示两个角的坐标、沿 X/Y/Z 的尺寸及方块总数，`/cx desel` 清除自己的选区。

设点也可以指定绝对、相对或局部坐标。例如，依次执行 `/cx pos1 0 64 0`、`/cx pos2 9 68 9`，得到 10 × 5 × 10、共 500 个方块的选区。

选区默认上限为 8,000,000 个方块，可在 `config.jsonc` 的 `limits.maxSelectionVolume` 中调整。短命令（如 `/pos1`）由 `command.shortCommands` 控制，双斜杠入口由 `command.chatDoubleSlash` 控制。

## License

CC0-1.0 © LeviMC (LiteLDev)
