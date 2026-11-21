# CreatorX

CreatorX 是基于 LeviLamina 的基岩版快速建造模组。当前开发版实现了长方体选区，命令默认需要 OP 权限。

站在第一个角点执行 `/cx pos1`，走到对角执行 `/cx pos2`。`/cx size` 显示两个角的坐标、沿 X/Y/Z 的尺寸及方块总数，`/cx desel` 清除自己的选区。

设点也可以指定绝对、相对或局部坐标。例如，依次执行 `/cx pos1 0 64 0`、`/cx pos2 9 68 9`，得到 10 × 5 × 10、共 500 个方块的选区。

用 `/cx wand` 领取魔杖，左键方块设置点 1，右键设置点 2。`/cx togglewand` 可临时关闭或恢复魔杖设点。`/cx hpos1` 和 `/cx hpos2` 取准星瞄准的方块，默认最远 128 格。两个角点设好后，附近的选区棱线默认以线框显示，仅自己可见。将 `visual.mode` 改为 `particles` 可切换到粒子。

`/cx expand <格数> [反向格数] [方向]` 扩大选区，`/cx contract <格数> [反向格数] [方向]` 向指定方向收缩选区。方向可填 `north`、`south`、`east`、`west`、`up` 或 `down`，省略时取玩家视线方向。`/cx expand vert` 将选区延伸到当前维度的最低和最高可建造高度。

选区默认上限为 8,000,000 个方块，可在 `config.jsonc` 的 `limits.maxSelectionVolume` 中调整。短命令（如 `/pos1`）由 `command.shortCommands` 控制，双斜杠入口由 `command.chatDoubleSlash` 控制。

## License

CC0-1.0 © LeviMC (LiteLDev)
