# xiaozhi-ADV-car 实现计划

> Fork 自 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)，板型 OTA 名 `m5stack-cardputer-adv-car`  
> 目标：聊天页 + 小车控制页（上下左右停），可扩展多页；键盘 WiFi 配网

## 结论

**可行。** 上游 ADV 已含键盘配网（`wifi_config_ui`）；主要新增板级 PageManager + 独立 EMQX 车控 MQTT。  
预计 **10–14 新文件、6–8 改文件、约 1800–2500 LOC**，**4–6 次 agent session** 可落地 MVP。

---

## 架构要点

### 多页

```
PageId { PAGE_CHAT=1, PAGE_CAR=2, /* 未来 PAGE_FOC=3 */ }
Fn+1 → 聊天，Fn+2 → 车控（对齐 cardputer-motor）
```

- 全部在 `main/boards/m5stack-cardputer-adv-car/`
- 切页用 `lv_obj HIDDEN` 隐藏聊天 UI，**禁止** `lv_obj_clean` 毁树（wifi_config 退出需修）

### 双 MQTT

| 通道 | 用途 |
|------|------|
| 小智 `MqttProtocol` | 语音/MCP，**不动** |
| 新增 `EmqxCarMqtt` | `broker-cn.emqx.io` → `car/cmd` + `foc/cmd` |

Client ID：`xiaozhi-adv-car-<mac后4>`

### 分区 / RAM

- 8MB Flash，无 PSRAM（`CONFIG_SPIRAM=n`）
- 车页 LVGL 极简，MQTT 缓冲 ≤512B

---

## 文件清单

### 新建（10–14）

| 文件 | 行数 | 说明 |
|------|------|------|
| `page_id.h` / `page.h` / `page_manager.*` | ~240 | 多页壳 |
| `cardputer_adv_lcd_display.*` | ~160 | 子类 HIDDEN 切页 |
| `net/emqx_config.h` | ~40 | broker/topics |
| `net/car_state.h` / `foc_state.h` | ~85 | 状态 |
| `net/emqx_mqtt_client.*` | ~300 | 第二条 MQTT |
| `pages/car_page.*` | ~330 | LVGL 车控 UI |

### 修改（6–8）

| 文件 | 说明 |
|------|------|
| 从 `m5stack-cardputer-adv` 拷贝整目录 → `-car` | 基线 ~1800 行 |
| `config.json` | `name`: `m5stack-cardputer-adv-car` |
| `Kconfig.projbuild` / `CMakeLists.txt` | 新板型 |
| 板主 `.cc` | PageManager + 键路由 |
| `wifi_config_ui.*` | 退出后恢复 Chat UI |

### 尽量不改

`application.cc`、`lcd_display.*`、`mqtt_protocol.*`（用板级子类）

---

## 阶段

| Phase | 目标 | 验收 |
|-------|------|------|
| 0 | Fork + 独立 board 名 + 能编译烧录 | 单页小智 Chat |
| 1 | PageManager + HIDDEN 切页 | Fn+1/2 切换不崩 |
| 2 | EMQX + CarPage + 按键 | 麦轮能动车 |
| 3 | WiFi 键盘配网修复 | 无手机可配网 |
| 4 | 进车页停语音、安全停、README | 稳定 |

---

## 合并上游

- `upstream` = `78/xiaozhi-esp32`，分支 `feat/adv-car-pages`
- OTA 名 **必须** 用 `m5stack-cardputer-adv-car`，勿用官方 `m5stack-cardputer-adv`
- 车控代码 **仅** 在 board 目录；`PROJECT_VER` 跟上游 tag

---

## 车控按键（Car 页）

| 按键 | 动作 | Topic | Payload |
|------|------|-------|---------|
| Fn + ;（↑） | 前进 | `car/cmd` | `{"run":1,"speed":50}` |
| Fn + .（↓） | 停 | `car/cmd` | `{"run":0,"speed":50}` |
| Fn + ,（←） | 左转 | `foc/cmd` | `{"dir":1,"speed":50}` |
| Fn + /（→） | 右转 | `foc/cmd` | `{"dir":-1,"speed":50}` |
| Fn + 1 / 2 | 聊天 / 车控页 | — | 切页前可 `run=0` |

协议见 `~/Documents/esp/car/esp32/io.md`、`head-tracker/cardputer-motor/AGENTS.md`

---

## 风险

- RAM 紧 → 车页极简 UI
- WDT → MQTT 队列单线程（对齐 `car/esp32`）
- 键盘冲突 → **按页路由**键事件
- 同 broker 多车 → 文档说明；后续可加 topic 前缀

---

## 参考源码

- 小智 ADV：`main/boards/m5stack-cardputer-adv/`（本地 `xiaozhi-esp32-main` v2.2.6）
- 车控逻辑：`head-tracker/cardputer-motor/src/`

---

## 待确认

1. OTA 名 `m5stack-cardputer-adv-car` ✓ 推荐
2. 车页合并 ↑↓ + ←→（推荐）vs 仅启停
3. Fork 路径 `~/Documents/esp/xiaozhi-ADV-car`

---

## 进度

- [x] Phase 0：fork + `m5stack-cardputer-adv-car` 注册
- [x] Phase 1：PageManager + Fn+1/2 + HIDDEN 切页（占位车控 UI）
- [ ] Phase 2：EMQX MQTT + 车控按键
- [ ] Phase 3：WiFi 退出 UI 修复
- [ ] Phase 4：联调与文档
