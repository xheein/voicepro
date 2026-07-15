# VoicePro


## English

### [[preparation]]

1. Install a virtual audio driver (Recommended: VB-CABLE; after installation, you will see "CABLE Input" and "CABLE Output").

### [[usage]]

1. Set the output device (virtual audio cable) to "CABLE Input".
2. Select your desired microphone and application audio sources as input devices.
3. In games, recorders, or other apps that need audio input, set the "Audio Input" device to "CABLE Output".

### [[others]]

1. Adjust the volume levels for individual inputs.
2. Adjust the master volume level.
3. Click X to remove an app from mixing.

### [[compile]]

1. voicepro> cmake -B build
2. voicepro> cmake --build build --config Release
3. voicepro/build/Release/VoicePro.exe

## 中文

### [[preparation]]

1. 必须安装虚拟音频驱动（推荐: VB-CABLE，安装后会看到 "CABLE Input" 和 "CABLE Output"）

### [[usage]]

1. 输出设备（虚拟音频线）选择"CABLE Input"
2. 输入设备选择要添加的麦克风和应用程序音频
3. 在游戏/录音机等需要声音输出的地方，选择“音频输入”为"CABLE Output"

### [[others]]

1. 可以调整各个输入的音量
2. 可以调整主音量的音量
3. 点击X可以取消该应用的混音

### [[compile]]

1. voicepro> cmake -B build
2. voicepro> cmake --build build --config Release
3. voicepro/build/Release/VoicePro.exe