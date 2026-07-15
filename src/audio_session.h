#pragma once // 防止头文件被重复包含

// 引入 Windows 系统核心音频接口定义的头文件
#include <windows.h>
#include <mmdeviceapi.h>  // Multimedia Device API: 用于枚举系统的音频硬件设备（如扬声器、麦克风）
#include <audiopolicy.h>  // Audio Policy API: 用于管理音频会话（每个发声的程序都是一个会话）
#include <string>         // C++ 标准字符串类
#include <vector>         // C++ 标准动态数组容器

/**
 * 结构体：AudioSessionInfo
 * 作用：保存一个正在播放声音的应用程序的相关信息。
 */
struct AudioSessionInfo {
    std::wstring name;            // 会话的“友好名称”（例如“网页浏览器”），有些程序不设置此值
    std::wstring id;              // Windows 给这个音频会话分配的唯一内部 ID
    DWORD process_id;             // 产生声音的那个程序的进程 ID (PID)
    std::wstring executable_name; // 程序的实际文件名（如 "chrome.exe" 或 "cloudmusic.exe"）
};

/**
 * 类：AudioSessionEnumerator
 * 作用：这个类的任务是去询问 Windows 系统：“嘿，现在有哪些程序正在通过我的声卡播音乐？”
 * 它使用 WASAPI (Windows Audio Session API) 技术来实现。
 */
class AudioSessionEnumerator {
public:
    AudioSessionEnumerator();  // 构造函数
    ~AudioSessionEnumerator(); // 析构函数

    /**
     * 初始化枚举器
     * 在 Windows 中使用音频功能前，通常需要先获取一个名为 IMMDeviceEnumerator 的核心接口。
     * @return 成功返回 true，失败返回 false
     */
    bool initialize();
    
    /**
     * 执行扫描并返回当前所有音频会话列表
     * 这个方法会遍历系统中所有的活跃音频输出，找出哪些程序正在占用它们。
     */
    std::vector<AudioSessionInfo> enumerate_sessions();
    
private:
    /**
     * 辅助方法：通过进程 ID (PID) 获取该程序的名称
     * @param pid 进程 ID
     * @return 返回程序的名称字符串（如 "MusicPlayer.exe"）
     */
    std::wstring get_process_name(DWORD pid);
    
    // IMMDeviceEnumerator 是 Windows 定义的一个 COM 接口指针，用于管理音频设备
    IMMDeviceEnumerator* device_enumerator_ = nullptr; 
    bool initialized_ = false; // 记录是否已经成功调用过 initialize()
};
