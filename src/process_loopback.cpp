#include "process_loopback.h"
#include <cmath>
#include <vector>

/**
 * 宏定义与兼容性处理
 * 某些旧版开发工具可能没有定义这些标志，我们手动补齐。
 * 
 * AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM: 这是一个非常实用的标志。
 * 如果采集到的声音格式（采样率等）与我们要求的格式不完全一致，
 * Windows 内部会自动帮我们进行转换，省去了手动写重采样算法的麻烦。
 */
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

/**
 * 构造函数
 * @param process_id 目标程序的 PID
 * @param buffer 用于存放抓取到的音频数据的环形缓冲区
 */
ProcessLoopbackCapture::ProcessLoopbackCapture(DWORD process_id, std::shared_ptr<RingBuffer<float>> buffer)
    : process_id_(process_id), buffer_(buffer) {
    // 创建一个自动重置的 Windows 事件
    // 作用：当音频驱动里积攒了一些数据时，它会触发这个事件，我们的采集线程检测到后就开始干活。
    capture_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

/**
 * 析构函数
 * 确保在采集器对象被删除时，后台抓取线程能干净地退出。
 */
ProcessLoopbackCapture::~ProcessLoopbackCapture() {
    stop(); // 停止线程
    if (capture_event_) CloseHandle(capture_event_); // 关闭系统事件句柄
}

/**
 * 启动采集任务
 */
bool ProcessLoopbackCapture::start() {
    if (running_.load()) return true; // 如果已经在跑了，直接返回
    
    running_.store(true); // 将原子标志设为运行中
    // 开启后台子线程
    // std::thread 会在后台并行运行 capture_thread 方法
    thread_ = std::thread(&ProcessLoopbackCapture::capture_thread, this);
    return true;
}

/**
 * 停止采集任务
 */
void ProcessLoopbackCapture::stop() {
    running_.store(false); // 通知后台线程停止循环
    
    // 如果线程卡在 WaitForSingleObject 等待信号，手动激活它让它醒过来，发现 running 为 false 从而退出。
    if (capture_event_) SetEvent(capture_event_); 
    
    // 等待后台线程执行完最后的清理工作并合并回主线程
    if (thread_.joinable()) {
        thread_.join(); 
    }
    
    // 释放所有的 COM 接口资源
    // 在 Windows 音频编程中，一定要小心管理接口的生命周期
    if (capture_client_) { capture_client_->Release(); capture_client_ = nullptr; }
    if (audio_client_) { audio_client_->Release(); audio_client_ = nullptr; }
    if (format_) { CoTaskMemFree(format_); format_ = nullptr; }
}

/**
 * 后台线程的核心执行函数
 */
void ProcessLoopbackCapture::capture_thread() {
    // 必须在每个线程开始时初始化 COM 环境
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    
    // 核心：调用 try_process_loopback 初始化音频流并启动抓取
    bool success = try_process_loopback();
    
    if (!success) {
        // 如果失败，输出调试信息并结束线程
        WCHAR dbg[256];
        swprintf_s(dbg, L"VoiceM: [错误] 无法启动进程 %lu 的音频抓取任务\n", process_id_);
        OutputDebugStringW(dbg);
        
        CoUninitialize();
        running_.store(false);
        return;
    }
    
    DWORD last_heartbeat = GetTickCount(); // 计时器，用于定期打印心跳包
    UINT64 total_frames = 0;              // 计数器，记录一共抓了多少个采样点
    
    /**
     * 这里是程序最忙的地方：音频拉取循环
     */
    while (running_.load()) {
        // 等待信号。如果 1000 毫秒（1秒）内驱动都没发来声音数据，WaitForSingleObject 会返回超时。
        DWORD wait_result = WaitForSingleObject(capture_event_, 1000); 
        
        if (!running_.load()) break; // 二次检查，确保能及时退出循环

        if (wait_result == WAIT_TIMEOUT) {
            // 如果程序在播音乐却没有数据，打印一条调试心跳。
            if (GetTickCount() - last_heartbeat > 5000) {
                OutputDebugStringW(L"VoiceM: 抓取线程正在等待音频就绪...\n");
                last_heartbeat = GetTickCount();
            }
            continue;
        }
        
        if (wait_result != WAIT_OBJECT_0) break; // 出现非预期错误，强制停掉

        if (!capture_client_) continue;

        // capture_client_ 会维护一个内部缓冲区
        // 我们通过循环拉取直到所有数据包都被处理完
        UINT32 packet_size = 0;
        while (SUCCEEDED(capture_client_->GetNextPacketSize(&packet_size)) && packet_size > 0) {
            BYTE* data = nullptr;   // 原始音频数据指针
            UINT32 frames = 0;      // 本次抓到的采样帧数
            DWORD flags = 0;        // 标志位（如静音标志）
            
            // GetBuffer: 从 Windows 获取该程序发出的原始音频数据的内存地址
            HRESULT hr = capture_client_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;
            
            // 检查这一包声音是不是静音 (Silent)
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                // 如果有内容，累加帧数并处理
                total_frames += frames;
                
                // 定期输出调试信息（有助于开发者排查卡顿问题）
                if (GetTickCount() - last_heartbeat > 2000) {
                    WCHAR fdbg[128];
                    swprintf_s(fdbg, L"VoiceM: [运行中] 进程 %lu - 已抓取 %llu 帧活跃音频\n", process_id_, total_frames);
                    OutputDebugStringW(fdbg);
                    last_heartbeat = GetTickCount();
                }
                
                // 处理缓冲区：转换格式并存入 ring buffer
                process_buffer(data, frames, format_);
            } else {
                // 如果是静音，我们也需要往环形缓冲区填入“静音数据” (填0)
                // 这样做是为了保持时间同步，防止因为丢掉静音帧导致声音播放加速
                if (frames > 0) {
                    std::vector<float> silence(frames, 0.0f);
                    buffer_->push(silence.data(), frames);
                }
                
                // 衰减峰值电平，让界面上的音量条慢慢落下来，而不是瞬间静止
                float current = peak_level_.load();
                peak_level_.store(current * 0.95f);
            }
            
            // GetBuffer 对应的“归还”操作：告诉驱动我们已经读完这一块内存了，你可以重用了
            capture_client_->ReleaseBuffer(frames);
        }
    }
    
    // 退出前停止音频流
    if (audio_client_) audio_client_->Stop();
    CoUninitialize(); // 退出 COM 环境
}

/**
 * 辅助函数：获取当前系统主输出（扬声器）的默认格式
 * 有时程序自身的音频格式很奇怪，我们需要参考系统默认格式来强制初始化。
 */
HRESULT GetSystemMixFormat(WAVEFORMATEX** ppFormat) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return hr;

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) return hr;

    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(hr)) return hr;

    return client->GetMixFormat(ppFormat);
}

/**
 * 进程音频抓取的初始化逻辑
 * 使用了 Windows 10/11 后期加入的“进程级采集”功能。
 */
bool ProcessLoopbackCapture::try_process_loopback() {
    WCHAR dbg[1024];
    swprintf_s(dbg, L"VoiceM: 正在为进程 [PID %lu] 初始化进程级环回流...\n", process_id_);
    OutputDebugStringW(dbg);
    
    // 动态获取函数：这个 API 并不在老旧的 Windows 10 版本中存在
    HMODULE mmdevapi = GetModuleHandleW(L"mmdevapi.dll");
    if (!mmdevapi) mmdevapi = LoadLibraryW(L"mmdevapi.dll");
    if (!mmdevapi) return false;
    
    auto pfn = (PFN_ActivateAudioInterfaceAsync)GetProcAddress(mmdevapi, "ActivateAudioInterfaceAsync");
    if (!pfn) return false;
    
    /**
     * 设置采集参数：告诉 Windows 我们要采集的是“进程回环”
     */
    AUDIOCLIENT_ACTIVATION_PARAMS params = {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = process_id_; // 你想要采集的程序 PID
    // 包含整个进程树（比如如果你采集 Chrome，它所有标签页进程的声音都会被抓到）
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    
    // PROPVARIANT: Windows API 中常用的一种复杂结构体封装容器，用于传递不同类型的参数
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_BLOB; // 二进制块类型
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = (BYTE*)&params;
    
    // CompletionHandler: 我们之前在头文件定义的那个回调处理类
    ComPtr<CompletionHandler> handler = Make<CompletionHandler>();
    if (!handler) return false;

    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    
    // 异步激活音频接口
    HRESULT hr = pfn(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, handler.Get(), op.GetAddressOf());
    if (FAILED(hr)) return false;
    
    // 由于激活是异步的（不在当前行立刻完成），我们必须等回调信号或是超时
    if (WaitForSingleObject(handler->GetEvent(), 5000) == WAIT_OBJECT_0) {
        hr = handler->GetResult(&audio_client_); // 成功后从 handler 中取出 IAudioClient 实例
    } else {
        hr = E_ABORT; // 5秒还没反应，说明超时了
    }
    
    if (FAILED(hr) || !audio_client_) return false;
    
    bool used_fallback_format = false;
    // GetMixFormat: 询问该程序的音频采样率是多少（比如 44100Hz 还是 48000Hz）
    hr = audio_client_->GetMixFormat(&format_);
    if (FAILED(hr) || !format_) {
        // 如果失败，尝试用系统默认格式强制覆盖（fallback）
        hr = GetSystemMixFormat(&format_);
        if (SUCCEEDED(hr) && format_) {
            used_fallback_format = true;
        } else {
            return false;
        }
    }
    
    /**
     * 设置流标志位
     * LOOPBACK: 我们是采集播放出的声音，不是麦克风录音
     * EVENTCALLBACK: 告诉 Windows 有新数据时通过事件通知我，而不是让我自己去不停轮询查
     */
    DWORD stream_flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    
    // 如果用了后备格式，必须开启自动转换，否则驱动会因为格式不匹配而拒绝 Initialize。
    if (used_fallback_format) {
        stream_flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
    }

    // 初始化音频流
    // 这里将缓冲区设为约 50 毫秒（延迟很低，适合实时处理）
    hr = audio_client_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        stream_flags,
        50000000, 0, format_, nullptr
    );

    if (FAILED(hr)) return false;
    
    // 关键点：将我们的 capture_event_ 句柄传给 Windows
    // 每当驱动层有了新声音，Windows 就会立刻唤起我们的后台线程。
    hr = audio_client_->SetEventHandle(capture_event_);
    if (FAILED(hr)) return false;

    // 获取实际读取数据的服务接口
    hr = audio_client_->GetService(__uuidof(IAudioCaptureClient), (void**)&capture_client_);
    if (FAILED(hr)) return false;
    
    // 全部准备就绪，命令音频引擎：开始干活！
    hr = audio_client_->Start();
    if (FAILED(hr)) return false;
    
    OutputDebugStringW(L"VoiceM: 音频流启动成功，开始采集进程声音。\n");
    return true;
}

/**
 * 备选方案（目前已禁用）
 */
bool ProcessLoopbackCapture::fallback_device_loopback() {
    // 禁用录制整个电脑系统声音的备选方案。
    return false;
}

/**
 * 处理缓冲区中的原始音频数据
 * 这个函数非常重要，它负责把“乱七八糟”的原始数据变成程序可理解的 float 声音。
 * 
 * @param data 原始字节数组
 * @param frames 本次数据块中点的数量
 * @param format 数据块的格式（由音频驱动提供）
 */
void ProcessLoopbackCapture::process_buffer(BYTE* data, UINT32 frames, WAVEFORMATEX* format) {
    if (!data || !format || frames == 0) return;
    
    float vol = volume_.load();        // 当前音量倍率
    int channels = format->nChannels;   // 通道数（比如 2 是立体声，左+右）
    std::vector<float> mono(frames);    // 准备一个临时的数组，用来存放混缩后的单声道数据
    float peak = 0.0f;                  // 本次数据中的最大振幅
    
    // 这个 GUID 用于判断数据是不是浮点数。WASAPI 会返回各种格式（16位整数、32位整数、32位浮点数）。
    bool is_float = (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE* ext = (WAVEFORMATEXTENSIBLE*)format;
        is_float = IsEqualGUID(ext->SubFormat, LOCAL_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    
    /**
     * 遍历每一个采用点（帧）进行格式转换。
     * 这里的逻辑是把多声道（Stereo）根据采样值相加后平均，合并成一个单声道（Mono）。
     */
    for (UINT32 i = 0; i < frames; ++i) {
        float sum = 0.0f;
        for (int ch = 0; ch < channels; ++ch) {
            float sample = 0.0f;
            // 情况 A: 32 位浮点数 (常见的现代系统格式)
            if (is_float && format->wBitsPerSample == 32) {
                sample = ((float*)data)[i * channels + ch];
            } 
            // 情况 B: 16 位短整数 (CD 品质原始数据)
            else if (format->wBitsPerSample == 16) {
                // 将整数范围 (-32768 to 32767) 映射回 (-1.0 to 1.0)
                sample = ((short*)data)[i * channels + ch] / 32768.0f;
            } 
            // 情况 C: 32 位标准整数
            else if (format->wBitsPerSample == 32) {
                sample = ((int*)data)[i * channels + ch] / 2147483648.0f;
            }
            sum += sample;
        }
        
        // 计算平均值并应用咱们程序中那个"音量滑条"设定的增益
        float m = (sum / channels) * vol;
        // 软膝限幅器（Soft Knee Limiter）：
        // - 信号绝对值 <= 1.0：完全线性放大，零失真，保留所有细节
        // - 信号绝对值  > 1.0：仅对超出部分用 tanh 平滑压缩，不产生方波削波噪音
        // 这样 QQ 音乐等偏小的信号能被有效放大，只有真正过载时才会被保护性压缩。
        if (m > 1.0f) {
            m = 1.0f + std::tanh(m - 1.0f) * 0.1f; // 超出部分压缩到 (1.0, 1.1] 内
        } else if (m < -1.0f) {
            m = -1.0f - std::tanh(-m - 1.0f) * 0.1f; // 负向对称处理
        }
        mono[i] = m; // 存入单声道队列
        
        // 峰值记录逻辑：abs 计算绝对值，找出震幅最大的那一点
        float abs_m = std::fabs(m);
        if (abs_m > peak) peak = abs_m;
    }
    
    // 更新电平值（UI 需要），带一点缓慢下降的“回弹”动画效果
    float current = peak_level_.load();
    if (peak > current) {
        peak_level_.store(peak); // 瞬间冲上去
    } else {
        peak_level_.store(current * 0.9f + peak * 0.1f); // 优雅地降下来
    }

    // 最后，将处理好的成品 float 音频流推进“中转站” RingBuffer
    // 渲染线程会从另一端把这个数据取走并播放。
    buffer_->push(mono.data(), frames);
}
