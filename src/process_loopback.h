#pragma once
#include <windows.h>
#include <thread>
#include <atomic>   // 提供原子操作支持，用于在多线程间安全地读写简单变量（如 bool, float）
#include <memory>
#include "ring_buffer.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/implements.h>  // Window 运行时库 (WRL) 实现 COM 接口的辅助工具
#include <wrl/client.h>
#include <audioclientactivationparams.h> // 包含 WASAPI 的进程启动参数定义 (AUDIOCLIENT_ACTIVATION_PARAMS)

// 使用 WRL 的命名空间，简化代码
using namespace Microsoft::WRL;

/**
 * IEEE_FLOAT 格式定义
 * WASAPI 处理音频时，通常使用 32 位浮点数 (float) 表示声音的大小。
 * 声音范围在 -1.0 到 1.0 之间。
 */
#ifndef KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_DEFINED
#define KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_DEFINED
static const GUID LOCAL_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = 
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
#endif

/**
 * ActivateAudioInterfaceAsync 函数指针
 * 这是 Windows 10/11 提供的一个现代 API，用于异步激活音频接口。
 * 由于它是动态加载的或在较旧 SDK 中可能缺失，这里通过定义函数指针来调用它。
 */
typedef HRESULT (WINAPI *PFN_ActivateAudioInterfaceAsync)(
    LPCWSTR deviceInterfacePath,
    REFIID riid,
    PROPVARIANT* activationParams,
    IActivateAudioInterfaceCompletionHandler* completionHandler,
    IActivateAudioInterfaceAsyncOperation** activationOperation
);

/**
 * 类：CompletionHandler
 * 作用：处理异步激活的结果。
 * 
 * 在使用 ActivateAudioInterfaceAsync 时，由于它是异步的，
 * Windows 会在激活成功或失败后，通过调用这个类的 ActivateCompleted 方法来通知我们。
 */
class CompletionHandler : 
    public RuntimeClass<RuntimeClassFlags<ClassicCom>, FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
    CompletionHandler() {
        // 创建一个 Windows 事件，用于跨线程同步（等待激活完成）
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); 
    }
    
    virtual ~CompletionHandler() {
        if (event_) CloseHandle(event_);
        if (client_) client_->Release();
    }
    
    /**
     * 当激活操作结束时，Windows 自动执行此回调函数。
     */
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* operation) override {
        if (operation) {
            IUnknown* unknown = nullptr;
            // 获取激活的最终结果（HRESULT）和具体的音频客户端接口指针
            hr_ = operation->GetActivateResult(&activate_hr_, &unknown);
            if (SUCCEEDED(hr_) && SUCCEEDED(activate_hr_) && unknown) {
                // 如果成功，将获取到的接口转换（QueryInterface）为所需的 IAudioClient 接口
                unknown->QueryInterface(__uuidof(IAudioClient), (void**)&client_);
                unknown->Release(); // 释放原始指针，我们已经存好转换后的 client_ 了
            }
        }
        // 设置事件信号，通知主线程：事情办完了！
        SetEvent(event_); 
        return S_OK;
    }
    
    /**
     * 辅助方法：让主线程能够取出成功激活的 IAudioClient 接口。
     */
    HRESULT GetResult(IAudioClient** client) {
        if (SUCCEEDED(hr_) && SUCCEEDED(activate_hr_) && client_) {
            *client = client_;
            client_ = nullptr; // 将所有权转交给调用者，防止析构时 Release
            return S_OK;
        }
        return FAILED(hr_) ? hr_ : activate_hr_;
    }
    
    // 获取事件句柄，用于 WaitForSingleObject 等待
    HANDLE GetEvent() const { return event_; }

private:
    HANDLE event_ = nullptr;
    IAudioClient* client_ = nullptr;
    HRESULT hr_ = E_FAIL;         // 获取操作结果本身的成功状态
    HRESULT activate_hr_ = E_FAIL; // 激活音频客户端的具体业务结果
};

/**
 * 类名：ProcessLoopbackCapture
 * 功能：专门负责对“特定进程”产生的音频流进行抓包（采集）。
 * 比如：你只想录制正在运行的“网易云音乐”的声音，而不录制系统的警告音。
 */
class ProcessLoopbackCapture {
public:
    /**
     * 构造函数
     * @param process_id 要抓取音频的目标进程 ID (PID)
     * @param buffer 用于存放抓取到的音频数据的共享循环缓冲区
     */
    ProcessLoopbackCapture(DWORD process_id, std::shared_ptr<RingBuffer<float>> buffer);
    ~ProcessLoopbackCapture();

    /**
     * 开始采集。它会创建一个后台线程，不断从 Windows 驱动中提取音频数据。
     */
    bool start();

    /**
     * 停止采集并清理占用的所有音频接口。
     */
    void stop();

    // 查询当前是否正在运行
    bool is_running() const { return running_.load(); }
    
    // 获取当前的实时声音电平（0.0 到 1.0）
    float get_peak_level() const { return peak_level_.load(); }

    // 设置该捕获源的音量（采集层面音量放大/缩小）
    void set_volume(float vol) { volume_.store(vol); }
    float get_volume() const { return volume_.load(); }

private:
    /**
     * 各类后台私有方法
     */
    void capture_thread();            // 后台线程的主执行逻辑循环
    bool try_process_loopback();      // 核心：使用 Windows 10/11 的进程采集 API
    bool fallback_device_loopback();  // 兜底：如果进程级采集不可用，改为采集整个系统的声音（不常用）
    
    /**
     * 音频处理：将驱动发回的原始 BYTE 数据转换为我们需要的 float 格式，并计算电平
     */
    void process_buffer(BYTE* data, UINT32 frames, WAVEFORMATEX* format);

    DWORD process_id_;                          // 记录目标进程 ID
    std::shared_ptr<RingBuffer<float>> buffer_; // 数据产出的“目的地”
    
    std::thread thread_;                        // 后台音频拉取线程
    std::atomic<bool> running_{false};          // 是否正在运行的开关（线程安全）
    std::atomic<float> volume_{1.0f};           // 当前设定的音量
    std::atomic<float> peak_level_{0.0f};        // 计算出的实时峰值，供 UI 使用

    // --- WASAPI 高级接口：与音频驱动通信的桥梁 ---
    IAudioClient* audio_client_ = nullptr;      // 音频客户端：负责初始化流、格式协商、开始/停止
    IAudioCaptureClient* capture_client_ = nullptr; // 采集客户端：专门负责读取每一块 PCM 数据
    HANDLE capture_event_ = nullptr;            // 通知事件：驱动如果有新数据，会“推”这个事件
    WAVEFORMATEX* format_ = nullptr;            // 描述音频流的格式（采样率、通道数等）
};
