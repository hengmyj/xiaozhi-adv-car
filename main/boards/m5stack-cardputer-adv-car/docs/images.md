# 图片与内存坑

本板 **无 PSRAM**，内部 SRAM 空闲大约 40KB，最大连续块常常只有 **12–18KB**。以后往任意页面加图，按这条路走，不要再试运行时解 PNG。

## 正确做法：编译期转 RGB565，flash 直读

1. 源图用真正的全彩像素画 / 插画，不要用白底截图、不要先阈值剪影。
2. 在电脑上缩到目标尺寸（Dino logo 验证过 **120×90** 一屏居中刚刚好）。
3. 转 RGB565 数组，写进 `common/xxx_icon.h` 的 `static const uint16_t` + `lv_image_dsc_t`。
4. 页面里：

```cpp
#include "xxx_icon.h"

icon_ = lv_image_create(panel_);
lv_image_set_src(icon_, &kXxxIcon);
lv_img_set_zoom(icon_, 256);   // 256 = 1x；512 = 2x
lv_obj_align(icon_, LV_ALIGN_CENTER, 0, 0);
```

数据在 `.rodata`（8MB flash），LVGL 按 RGB565 直接扫，**运行时 0 字节解码缓冲**。120×90 占 flash `120*90*2 = 21600` 字节。

透明区域填页面背景色（Dino 是 `0x0A0A12`），不要依赖 ARGB。

### 转换脚本（Pillow）

```python
from PIL import Image

W, H = 120, 90
BG = (0x0A, 0x0A, 0x12)  # 与页面背景一致

im = Image.open("rr.png").convert("RGBA").resize((W, H), Image.Resampling.LANCZOS)
pixels = []
for y in range(H):
    for x in range(W):
        r, g, b, a = im.getpixel((x, y))
        if a < 128:
            r, g, b = BG
        rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        pixels.append(rgb565)

# 再写成 C 数组 + lv_image_dsc_t：
#   .header.magic = LV_IMAGE_HEADER_MAGIC
#   .header.cf    = LV_COLOR_FORMAT_RGB565
#   .header.w/h   = W, H
#   .header.stride = W * 2
#   .data_size    = W * H * 2
#   .data         = (const uint8_t*)array
```

写进头文件前先在电脑上预览：唯一色数、白像素占比。**白像素超过大约一半，放到深色页上就会像「占位符 / 空图」。** 这是 Dino logo 第一版失败的根因——源图是白填充卡通截图，不是渲染 API 坏了。

## 明确不要做的

| 做法 | 为什么挂 |
|------|----------|
| 运行时 LODEPNG 解 PNG | 解码成 ARGB8888。120×90 就要约 43KB，即便更小也要约 19KB **连续**堆；本板最大连续块 ~18KB，`lv_malloc` 失败，图是空的 |
| `lv_image` 指 PNG 原始字节 + `LV_COLOR_FORMAT_RAW` | 一样要走解码器，RAM 问题相同 |
| 从 SPIFFS/LittleFS 读 PNG | LVGL 文件系统驱动没开 |
| 阈值剪影 / 只留轮廓 | 全彩插画会变成一块白或一块黑 |
| 用 `lv_canvas` + RAM 缓冲去显示静态图 | 静态图没必要拷进 RAM；canvas 留给 Clock / Rain 这种每帧重画 |
| 先怀疑 `lv_timer_handler` / `lv_obj_set_size` | 独占页的 LVGL 任务一直在跑。图不对先 dump 源数据 |

Clock / Rain 用 canvas 是因为像素每帧变，缓冲放 **BSS 静态数组**（不走堆）：Clock 182×54 ≈ 19.6KB，Rain 120×68 ≈ 16.3KB。静态 logo 不要学它们去占 BSS。

## 尺寸经验

| 用途 | 建议 | flash |
|------|------|-------|
| 全屏点缀 logo（Dino Ready/Dead） | 120×90，1x | 21.6KB |
| 小图标 | 32×32 或 48×48 | 2–4.5KB |
| 超过 ~160×120 的全彩图 | 先缩小；8MB flash 够，但编译体积和 cache 都变差 | >38KB |

多张图可以共存于 flash，只要运行时不要同时再申请大块堆。

## 切页内存（和图片同一类坑）

隐藏的 LVGL 树仍占内部 SRAM：

- 启动器约 10KB / 46 对象 → 进 Snake/Dino 必须 Destroy。
- Music 24 柱、Radio 24 柱 → 离开必须 Destroy，否则下一次 Radio 没有连续块给 MP3。
- Car/Spider/IceBox 互切保留 panel（Destroy 曾导致 IceBox→Car 卡死）；离开这一组再 Destroy。
- 进 Radio 会 sweep 所有其它独占页。
- 回 Chat 再重建 Opus（约 43KB），不要在 Radio/Music `OnLeave` 里重建。

串口对照：`ShowPage enter/leave`、`DestroyPanel`、`heap=`、`largest=`。`largest` 掉到 12KB 以下还硬开 Radio / 解 PNG，基本必失败或重启。
