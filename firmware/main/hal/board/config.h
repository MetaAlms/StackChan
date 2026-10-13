#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

// M5Stack StackChan Board configuration

#include <driver/gpio.h>

// Original vendor setting, restored after measuring that the alternative broke
// the uplink audio.
//
// Declaring a reference makes the AFE build an "MR" input format (one
// microphone plus one reference) and leaves the second ES7210 channel unused.
// The hardware does have two microphones and no speaker loopback, so "MM" with
// dual-microphone beamforming looked like the correct model - but it routes the
// AFE through SE(BSS), blind source separation, and the resulting uplink was
// measurably wrong: the decoded audio peaked at -25 dB with a nearly flat,
// high-frequency-tilted spectrum, whereas intelligible speech concentrates
// energy at 80-1000 Hz. The server transcribed it as "mechanical clicking".
//
// With "MR" the AFE uses the single primary microphone; the second channel is
// simply not consumed.
#define AUDIO_INPUT_REFERENCE    true
#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_0
#define AUDIO_I2S_GPIO_WS GPIO_NUM_33
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_34
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_14
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_13

#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_12
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_11
#define AUDIO_CODEC_AW88298_ADDR AW88298_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR  ES7210_CODEC_DEFAULT_ADDR

#define BUILTIN_LED_GPIO        GPIO_NUM_NC
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC

#define DISPLAY_SDA_PIN GPIO_NUM_NC
#define DISPLAY_SCL_PIN GPIO_NUM_NC
#define DISPLAY_WIDTH   320
#define DISPLAY_HEIGHT  240
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false

#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0

#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_NC
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT true



/* Camera pins */
#define CAMERA_PIN_PWDN GPIO_NUM_NC
#define CAMERA_PIN_RESET GPIO_NUM_NC
#define CAMERA_PIN_XCLK  GPIO_NUM_NC // 像素时钟 (固定由 20MHz 外部晶振输入) 
#define CAMERA_PIN_SIOD GPIO_NUM_NC  // 串行时钟 Using existing I2C port
#define CAMERA_PIN_SIOC GPIO_NUM_NC  // 串行时钟 Using existing I2C port
#define CAMERA_PIN_D0 GPIO_NUM_39
#define CAMERA_PIN_D1 GPIO_NUM_40
#define CAMERA_PIN_D2 GPIO_NUM_41
#define CAMERA_PIN_D3 GPIO_NUM_42
#define CAMERA_PIN_D4 GPIO_NUM_15
#define CAMERA_PIN_D5 GPIO_NUM_16
#define CAMERA_PIN_D6 GPIO_NUM_48
#define CAMERA_PIN_D7 GPIO_NUM_47
#define CAMERA_PIN_VSYNC GPIO_NUM_46
#define CAMERA_PIN_HREF GPIO_NUM_38
#define CAMERA_PIN_PCLK GPIO_NUM_45

#define XCLK_FREQ_HZ 20000000

 

#endif // _BOARD_CONFIG_H_
