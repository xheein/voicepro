#include "audio_session.h"
#include <psapi.h>      // Process Status API: 用于查询进程信息的 Windows API
#include <tlhelp32.h>   // Tool Help Library: 用于获取系统进程快照的工具库

/**
 * #pragma comment(lib, "psapi.lib")
 * 这是一条编译器指令，告诉链接器在构建程序时自动包含 psapi.lib 静态库。
 * 这样做就不用在 CMake 或项目设置里手动添加这个库了。
 */
#pragma comment(lib, "psapi.lib")

// 构造函数
AudioSessionEnumerator::AudioSessionEnumerator() {}

/**
 * 析构函数
 * 在 Windows COM 编程中，所有以 'I' 开头的接口（如 IMMDeviceEnumerator）
 * 都是引用计数的。当你不再使用它们时，必须调用 Release() 来减少引用计数，
 * 否则会导致严重的内存泄漏（程序占用的内存不断增加）。
 */
AudioSessionEnumerator::~AudioSessionEnumerator() {
    if (device_enumerator_) {
        device_enumerator_->Release(); // 释放设备枚举器接口
    }
}

/**
 * 初始化方法
 */
bool AudioSessionEnumerator::initialize() {
    if (initialized_) return true; // 如果已经初始化过，就不再重复操作

    /**
     * CoCreateInstance: COM 的核心函数，用于创建一个组件对象。
     * 你可以把它想象成 C++ 中的 'new'，但它是跨语言、跨组件的。
     * 
     * __uuidof(MMDeviceEnumerator): 请求创建“多媒体设备枚举器”这个类。
     * CLSCTX_ALL: 在任何上下文中运行（进程内、进程外等）。
     * __uuidof(IMMDeviceEnumerator): 得到的对象要符合这个接口的规范。
     * &device_enumerator_: 将创建好的对象地址存入我们的类成员变量中。
     */
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**)&device_enumerator_
    );

    /**
     * HRESULT 是 COM 函数的通用返回值类型。
     * SUCCEEDED 宏用于检查结果是否为“成功”（即大于等于 0）。
     */
    initialized_ = SUCCEEDED(hr);
    return initialized_;
}

/**
 * 根据进程 ID 获取该进程的 exe 文件名
 */
std::wstring AudioSessionEnumerator::get_process_name(DWORD pid) {
    std::wstring name;
    
    /**
     * CreateToolhelp32Snapshot: 获取当前系统所有运行中进程的一个“快照”（就像拍了一张照片）。
     * TH32CS_SNAPPROCESS: 表示我们要拍的是进程列表的照片。
     */
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    
    // 检查快照句柄是否有效
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe;           // 用于存储单个进程信息的结构体
        pe.dwSize = sizeof(pe);       // 使用前必须设置结构体大小，这是 Windows API 的通用要求
        
        // 获取快照中的第一个进程信息
        if (Process32FirstW(snapshot, &pe)) {
            do {
                // 如果发现这个进程的 ID (dwProcessId) 正是我们想要的 (pid)
                if (pe.th32ProcessID == pid) {
                    name = pe.szExeFile; // 记录下它的可执行文件名（如 "Spotify.exe"）
                    break;
                }
                // 继续寻找下一个进程
            } while (Process32NextW(snapshot, &pe));
        }
        
        /**
         * CloseHandle: 在 Windows 中，所有的 HANDLE（句柄）使用完后都必须关闭，
         * 否则会造成内核资源泄漏。
         */
        CloseHandle(snapshot);
    }
    
    return name;
}

/**
 * 枚举所有音频会话的核心逻辑
 */
std::vector<AudioSessionInfo> AudioSessionEnumerator::enumerate_sessions() {
    std::vector<AudioSessionInfo> sessions;
    
    // 如果没有初始化成功，直接返回空的列表
    if (!initialized_ || !device_enumerator_) {
        return sessions;
    }

    /**
     * 1. 获取默认的音频渲染端点（通常是你的默认扬声器）
     * eRender: 表示输出设备。
     * eConsole: 表示标准输出设备（相对于通讯设备）。
     */
    IMMDevice* device = nullptr;
    HRESULT hr = device_enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device) {
        return sessions;
    }

    /**
     * 2. 激活音频会话管理器 (IAudioSessionManager2)
     * 这个接口专门负责管理一个端点（设备）上的所有音频会话。
     */
    IAudioSessionManager2* session_manager = nullptr;
    hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&session_manager);
    if (FAILED(hr) || !session_manager) {
        device->Release(); // 出错也别忘了释放之前获取的 device
        return sessions;
    }

    /**
     * 3. 获取会话枚举器
     * 通过它，我们可以像数数一样一个一个地查看当前有哪些程序在播声音。
     */
    IAudioSessionEnumerator* session_enum = nullptr;
    hr = session_manager->GetSessionEnumerator(&session_enum);
    if (FAILED(hr) || !session_enum) {
        session_manager->Release();
        device->Release();
        return sessions;
    }

    /**
     * 4. 遍历所有会话
     */
    int session_count = 0;
    session_enum->GetCount(&session_count); // 获取当前一共有多少个音频会话

    for (int i = 0; i < session_count; ++i) {
        IAudioSessionControl* session_control = nullptr;
        // 获取第 i 个会话的控制对象
        if (SUCCEEDED(session_enum->GetSession(i, &session_control))) {
            
            /**
             * session_control 只能做简单的控制。
             * 我们需要它的“高级版本” IAudioSessionControl2 来获取进程 ID 等详细信息。
             * QueryInterface 是 COM 接口转换的标准方法。
             */
            IAudioSessionControl2* session_control2 = nullptr;
            if (SUCCEEDED(session_control->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&session_control2))) {
                
                DWORD pid = 0;
                session_control2->GetProcessId(&pid); // 询问：这个音频流是谁产生的？
                
                // pid 为 0 通常代表系统服务的音频，或者是已经退出的程序，我们要找的是真实正在运行的程序
                if (pid > 0) {
                    AudioSessionInfo info;
                    info.process_id = pid;
                    // 通过 PID 换取程序的名字
                    info.executable_name = get_process_name(pid);
                    
                    /**
                     * 获取会话的显示名称
                     * COM 使用专用的 LPWSTR (宽字符串指针)。
                     */
                    LPWSTR display_name = nullptr;
                    if (SUCCEEDED(session_control->GetDisplayName(&display_name)) && display_name) {
                        // 如果程序自己设置了名字，就用它的名字
                        if (wcslen(display_name) > 0) {
                            info.name = display_name;
                        } else {
                            // 否则就用它的名字（如 "music.exe"）作为显示名
                            info.name = info.executable_name;
                        }
                        
                        /**
                         * CoTaskMemFree: 这是 COM 特有的内存释放函数。
                         * Windows 分配给你的字符串，必须用这个函数还给 Windows。
                         */
                        CoTaskMemFree(display_name);
                    } else {
                        info.name = info.executable_name;
                    }
                    
                    /**
                     * 获取会话的唯一标识符字符串
                     */
                    LPWSTR session_id = nullptr;
                    if (SUCCEEDED(session_control2->GetSessionIdentifier(&session_id)) && session_id) {
                        info.id = session_id;
                        CoTaskMemFree(session_id);
                    }
                    
                    // 如果信息收集完整，存入列表
                    if (!info.name.empty()) {
                        sessions.push_back(info);
                    }
                }
                session_control2->Release(); // 释放高级接口
            }
            session_control->Release(); // 释放基础接口
        }
    }

    /**
     * 5. 释放获取的所有顶级 COM 接口（顺序与获取时相反）
     */
    session_enum->Release();
    session_manager->Release();
    device->Release();

    return sessions;
}
