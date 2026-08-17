# Cardputer ADV Car 板级文档

本目录是 **m5stack-cardputer-adv-car** 板型的说明，对应固件入口 `m5stack_cardputer_adv_car.cc`。硬件是 M5Stack Cardputer ADV：ESP32-S3FN8、8MB Flash、**无 PSRAM**、ST7789 240×135、TCA8418 56 键、ES8311 音频、板载红外 GPIO 44。

| 文档 | 内容 |
|------|------|
| [architecture.md](architecture.md) | 目录结构、页面框架、切页、内存生命周期 |
| [pages.md](pages.md) | 小智主页 + 启动器 + 9 个分页面：画面、按键、进出页行为 |
| [images.md](images.md) | 以后加图怎么转 RGB565、为什么不能运行时解 PNG、内存坑 |

全局切页（任意页面都有效）：

| 组合 | 去哪 |
|------|------|
| **Fn+1** | 小智聊天主页 |
| **Fn+2** | 启动器（9 个分页面入口） |

分页面只能从启动器数字键 1–9 进入，没有 Fn+3 这类直切。
