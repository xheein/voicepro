#pragma once // 确保头文件只被包含一次

#include <windows.h>
#include <audioclient.h> // 提供 WASAPI (Windows Audio Session API) 核心接口，用于音频流控制
#include <mmdeviceapi.h>  // Multimedia Device API: 用于发现和通过代码操作声卡、扬声器、麦克风
#include <thread>         // C++ 标准线程库
#include <atomic>         // 原子操作库，确保多线程下访问简单变量（如运行标志）的安全性
#include <memory>         // 智能指针库
#include <string>         // 字符串库
#include <vector>         // 容器库
#include "ring_buffer.h"  // 项目自定义的线程安全环形缓冲区

/**
 * 结构体：MicrophoneDevice
 * 作用：简单封装一个麦克风设备的信息，用于在 UI 列表显示。
 */
struct MicrophoneDevice {
    std::wstring id;   // 设备的“身份证号”（系统内部的长字符串 ID）
    std::wstring name; // 设备的“友好名称”（例如“Realtek Audio Microphone”）
};

/**
 * 类名：MicrophoneCapture
 * 功能：专门负责从麦克风（录音设备）采集原始声音。
 * 原理：它通过 WASAPI 开启一个高优先级采样流，将录到的每一块 PCM 数据推送到 RingBuffer 中。
 */
class MicrophoneCapture {
public:
    /**
     * 构造函数
     * @param device_id 想要开启的麦克风 ID
     * @param buffer    采集到的声音存放的“池子”（环形缓冲区）
     */
    MicrophoneCapture(const std::wstring& device_id, std::shared_ptr<RingBuffer<float>> buffer);
    ~MicrophoneCapture();

    /**
     * 启动采集线程
     * @return 启动成功返回 true
     */
    bool start();

    /**
     * 停止采集并释放硬件资源
     */
    void stop();

    // 查询当前是否正在录音
    bool is_running() const { return running_.load(); }
    
    // 获取当前的实时瞬时声音电平 (0.0 到 1.0)
    float get_peak_level() const { return peak_level_.load(); }

    // 设置软件增益（音量放大系数）
    void set_volume(float vol) { volume_.store(vol); }
    float get_volume() const { return volume_.load(); }
    
    /**
     * 静态方法：枚举当前系统中连接的所有录音设备
     * 所谓“枚举”，就是让 Windows 报个数，并把每个设备的名称和 ID 吐出来。
     */
    static std::vector<MicrophoneDevice> enumerate_devices();

private:
    /**
     * 采集线程：在后台不断死循环拉取数据的函数
     */
    void capture_thread();

    /**
     * 音频处理：将驱动发回的原始 BYTE 数据转换成程序易算的 float 格式，并计算当前电平
     */
    void process_audio_buffer(BYTE* data, UINT32 frames, WAVEFORMATEX* format);

    std::wstring device_id_;                    // 当前使用的设备 ID
    std::shared_ptr<RingBuffer<float>> buffer_; // 数据流向的目标缓冲区
    
    std::thread capture_thread_;                // 后台线程对象
    std::atomic<bool> running_{false};          // 运行状态标志
    std::atomic<float> volume_{1.0f};           // 音量调节系数
    std::atomic<float> peak_level_{0.0f};        // 计算出的实时最大振幅（用于 UI 跳动条）
    
    // --- WASAPI 硬件操作相关指针 ---
    IMMDevice* device_ = nullptr;               // 具体的一个音频设备对象
    IAudioClient* audio_client_ = nullptr;     // 每个采集流都要对应一个“音频客户端”
    IAudioCaptureClient* capture_client_ = nullptr; // 用于从 IAudioClient 实际读取数据的接口
    WAVEFORMATEX* mix_format_ = nullptr;        // 硬件告诉我们的音频格式详情（采样率、位深等）
};
