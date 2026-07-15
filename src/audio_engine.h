#pragma once // 确保头文件只被包含一次，防止重复定义错误

// 引入标准库头文件
#include <memory>  // 提供智能指针支持（unique_ptr, shared_ptr）
#include <vector>  // 提供动态数组容器支持
#include <string>  // 提供字符串类支持
#include <mutex>   // 提供互斥锁支持，用于多线程同步

// 引入项目内部的其他组件头文件
#include "audio_session.h"      // 音频会话枚举
#include "process_loopback.h"   // 进程音频采集逻辑
#include "microphone_capture.h"  // 麦克风音频采集逻辑
#include "audio_renderer.h"     // 音频输出逻辑
#include "ring_buffer.h"        // 线程安全的音频数据循环缓冲区

/**
 * 音频源类型枚举
 * 用于区分当前音源是来自于硬件麦克风还是来自于某个应用程序的音频流
 */
enum class SourceType {
    Microphone,  // 麦克风输入
    Application  // 应用程序采集（例如音乐播放器、浏览器等）
};

/**
 * 音频源结构体
 * 封装了一个音源的所有信息，包括它的采集器、缓冲区以及音量状态
 */
struct AudioSource {
    SourceType type;            // 音源类型（麦克风或应用）
    std::wstring name;          // 显示在界面上的名字（如“系统声音”或“QQ音乐”）
    std::wstring id;            // 唯一标识符（对于麦克风是设备 ID，对于应用是会话标识）
    DWORD process_id = 0;       // 如果是应用类型，存储该应用的进程 ID

    /**
     * 环形缓冲区 (Ring Buffer)
     * 作用：作为采集端和混音端的“中转站”。
     * 采集线程往里写数据，混音线程从中读数据。
     * 使用 float 类型是因为 WASAPI 处理音频时通常使用 32 位浮点数。
     */
    std::shared_ptr<RingBuffer<float>> buffer;

    // 两个采集器的指针。根据 SourceType 实例化其中之一。
    std::unique_ptr<ProcessLoopbackCapture> app_capture; // 负责从特定进程抓取音频
    std::unique_ptr<MicrophoneCapture> mic_capture;     // 负责从麦克风硬件录入音频

    float volume = 1.0f;        // 该音源的增益系数（音量倍数），默认 1.0 (原声)
    float peak_level = 0.0f;     // 瞬时峰值电平，用于驱动 UI 上的音量跳动条
};

/**
 * 音频核心引擎类 (AudioEngine)
 * 这是整个音频系统的“司令部”。它管理所有的音源，控制采集的开始和停止，
 * 并负责将多个音源混合后发送给播放设备。
 */
class AudioEngine {
public:
    AudioEngine();  // 构造函数
    ~AudioEngine(); // 析构函数，负责清理资源

    /**
     * 初始化音频引擎
     * 准备 COM 环境、枚举器等基础组件。
     * @return 成功返回 true，失败返回 false
     */
    bool initialize();

    /**
     * 彻底关闭引擎并清理所有已分配的资源
     */
    void shutdown();

    // --- 设备与会话管理接口 ---

    /**
     * 获取系统中所有可用的麦克风列表
     */
    std::vector<MicrophoneDevice> get_microphones();

    /**
     * 获取系统中所有可用的播放（输出）设备列表
     */
    std::vector<OutputDevice> get_output_devices();

    /**
     * 获取当前系统中正在播放声音的应用程序列表
     */
    std::vector<AudioSessionInfo> get_audio_sessions();

    /**
     * 重新扫描系统中的音频会话，刷新列表
     */
    void refresh_sessions();

    // --- 音频源控制接口 ---

    /**
     * 向引擎中添加一个麦克风作为音源
     * @param device_id 麦克风设备的唯一 ID
     * @param name      显示给用户的名称
     */
    bool add_microphone(const std::wstring& device_id, const std::wstring& name);

    /**
     * 向引擎中添加一个应用程序作为音源
     * @param process_id 目标进程的 ID
     * @param name       显示的名称
     */
    bool add_application(DWORD process_id, const std::wstring& name);

    /**
     * 从列表中移除一个已存在的音源
     * @param index 音源在列表中的索引
     */
    void remove_source(size_t index);

    /**
     * 获取当前添加的所有音源列表的引用
     */
    std::vector<AudioSource>& get_sources() { return sources_; }

    // --- 音量与电平控制 ---

    /**
     * 调整特定音源的音量大小
     * @param index  音源索引
     * @param volume 音量值 (0.0 到 1.0)
     */
    void set_source_volume(size_t index, float volume);

    /**
     * 设置总输出音量
     */
    void set_master_volume(float volume);

    /**
     * 获取当前设定的总输出音量
     */
    float get_master_volume() const;

    /**
     * 获取混音后的总输出瞬时峰值电平（用于主电平表）
     */
    float get_master_peak_level() const;

    // --- 引擎生命周期控制 ---

    /**
     * 启动音频引擎：开始从音源采集数据并向输出设备播放。
     * @param output_device_id 用户选择的输出设备 ID
     */
    bool start(const std::wstring& output_device_id);

    /**
     * 停止采集和播放
     */
    void stop();

    /**
     * 检查引擎目前是否正在运行（采集/播放中）
     */
    bool is_running() const;

    /**
     * 触发实时电平更新。
     * 该方法会通知所有采集器计算最新的峰值，并由 UI 轮询读取。
     */
    void update_peak_levels();

private:
    AudioSessionEnumerator session_enumerator_; // 用于寻找系统中正在发声程序的组件
    std::vector<AudioSource> sources_;           // 存储当前用户添加的所有音源
    std::unique_ptr<AudioRenderer> renderer_;   // 实际负责混音并推送到声卡的播放器模块

    std::mutex sources_mutex_;   // 线程锁：防止在混音的同时添加/删除音源导致程序崩溃
    bool running_ = false;       // 标识引擎是否已开启采集和播放
    bool initialized_ = false;   // 标识引擎是否已成功初始化
};
