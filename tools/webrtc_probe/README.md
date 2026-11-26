# 标准 WebRTC 对照探针

用**标准 WebRTC 栈**（`node-datachannel`）从 Mac 直连阿里云 Realtime WebRTC 端点，
用于把"设备侧问题"与"网络/端点问题"分开。

## 为什么需要它

M0 在设备上失败时，无法区分是 `esp_peer` 的问题还是网络/端点的问题。
这个探针用同一网络、同一端点、同一流程（非 trickle SDP 交换 + DataChannel），
但换成成熟的 WebRTC 实现：

- 探针**成功** ⟹ 网络与端点没问题，故障在 `esp_peer` 侧
- 探针**也失败** ⟹ 环境或端点的问题，调 `esp_peer` 无意义

2026-10-07 实测：探针 31ms 完成 ICE 并收到 `session.created` / `session.updated`，
据此把 M0 的失败定位到 `esp_peer` 的 ICE 提名缺陷。

## 用法

```bash
cd tools/webrtc_probe
npm install                       # 安装 node-datachannel

node standard_webrtc_probe.js <apiKey> <workspaceId> <model>
```

凭据从 macOS Keychain 取（不要在命令行留下痕迹、不要写入文件）：

```bash
KEY=$(bash -lc "~/.agents/skills/blue-keychain-save/keychain.sh get \
  --service blue-stackchan-bailian-key")
node standard_webrtc_probe.js "$KEY" "ws-ik1clk14dycbzaqx" "qwen3.8-omni-flash-realtime"
```

## 输出要点

```
[   746ms] [ice] connected            ← 标准栈 ICE 毫秒级完成
[  2789ms] [server-opened channel] txt
[  2800ms] <<<[txt] {"type":"session.created",...}
[  2946ms] <<<[txt] {"type":"session.updated",...}
```

## 两个已知要点

1. **音频段必须注册编解码器。** 只 `addTrack(new ndc.Audio(...))` 会产生
   `m=audio ... SAVPF `（payload type 列表为空、无 `a=rtpmap`），
   服务端返回 `HTTP 400 remote_sdp_failed`。必须调 `addOpusCodec(111)`。
2. **事件走服务端创建的 `txt` 通道**（stream 1），不是客户端建的
   `oai-events`（stream 0）。`session.update` 要在收到 `session.created`
   的那条 stream 上回复，否则拿不到 `session.updated`。

## 局限

- `localDescription()` 返回 `{type, sdp}` 对象而非字符串；
  候选地址由 `onLocalCandidate` 收集后注入 SDP（本探针为非 trickle 流程）
- 只验证控制面（SDP/ICE/DTLS/SCTP/DataChannel），**不发送媒体**，
  因此不能证明服务端 AEC、双向语音或语音打断可用
