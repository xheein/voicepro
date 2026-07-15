#pragma once

#include <windows.h>
#include <audioclient.h> // 提供 WASAPI 核心接口，用于音频流控制（播放/停止）
#include <mmdeviceapi.h>  // Multimedia Device API: 用于发现和操作输出设备（扬声器、耳机）
#include <thread>         // C++ 标准线程库
#include <atomic>         // 原子操作库，确保多线程下访问主音量、运行标志等变量的安全性
#include <memory>         // 智能指针库
#include <vector>         // 容器库
#include <string>         // 字符串库
#include <mutex>          // 互斥锁，用于保护音源列表的多线程读写同步
#include "ring_buffer.h"  // 项目自定义的线程安全环形缓冲区

/**
 * 结构体：OutputDevice
 * 作用：封装一个播放设备的信息，用于在 UI 的下拉菜单中显示。
 */
struct OutputDevice {
    std::wstring id;   // 设备的“身份证号”（系统内部的长字符串 ID）
    std::wstring name; // 设备的“友好名称”（例如“Realtek Audio Speakers”）
};

/**
 * 类名：AudioRenderer
 * 功能：音频渲染器，这是整个程序的“混音终端”。
 * 原理：
 * 1. 它开启一个物理扬声器流。
 * 2. 在后台线程中，它从所有的音源（麦克风、各个程序）的 RingBuffer 中取出声音片段。
 * 3. 将这些片段叠加（混音）在一起，应用主音量。
 * 4. 最后把混好的数据推送到真正的扬声器播放出来。
 */
class AudioRenderer {
public:
    /**
     * 构造函数
     * @param device_id 想要用来播放声音的设备 ID
     */
    AudioRenderer(const std::wstring& device_id);
    ~AudioRenderer();

    /**
     * 启动渲染线程
     * @return 启动成功返回 true
     */
    bool start();

    /**
     * 停止播放并释放显存和接口资源
     */
    void stop();

    // 查询当前渲染器是否正在工作
    bool is_running() const { return running_.load(); }
    
    /**
     * 音源管理方法
     */
    // 向混音器的“清单”里加入一个新的音源缓冲区
    void add_source(std::shared_ptr<RingBuffer<float>> source);
    // 从“清单”里移除一个音源
    void remove_source(std::shared_ptr<RingBuffer<float>> source);
    // 清空清单（静音所有音源）
    void clear_sources();
    
    /**
     * 主音量与电平控制
     */
    // 设置“总闸”音量
    void set_master_volume(float vol) { master_volume_.store(vol); }
    float get_master_volume() const { return master_volume_.load(); }
    
    // 获取混音后最终输出的声音大小（峰值电平，用于 UI 指针跳动）
    float get_peak_level() const { return peak_level_.load(); }
    
    /**
     * 静态方法：枚举当前系统中连接的所有播放设备（扬声器、耳机等）
     */
    static std::vector<OutputDevice> enumerate_devices();

private:
    /**
     * 渲染线程：这是程序最核心的循环，负责不停地混音和播放
     */
    void render_thread();

    std::wstring device_id_;                                    // 目标输出设备 ID
    std::vector<std::shared_ptr<RingBuffer<float>>> sources_;   // 正在参与混音的所有音源缓冲区列表
    std::mutex sources_mutex_;                                  // 互斥锁，防止在遍历 sources_ 时被其他线程修改列表
    
    std::thread render_thread_;                                 // 后台消费线程
    std::atomic<bool> running_{false};                          // 引擎运行状态开关
    std::atomic<float> master_volume_{1.0f};                    // 最终输出的总音量缩放倍率
    std::atomic<float> peak_level_{0.0f};                        // 最终混音结果的瞬时峰值

    // --- WASAPI 播放相关指针 ---
    IMMDevice* device_ = nullptr;                               // 音频输出硬件对象
    IAudioClient* audio_client_ = nullptr;                      // 播放流客户端实例
    IAudioRenderClient* render_client_ = nullptr;              // 用于向扬声器缓冲区写入数据的接口
    WAVEFORMATEX* mix_format_ = nullptr;                        // 硬件支持并由我们使用的混合格式（通常是 48kHz, 立体声, 32位浮点）
};
