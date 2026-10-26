# 更正：TDM-I²S 顺序 1,3,2,4 —— MIC3 参考其实就在 2 通道上行里（2026-10-26）

> **本文撤回** [硬件/驱动审计](webrtc-media-hardware-driver-audit-2026-10-24.md) 的
> F1 与所有“MIC3 从未被读 / ch1=MIC2”结论。
> **新结论**：当前 2 slot 采集实际是 **MIC1 + MIC3（扬声器参考）**；
> AFE 的 `"MR"` 标注是**正确**的；设备 AEC 的参考数据一直在上行里，
> 只是 `CONFIG_USE_DEVICE_AEC` 未启用。
>
> 本节只更正事实与假设，**未修改音视频路径、未启用 AEC、未改通道/增益/swap**。

## 1. 决定性证据：数据手册的 TDM 通道顺序

ES7210 datasheet Rev 18.0（2021-10），**第 6 页**：

- **Figure 2e（TDM I²S Serial Audio Data Format）**：LRCK 内顺序为
  **`Channel 1 → Channel 3 → Channel 2 → Channel 4`**（已渲染页面核对，不是文本抽取假象）。
- Figure 2f（TDM Left Justified）才是 `1 → 2 → 3 → 4`。

本项目的 ES7210 配置：

- `es7210_mic_select()` 在 `mic_num >= 3` 时写 `REG12 = 0x02` 进入 TDM
  （`managed_components/espressif__esp_codec_dev/device/es7210/es7210.c`）；
- 格式函数走 `ES_I2S_FMT_I2S`（REG11 的 I²S 位），即 **Figure 2e**。

因此驱动在 TDM 模式下输出的 slot 顺序是 **1, 3, 2, 4**，不是 1, 2, 3, 4。

## 2. 与 2-slot 读取的合成

`esp_codec_dev` 在 `channel=2` 打开时把 RX 重配为
`total_slot=2 / ws_width=16 / slot_mask=0x3`（这一点不变）。
配合 Figure 2e，两个 slot 是：

```
slot0 = MIC1（物理麦克风）
slot1 = MIC3（AEC_P/N 扬声器参考）
```

所以：
- 当前上行 = **MIC1 + 参考**；`input_reference=true` → AFE `"MR"` 是**正确配置**；
- 之前“MIC3 不在读取范围、ch1=MIC2、AFE 标注错误”的判断**全部撤回**；
- 4-slot 初始化被 2-slot 重配**不是缺陷**：它恰好取到 mic+reference。

## 3. 设备实测与之一致（AUDIT 封孔实验）

2026-10-26，`AUDIT 6` 在只封住正面两个麦克风针孔（扬声器不动）后：

| 条件 | tone ch0 | tone ch1 | 结论指向 |
|---|---|---|---|
| 无遮挡 | -22.8 dBFS | -19.8 dBFS | 参考+麦克风混合 |
| 封麦孔 | **-23.3 dBFS** | **-18.4 dBFS** | ch1 几乎不受封孔影响 ⇒ **电气参考** |
| 厚布遮脸 | -25.1 dBFS | -22.7 dBFS | 底噪降 7–8 dB，tone 仅降 2–3 dB |

ch1 在“封孔”后保持甚至略升，符合“ch1 是扬声器参考（电气）”；
ch0 也残留 -23.3 dBFS，可能是参考走线对 MIC1 的电气串扰，或封孔未完全密封——
不影响“ch1 = 参考”的结论，但需要下一轮区分 MIC1 的残留来源。

## 4. 对双向循环的含义

- 上行**同时包含** MIC1 与参考，但 `CONFIG_USE_DEVICE_AEC` 未启用，
  AFE 的 `aec_init=false`，参考被忽略；服务端仍会收到 MIC1 里的扬声器回声，
  于是 `turn_detected` 取消回答（B52）。
- 因此“循环”的根因更可能是**没有启用设备 AEC**，而不是“缺少参考”。
- 历史“启用 AEC 后语音峰值 4036→120”需要重新评估：当时参考通道映射/双讲处理
  可能有误，不能再用它证明“硬件无参考”。

## 5. 下一步（需新授权，不在本文执行）

1. 在 M15 诊断固件里**有条件地**启用 `CONFIG_USE_DEVICE_AEC=y`（保持 `"MR"`），
   先做**本地**验证：扬声器播放已知音时，AFE 输出里的回声应显著低于输入；
   再做双讲测试，确认不吞用户语音。
2. 若双讲下仍吞语音，检查 AEC 模式/延迟/参考增益与 AFE 版本，而不是回退到半双工。
3. 4ch/帧宽改动**不再必要**（参考已在 2ch 里）；如要读 MIC2/MIC4 才需要另设计。
4. 所有测试仍须新弹窗 Q；未授权前不刷机、不 START。

## 6. 需要同步更正的文件

`AGENTS.md`、`docs/aec-limitation.md`、`docs/stackchan-hardware.md`、
`docs/webrtc-media-hardware-driver-audit-2026-10-24.md`、
`docs/firmware-findings.md`（B54/B56）、`docs/webrtc-media-spec.md`、
`docs/webrtc-media-plan.md`、`docs/webrtc-media-redevelopment-2026-10-23.md`、
本轮的评审请求/结果文档。

## 7. 证据链接

- [ES7210 datasheet（Rev 18.0）](https://www.pawpaw.cn/media/documents/2021-12/ES7210_PB.pdf)，第 6 页 Figure 2e/2f。
- 本仓库驱动：`managed_components/espressif__esp_codec_dev/device/es7210/es7210.c`（TDM 使能/格式）。
- AUDIT 日志：`docs/device_diag_evidence/2026-10-24-m15/audit-06-*.log`。
- 官方 C156 图（物理位置）：[C156 图 01](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1254/C156_StackChan_Core_main_pictures_01.webp)。
