# AFE 软件播放参考条件预研（只读）

HEAD：3ab11e9823866fa0979298caa71184c15bb6c478（本地当前提交）；generated xiaozhi / managed components 的实际文件另以 SHA256 固定，见本文末尾 SHA256 表。没有实现、构建、联网会话、设备/凭据/DSH/PR操作。M1.5 HOLD 与冻结规格不变。

## 结论

当前 ESP-SR 2.3.1 的公开算法接口确实接收普通 PCM 播放参考，而不要求参考必须来自 ADC 第三路。因此“从软件播放 PCM 组成虚拟 reference 通道”有明确 API 可行性；当前 StackChan 应用没有这条连接，没有参考与麦克风对齐、DAC/功放链路一致性、真实双讲效果证据。不能据 API 接收 PCM 宣称设备 AEC/打断已经可用，不能撤销现有半双工限制或 M1.5 HOLD。

## 本机实际配置与路径

- dependencies.lock:174-191 / managed esp-sr idf_component.yml:16 = 2.3.1。
- main/hal/board/config.h:22-24：AUDIO_INPUT_REFERENCE=true，input/output均24000；不是旧文档中描述的当前 MM 格式。
- main/hal/board/stackchan.cc:605-611把这些配置交给当前 CoreS3AudioCodec。
- main/hal/board/cores3_audio_codec.cc:13,28保存reference=true但固定 input_channels_=2；:223-244打开16-bit、2ch并对两个物理麦通道设置增益；:272-275从esp_codec_dev_read读取。M5Stack官方CoreS3资料说明ES7210双麦输入： https://docs.m5stack.com/en/core/CoreS3 。不能从mic_selected=MIC1|MIC2|MIC3（:86）推断第三槽实际有可用回采波形。
- audio_service.cc:184-213按codec物理2ch读24k，并在有重采样器时转换到16k2ch；:268-278将这两路直接送AFE，没有混入播放 PCM。
- afe_audio_processor.cc:20-28：ref_num=1、channels=2，所以 input_format="MR"。第1路是真麦，第2路也来自麦采样，被配置标签R标记；“R”标签本身不使它成为播放参考。
- nonsecret sdkconfig:768 USE_AUDIO_PROCESSOR=y；:769 USE_DEVICE_AEC未设；:821 SR_NSN_WEBRTC=y；:823 SR_VADN_WEBRTC=y；:863 wake model wn9_histackchan_tts3（这是唤醒路径，不等于当前voice AFE带WakeNet）。afe_audio_processor.cc:40设AFE_TYPE_VC / HIGH_PERF，:41预置VOIP_HIGH_PERF，但:59-64关闭AEC并开启VAD。:37-53仅发现NSNET模型时才开NS；在现有NSN_WEBRTC配置下未选NSNET，所以当前代码分支禁用NS。:56关闭AGC。不要把未启用的VOIP_HIGH_PERF写成正在工作的AEC模型，也不要把VAD“WebRTC”误认成WebRTC服务端AEC。
- 上述是本地配置和调用推导；没有运行设备取得创建后afe_config_print/print_pipeline输出，内部最终pipeline仍需区分。

## 明确支持的输入 API

- 本机esp_afe_config.h:152-167定义 M=mic、R=playback reference、N=unused；:53-60定义总通道、mic_ids、ref_ids；:196-227提供PCM拆分/交错工具。
- esp_afe_sr_iface.h:89-100：feed接受const int16_t*，16-bit signed、16k、通道交错；:211-215提供get_feed_chunksize和get_feed_channel_num。这是内存PCM契约，没有要求全部通道来自同一硬件ADC。
- esp_aec.h:62-73：aec_process(handle, indata, refdata, outdata)明确将refdata定义为送扬声器的16-bit采样数组；:39-47明确16000采样率。它证明软件参考并非凭空发明的输入形式。
- esp_afe_aec.h:25-27的单独afe_aec wrapper当前只选择第一路mic和reference，不能据此保证同一wrapper对双麦独立AEC；不混淆单独wrapper和全AFE。
- Espressif官方FAQ明确区分芯片ADC取得的reference与主控制器复制的software reference，证实两种来源可存在： https://docs.espressif.com/projects/esp-faq/en/latest/application-solution/audio-development-framework.html#what-is-the-difference-between-software-aec-acoustic-echo-cancellation-and-hardware-aec-in-the-esp-adf-audio-application-development-framework 。这说明算法输入来源，不证明本项目同步/效果。
- 官方AFE说明为交错16kPCM，并给从I2S或文件喂入的示例： https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/audio_front_end/README.html 。v2.3.1本机头文件是实际版本证据；latest文档新增FD模式等不属于该版本现有API，不把新模式建议迁回当前2.3.1。

## 软件播放 PCM 与 AFE 输出的现有观测位置

- AudioService旧下行：audio_service.cc:350-378 Opus解码并重采样到codec输出率24k；:380-382入播放队列；:298-309从实际播放队列取task->pcm后调用codec->OutputData。:309是比“刚收到/刚解码”更接近播放的CPU PCM位置，但仍只是提交播放，不是DAC实际呈现时刻。
- AudioCodec::OutputData audio_codec.cc:17-19只调用Write；CoreS3AudioCodec::Write :279-283向esp_codec_dev_write提交。没有提供逐帧实际DAC呈现timestamp/硬件回采；Write即使未enable也返回samples，且ESP_ERROR_CHECK_WITHOUT_ABORT后仍返回samples，不能仅靠该返回值证明实际播放。
- 音量在cores3_audio_codec.cc:204-206,265通过codec_out_vol设置，不从现有task->pcm观察最终功放输出。PCM可复制但不能覆盖未观测的codec增益、扬声器失真和麦ADC饱和。
- AFE已有processed PCM观测点：afe_audio_processor.cc:144 fetch，:166-183无条件累积res->data并经OnOutput发16kmono；audio_service.cc:101-103当前直接送旧Opus编码队列。可访问于当前processor内部callback；AudioService公开Callbacks :78-83没有独立PCM监听钩子；当前没有持久化pre/post AEC与reference同步波形。
- esp_afe_sr_iface.h:30-50 res->data/data_size是增强后PCM，raw_data有多通道输出字段，但注释不保证它是原始ADC输入或AEC前PCM，不将该字段冒充有效原始观测。data_volume字段还明确VC模式无效（:36-38）。
- CONFIG_USE_AUDIO_DEBUGGER当前未开；其audio_service.cc:219-224位置只观察读入的mic PCM，没有播放参考。
- 当前M1是fixture路径；webrtc_m1.cc:1285-1287收到真实下行仅计数，不播放；其:492-524诊断解码自身上行不构成扬声器播放或reference。故旧AudioService中存在tap位置，不等于WebRTC M1.5已接入tap。

## 当前未支持的连接与不确定项

1. 现有Feed(data)使用 codec_->input_channels() 计算帧stride（afe_audio_processor.cc:102），Initialize也从codec物理ch数决定M/R。若要保留两麦再追加软件R，AFE三路输入与物理ADC两路需要显式分开，现有应用没有相应接口/状态；不能仅把codec ch改3或打开AEC。若仅一路mic+softwareR，算法已有MR输入形式，但当前第二路仍是mic，应用未用软件PCM替换它。
2. 本机esp_afe_sr_iface_t :205-235没有独立feed_render/reference、外部render/capture时间戳或参考延迟设置入口。fetch_with_delay的ticks_to_wait是等待fetch结果的超时，不是AEC参考延迟补偿（:114-124）。aec_filter_length是滤波长度，不是自动保证播放/采集同步。
3. 两条任务与播放队列、I2S DMA/codec缓存、24→16重采样的历史/群延迟和实际read/write覆盖区间未测量。冻结48k编解码仍可存在，参考应从实际最终播放PCM派生到AFE要求16k；不能靠改变RTP clock或随意降48k要求解决参考对齐。
4. 单一OutputData层可覆盖当前旧音效/TTS等同层播放，但需要确认实际混音/全部声音来源、未播放/取消队列不得提前进reference、空闲/静音应维持采集侧时间连续；当前没有这样的参考环形队列与样本序号契约。
5. 官方FAQ还明确效果依赖无失真播放、无噪声录音和有效reference。本机没有与软件参考同步的ADC及post-AEC录音，没有ERLE/双讲保真/恢复证据。因此可行性结论仅为“现有SDK允许该PCM输入”，不是“可以达到真实回声抑制/打断验收”。
6. 历史三路ADC限制继续成立为现有实验：a)项目记录了3ch失败/peak0；b)本机esp_codec_dev audio_codec_data_i2s.c:178-181确实拒绝channel=3（奇数格式检查）。仅这个判断不能证明所有多路TDM配置都绝对不支持，但本任务不尝试其他硬件读法，也不据此撤销“当前未取得第三路有效参考”状态。

分类：SDK接受软件参考——已明确支持；当前StackChan把它接入AFE——未实现；参考时序/模拟链路一致性与性能——证据缺失；无需硬件第三ADC即可自动可靠AEC——不成立。保持条件预研，不改变M1.5 HOLD，不替换当前server-AEC假设测试，也不实施新方案。

## 本次只读文件 SHA256

除跟踪文件绑定上述 HEAD 外，生成依赖以如下读取时 hash 固定；hash 不证明运行 pipeline 或 AEC 效果。

| 文件 | SHA256 |
|---|---|
| `firmware/main/hal/board/config.h` | `7e279cc5a16d72aef0c066014ac09032241563f574fc4974a2343490cba8ef01` |
| `firmware/main/hal/board/cores3_audio_codec.cc` | `d00a6dc3ade65e6fcf02615392ad4941f3815b7621f8456231a3dd426337dbb9` |
| `firmware/main/hal/board/stackchan.cc` | `5ad90e7c16e7dff15cd8a62b7ac367755611cdd8e8f739d37df2390a4ff7a522` |
| `firmware/xiaozhi-esp32/main/audio/processors/afe_audio_processor.cc` | `f17154c7e5147d6d8535f68d9d1a081ef0d4233ad1ae6c4aa9ede74db1a908f4` |
| `firmware/xiaozhi-esp32/main/audio/processors/afe_audio_processor.h` | `4b01ad1505a487cdd8221d07cd41af89f39fa2ce602c4dca480cc58a6fb56b78` |
| `firmware/xiaozhi-esp32/main/audio/audio_service.cc` | `83a184cc61d703763c480692e0118a5e8e39895e5b3adb64b27d7a4270a1276a` |
| `firmware/xiaozhi-esp32/main/audio/audio_service.h` | `cd2259d42568d3b9f0852cce21c1cec39f4f378a3ba04a25f54c2c675220773c` |
| `firmware/xiaozhi-esp32/main/audio/audio_codec.cc` | `2dafab53ce88f6aa66ff0cc51eb23da73dac8f034276636c3cf491e304443072` |
| `firmware/managed_components/espressif__esp-sr/include/esp32s3/esp_afe_config.h` | `c54b542a842806f3352731e7edea536ecacf4ce21b69258ef8ed7920c1b13f8d` |
| `firmware/managed_components/espressif__esp-sr/include/esp32s3/esp_afe_sr_iface.h` | `065855dbfab09740000c16257ec0e3467b83a9c3bfcb301090c38eab355c613e` |
| `firmware/managed_components/espressif__esp-sr/include/esp32s3/esp_afe_aec.h` | `c6af1d3f37e8fce4405e6138122a35e69346384d4562f8eadd674c41af36690a` |
| `firmware/managed_components/espressif__esp-sr/include/esp32s3/esp_aec.h` | `973cc94833f2797ad5434624f08333d31dbb15213f58faf0a9c9ee3db2a5cd23` |
| `firmware/managed_components/espressif__esp-sr/idf_component.yml` | `1f5b3968ae40c85608405c6b0993a74e0cc0f5f0c5a35f0331455b04f01f6551` |
| `firmware/managed_components/espressif__esp_codec_dev/platform/audio_codec_data_i2s.c` | `b30d62d4e3e4a2dbfcdb95c6b137109f88997a19281236c6f8fe16a8f52dc9d7` |
| `firmware/dependencies.lock` | `093178c2556d9fbfc211e504321dd27943a399e71c83ee6863459911c5ffa547` |
| `firmware/main/hal/webrtc/webrtc_m1.cc` | `061c2a398e9d5fe4bae79b592a54e490f7421a0d39f28370e4c3a676ac38a3ef` |
