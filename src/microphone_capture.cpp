#include "microphone_capture.h"
#include <initguid.h>
#include <devpkey.h>  // 设备属性键定义，用于获取像“名称”这样的硬件信息
#include <cmath>

/**
 * 本地定义 PKEY_Device_FriendlyName
 * PROPERTYKEY 是一种用于标识设备属性（如友好名称、制造商等）的结构。
 * 虽然系统头文件有定义，但在某些精简版编译器环境下，手动定义可以增加兼容性。
 */
static const PROPERTYKEY LOCAL_PKEY_Device_FriendlyName = 
    { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

/**
 * 定义 IEEE 浮点数的 GUID
 * GUID (全局唯一标识符) 用于在 Windows 中标识某种特定的音频格式（32位浮点型）。
 */
static const GUID LOCAL_MIC_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = 
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

// 构造函数：初始化设备 ID 和中转缓冲区
MicrophoneCapture::MicrophoneCapture(const std::wstring& device_id, std::shared_ptr<RingBuffer<float>> buffer)
    : device_id_(device_id), buffer_(buffer) {}

// 析构函数：确保对象销毁时，采集线程也跟着安全结束
MicrophoneCapture::~MicrophoneCapture() {
    stop();
}

/**
 * 静态工具方法：找出当前电脑上插着的所有麦克风
 */
std::vector<MicrophoneDevice> MicrophoneCapture::enumerate_devices() {
    std::vector<MicrophoneDevice> devices;
    
    // 1. 创建 WASAPI 设备枚举器 (IMMDeviceEnumerator)
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL, // 指定 COM 对象的运行上下文，这里表示所有可用的上下文
        __uuidof(IMMDeviceEnumerator),
        (void**)&enumerator
    );
    
    if (FAILED(hr) || !enumerator) {
        return devices; // 如果 COM 创建失败，返回空列表
    }
    
    // 2. 获取所有的“录制” (eCapture) 且处于“激活” (DEVICE_STATE_ACTIVE) 状态的端点
    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) {
        enumerator->Release();
        return devices;
    }
    
    // 3. 询问集合中有多少个设备
    UINT count = 0;
    collection->GetCount(&count);
    
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        // 逐个取出设备
        if (SUCCEEDED(collection->Item(i, &device)) && device) {
            MicrophoneDevice mic_device;
            
            // 获取设备的唯一长字符串 ID（比如 "{0.0.1.00000000}.{...}"）
            LPWSTR device_id = nullptr;
            if (SUCCEEDED(device->GetId(&device_id)) && device_id) {
                mic_device.id = device_id;
                CoTaskMemFree(device_id); // 归还 Windows 分配的内存
            }
            
            /**
             * 获取设备的“友好名称”（如 "Realtek High Definition Audio"）
             * 这需要打开设备的“属性库” (Property Store)。
             */
            IPropertyStore* props = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
                PROPVARIANT var_name;
                PropVariantInit(&var_name); // 初始化这个万能包装容器
                // 根据友名对应的 KEY 去查找值
                if (SUCCEEDED(props->GetValue(LOCAL_PKEY_Device_FriendlyName, &var_name))) {
                    if (var_name.vt == VT_LPWSTR && var_name.pwszVal) {
                        mic_device.name = var_name.pwszVal;
                    }
                    PropVariantClear(&var_name); // 清空并释放 PROPVARIANT 内部的内存
                }
                props->Release(); // 释放属性库接口
            }
            
            // 如果 ID 和 名字都拿到了，就存入我们的数组
            if (!mic_device.id.empty() && !mic_device.name.empty()) {
                devices.push_back(mic_device);
            }
            
            device->Release(); // 释放该设备对象
        }
    }
    
    collection->Release(); // 释放设备集合
    enumerator->Release(); // 释放枚举器
    
    return devices;
}

/**
 * 启动采集任务
 */
bool MicrophoneCapture::start() {
    if (running_.load()) return true; // 如果已经跑了，不重复启动
    
    running_.store(true);
    // std::thread 会在后台创建一个新线程，并在新线程中运行 capture_thread 方法
    capture_thread_ = std::thread(&MicrophoneCapture::capture_thread, this);
    return true;
}

/**
 * 停止采集任务
 */
void MicrophoneCapture::stop() {
    running_.store(false); // 通知后台死循环该停止了
    
    // 等待后台线程执行完最后的代码并安全关闭
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    
    // 按顺序释放之前申请的所有 COM 资源（引用计数减 1）
    if (capture_client_) {
        capture_client_->Release();
        capture_client_ = nullptr;
    }
    if (audio_client_) {
        audio_client_->Release();
        audio_client_ = nullptr;
    }
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
    // 也要释放音频格式结构体的内存
    if (mix_format_) {
        CoTaskMemFree(mix_format_);
        mix_format_ = nullptr;
    }
}

/**
 * 麦克风采集线程核心函数（后台运行）
 * 该线程负责初始化 WASAPI 接口，并循环从麦克风硬件拉取音频数据。
 */
void MicrophoneCapture::capture_thread() {
    // 必须要为每一个新线程初始化 COM (Component Object Model) 库，以便使用 WASAPI 接口
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); // COINIT_MULTITHREADED 表示多线程单元
    
    // 1. 再次通过枚举器找到我们指定的那个硬件设备
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr) || !enumerator) {
        CoUninitialize(); // COM 初始化后，即使失败也要清理
        running_.store(false);
        return;
    }
    
    // 根据长字符串 ID 锁定具体设备
    hr = enumerator->GetDevice(device_id_.c_str(), &device_);
    enumerator->Release(); // 枚举器用完即可释放
    
    if (FAILED(hr) || !device_) {
        CoUninitialize();
        running_.store(false);
        return;
    }
    
    // 2. 激活音频客户端接口 (IAudioClient)
    // 这是 WASAPI 的核心接口，用于管理音频流
    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audio_client_);
    if (FAILED(hr) || !audio_client_) {
        CoUninitialize();
        running_.store(false);
        return;
    }
    
    // 3. 询问该麦克风目前的工作采样格式（由驱动决定）
    // WASAPI 会提供一个推荐的格式，我们通常直接使用它
    hr = audio_client_->GetMixFormat(&mix_format_);
    if (FAILED(hr)) {
        CoUninitialize();
        running_.store(false);
        return;
    }
    
    /**
     * 4. 初始化音频客户端
     * AUDCLNT_SHAREMODE_SHARED: 使用共享模式，允许其他程序也用这个麦克风。
     * 0: 缓冲区标志，这里没有特殊要求。
     * 10000000: 请求约 1 秒的缓冲区深度 (以 100 纳秒为单位)。
     * 0: 周期性，这里不使用。
     * mix_format_: 使用设备推荐的音频格式。
     * nullptr: 事件句柄，这里不使用事件驱动模式。
     */
    hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, mix_format_, nullptr);
    if (FAILED(hr)) {
        CoUninitialize();
        running_.store(false);
        return;
    }
    
    // 5. 获取具体负责把数据“掏出来”的采集服务 (IAudioCaptureClient)
    // 这个接口用于从音频缓冲区中读取数据
    hr = audio_client_->GetService(__uuidof(IAudioCaptureClient), (void**)&capture_client_);
    if (FAILED(hr)) {
        CoUninitialize();
        running_.store(false);
        return;
    }
    
    // 6. 告诉声卡驱动：开始录音！
    hr = audio_client_->Start();
    if (FAILED(hr)) {
        CoUninitialize();
        running_.store(false);
        return;
    }
    
    /**
     * 这里进入真正的采集死循环
     * 线程会一直运行，直到 `running_` 标志被设置为 false。
     */
    while (running_.load()) {
        // 由于没有使用事件驱动模式，这里使用简单的睡眠轮询 (10ms 查一次)
        // 这种方式简单但效率略低，对于实时性要求不高的应用足够。
        Sleep(10);
        
        UINT32 packet_length = 0;
        // 询问 Windows：现在驱动里有多少采样点已经录好了？
        // packet_length 表示可用的帧数。
        while (SUCCEEDED(capture_client_->GetNextPacketSize(&packet_length)) && packet_length > 0) {
            BYTE* data = nullptr;
            UINT32 frames_available = 0;
            DWORD flags = 0;
            
            // 获取这一包音频数据的内存指针
            // data 指向原始 PCM 数据，frames_available 是可用的帧数。
            hr = capture_client_->GetBuffer(&data, &frames_available, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;
            
            // 检查是不是静音包 (AUDCLNT_BUFFERFLAGS_SILENT)
            // 如果是静音包，通常表示麦克风被静音或没有输入。
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                // 如果录到了声音，进行格式处理并计算它的分贝（电平）
                process_audio_buffer(data, frames_available, mix_format_);
            } else {
                // 如果是静音，往 RingBuffer 里塞一堆 0，保持时间轴不乱
                // 这样可以确保即使没有声音，音频流的连续性也不会中断。
                if (frames_available > 0) {
                    std::vector<float> silence(frames_available, 0.0f);
                    buffer_->push(silence.data(), frames_available);
                }
            }
            
            // 告诉 Windows 我们用完了这块数据内存，系统可以回收去录下一段了
            capture_client_->ReleaseBuffer(frames_available);
        }
    }
    
    // 退出循环后停止硬件录音
    audio_client_->Stop();
    CoUninitialize(); // 清理 COM 环境
}

/**
 * 音频数据格式处理逻辑
 * 哪怕是录音，Windows 返回的也可能是 32位整型、16位整型或32位浮点数。
 * 我们需要统一转化成 float，方便之后进行混音和波形显示。
 *
 * @param data 指向原始 PCM 数据的指针
 * @param frames 当前缓冲区中的帧数
 * @param format 音频数据的 WAVEFORMATEX 格式描述
 */
void MicrophoneCapture::process_audio_buffer(BYTE* data, UINT32 frames, WAVEFORMATEX* format) {
    if (!data || !format || frames == 0) return;
    
    float vol = volume_.load();        // 用户通过滑块设置的增强倍率
    int channels = format->nChannels;   // 麦克风通常只有 1 或者 2 声道
    std::vector<float> mono_samples(frames); // 暂存单声道 float 结果
    float peak = 0.0f;                  // 寻找这包数据里的最高点（瞬时音量）
    
    // 判断数据格式是不是浮点数
    // WASAPI 可能返回 WAVE_FORMAT_IEEE_FLOAT 或 WAVE_FORMAT_EXTENSIBLE (其中 SubFormat 是 IEEE_FLOAT)
    bool is_float = false;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE* ext = (WAVEFORMATEXTENSIBLE*)format;
        is_float = IsEqualGUID(ext->SubFormat, LOCAL_MIC_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    } else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        is_float = true;
    }
    
    /**
     * 遍历这一组采样的所有帧
     * 一帧包含所有声道在同一时间点的采样数据。
     */
    for (UINT32 i = 0; i < frames; ++i) {
        float sample_sum = 0.0f;
        
        // 把所有声道的音量加起来（取平均值），实现混缩
        // 这样多声道音频也能统一处理成单声道。
        for (int ch = 0; ch < channels; ++ch) {
            float sample = 0.0f;
            
            // 根据不同的位深和格式进行数据转换
            if (is_float && format->wBitsPerSample == 32) {
                // 32位浮点数，直接读取
                sample = ((float*)data)[i * channels + ch];
            } else if (format->wBitsPerSample == 16) {
                // 16位整数，转换为 [-1.0, 1.0] 范围的浮点数
                sample = ((short*)data)[i * channels + ch] / 32768.0f;
            } else if (format->wBitsPerSample == 32) {
                // 32位整数，转换为 [-1.0, 1.0] 范围的浮点数
                sample = ((int*)data)[i * channels + ch] / 2147483648.0f;
            }
            
            sample_sum += sample;
        }
        
        // 缩放并存储
        // 将多声道平均值应用音量增益，得到最终的单声道浮点采样。
        float mono = (sample_sum / channels) * vol;
        // 软膝限幅器（Soft Knee Limiter）：
        // - 信号绝对值 <= 1.0：完全线性放大，零失真
        // - 信号绝对值  > 1.0：仅对超出部分平滑压缩，避免硬削波失真
        if (mono > 1.0f) {
            mono = 1.0f + std::tanh(mono - 1.0f) * 0.1f;
        } else if (mono < -1.0f) {
            mono = -1.0f - std::tanh(-mono - 1.0f) * 0.1f;
        }
        mono_samples[i] = mono;
        
        // 更新峰值，用于驱动界面上的绿/红音量条
        // 寻找当前处理的这批数据中的最大绝对值，代表瞬时音量。
        float abs_sample = std::fabs(mono);
        if (abs_sample > peak) peak = abs_sample;
    }
    
    // 实时平滑电平值
    // 峰值衰减算法，使音量条的下降看起来更自然，而不是瞬间归零。
    float current_peak = peak_level_.load();
    if (peak > current_peak) {
        peak_level_.store(peak); // 瞬间感应爆音，快速响应音量上升
    } else {
        peak_level_.store(current_peak * 0.92f + peak * 0.08f); // 优雅衰落动画，缓慢下降
    }
    
    // 将采集到的单声道音频推送到中转站
    // 渲染线程会自动从这里取走声音并输出到你的喇叭/耳机
    buffer_->push(mono_samples.data(), frames);
}
