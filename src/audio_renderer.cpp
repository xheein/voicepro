#include "audio_renderer.h"
#include <cmath>
#include <algorithm> // 包含 std::clamp 等常用算法

/**
 * 本地定义 PKEY_Device_FriendlyName
 * PROPERTYKEY 是一种用于标识设备属性（如友好名称、制造商等）的结构。
 * 虽然系统中可能已有定义，但手动定义可以增强跨编译环境的兼容性。
 */
static const PROPERTYKEY LOCAL_PKEY_Device_FriendlyName =
    { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

/**
 * 定义 IEEE 浮点数的 GUID
 * GUID (全局唯一标识符) 用于在 Windows 中标识某种特定的音频格式。
 */
static const GUID LOCAL_RENDER_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

// 构造函数：记录选定的目标输出设备 ID（比如耳机还是喇叭）
AudioRenderer::AudioRenderer(const std::wstring& device_id)
    : device_id_(device_id) {}

// 析构函数：确保对象销毁时，播放线程被干净停掉
AudioRenderer::~AudioRenderer() {
    stop();
}

/**
 * 静态工具方法：找出电脑上插着的所有扬声器、耳机等设备
 */
std::vector<OutputDevice> AudioRenderer::enumerate_devices() {
    std::vector<OutputDevice> devices;

    // 1. 创建 WASAPI 设备枚举器
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**)&enumerator
    );

    if (FAILED(hr) || !enumerator) {
        return devices; // 如果初始化失败，直接返回空列表
    }

    // 2. 枚举所有活跃的音频播放端点 (eRender)
    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) {
        enumerator->Release();
        return devices;
    }

    // 3. 统计有多少个播放设备并遍历它们
    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        if (SUCCEEDED(collection->Item(i, &device)) && device) {
            OutputDevice out_device;

            // 获取设备的硬件唯一 ID
            LPWSTR device_id = nullptr;
            if (SUCCEEDED(device->GetId(&device_id)) && device_id) {
                out_device.id = device_id;
                CoTaskMemFree(device_id); // 释放 Windows 帮你算的字符串内存
            }

            // 获取设备的友好描述名（如 "Realtek Audio Speakers"）
            IPropertyStore* props = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
                PROPVARIANT var_name;
                PropVariantInit(&var_name);
                if (SUCCEEDED(props->GetValue(LOCAL_PKEY_Device_FriendlyName, &var_name))) {
                    if (var_name.vt == VT_LPWSTR && var_name.pwszVal) {
                        out_device.name = var_name.pwszVal;
                    }
                    PropVariantClear(&var_name);
                }
                props->Release();
            }

            if (!out_device.id.empty() && !out_device.name.empty()) {
                devices.push_back(out_device);
            }

            device->Release();
        }
    }

    collection->Release();
    enumerator->Release();

    return devices;
}

/**
 * 添加音源：将一个采集音源（如麦克风、浏览器声音）的缓冲区加入混音队列
 */
void AudioRenderer::add_source(std::shared_ptr<RingBuffer<float>> source) {
    // std::lock_guard 自动上锁和解锁，确保多线程下修改音源列表安全
    std::lock_guard<std::mutex> lock(sources_mutex_);
    sources_.push_back(source);
}

/**
 * 移除音源：不再播放某个特定的音源
 */
void AudioRenderer::remove_source(std::shared_ptr<RingBuffer<float>> source) {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    // 使用 erase-remove 模板删除指定的指针对象
    sources_.erase(std::remove(sources_.begin(), sources_.end(), source), sources_.end());
}

/**
 * 清空音源：静音所有声音
 */
void AudioRenderer::clear_sources() {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    sources_.clear();
}

/**
 * 启动播放渲染线程
 */
bool AudioRenderer::start() {
    if (running_.load()) return true;

    running_.store(true);
    // 开启后台消费线程，该线程会不停地混音并向声卡塞数据
    render_thread_ = std::thread(&AudioRenderer::render_thread, this);
    return true;
}

/**
 * 停止播放任务
 */
void AudioRenderer::stop() {
    running_.store(false);
    if (render_thread_.joinable()) {
        render_thread_.join(); // 等待后台逻辑彻底走完
    }

    // 清理并释放所有的 WASAPI 接口指针
    if (render_client_) {
        render_client_->Release();
        render_client_ = nullptr;
    }
    if (audio_client_) {
        audio_client_->Release();
        audio_client_ = nullptr;
    }
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
    if (mix_format_) {
        CoTaskMemFree(mix_format_);
        mix_format_ = nullptr;
    }
}

/**
 * 渲染线程核心：音频引擎的后端处理逻辑。
 * 它负责在这个线程里初始化硬件，并建立死循环进行“拉取音源 -> 叠加混音 -> 格式转化 -> 输出”。
 */
void AudioRenderer::render_thread() {
    // 每一个独立线程在使用 COM 接口前，必须先初始化 COM 环境
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // 1. 获取选定设备的 COM 对象
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr) || !enumerator) {
        CoUninitialize();
        running_.store(false);
        return;
    }

    hr = enumerator->GetDevice(device_id_.c_str(), &device_);
    enumerator->Release();

    if (FAILED(hr) || !device_) {
        CoUninitialize();
        running_.store(false);
        return;
    }

    // 2. 激活音频流客户端接口 (IAudioClient)
    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audio_client_);
    if (FAILED(hr) || !audio_client_) {
        CoUninitialize();
        running_.store(false);
        return;
    }

    // 3. 获取声卡推荐的输出格式（比如：48kHz, 立体声, float）
    hr = audio_client_->GetMixFormat(&mix_format_);
    if (FAILED(hr)) {
        CoUninitialize();
        running_.store(false);
        return;
    }

    /**
     * 4. 初始化音频输出客户端
     * AUDCLNT_SHAREMODE_SHARED: 共享模式，允许其他软件同时也发声。
     * 10000000: 请一个约 1 秒的后端缓冲深度。
     */
    hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, mix_format_, nullptr);
    if (FAILED(hr)) {
        CoUninitialize();
        running_.store(false);
        return;
    }

    // 获取声卡驱动分配的内部缓冲区大小（帧数）
    UINT32 buffer_frames = 0;
    hr = audio_client_->GetBufferSize(&buffer_frames);
    if (FAILED(hr)) {
        CoUninitialize();
        running_.store(false);
        return;
    }

    // 5. 获取具体负责把混合好的数据“塞”进去的播放服务接口 (IAudioRenderClient)
    hr = audio_client_->GetService(__uuidof(IAudioRenderClient), (void**)&render_client_);
    if (FAILED(hr)) {
        CoUninitialize();
        running_.store(false);
        return;
    }

    // 6. 命令声卡：开始播放流程！
    hr = audio_client_->Start();
    if (FAILED(hr)) {
        CoUninitialize();
        running_.store(false);
        return;
    }

    // 根据 mix_format_ 确定声道数和它的位深类型
    int channels = mix_format_->nChannels;
    bool is_float = false;
    if (mix_format_->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE* ext = (WAVEFORMATEXTENSIBLE*)mix_format_;
        is_float = IsEqualGUID(ext->SubFormat, LOCAL_RENDER_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    } else if (mix_format_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        is_float = true;
    }

    // 准备一些临时内存，用于存放混音过程中的数据
    std::vector<float> mix_buffer(buffer_frames);
    std::vector<float> temp_buffer(buffer_frames);

    /**
     * 这里是整个程序最繁忙的死循环：不断地从各个音源环形队列凑齐数据，并推给播放器
     */
    while (running_.load()) {
        UINT32 padding = 0;
        // GetCurrentPadding: 询问 Windows 此时已经放进声卡但还没播完的数据还有多少？
        hr = audio_client_->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            Sleep(1);
            continue;
        }

        // 计算目前我们可以补填多少新数据给声卡
        UINT32 frames_available = buffer_frames - padding;
        if (frames_available == 0) {
            // 如果声卡里数据还是满的，我们就稍微等一下再来
            Sleep(1);
            continue;
        }

        /**
         * 运行混音核心：
         * 我们遍历所有正在活跃的音源（如麦克风、QQ音乐），分别把它们相同位置的采样点相加叠加。
         */
        std::fill(mix_buffer.begin(), mix_buffer.begin() + frames_available, 0.0f);

        {
            // 加锁保护音源清单
            std::lock_guard<std::mutex> lock(sources_mutex_);
            for (auto& source : sources_) {
                // 从环形缓冲区提取一小段采集后的声音
                size_t read = source->pop(temp_buffer.data(), frames_available);
                for (size_t i = 0; i < read; ++i) {
                    // 数字混音原理：将各个音轨在同一个时间点的振幅数值直接相加
                    mix_buffer[i] += temp_buffer[i];
                }
            }
        }

        /**
         * 后期处理：应用总音量，并防止混音后振幅过大导致失真（削波）。
         */
        float master_vol = master_volume_.load(); // 获取用户设置的“总总音量”滑条
        float peak = 0.0f;

        for (UINT32 i = 0; i < frames_available; ++i) {
            mix_buffer[i] *= master_vol;
            // /**
            //  * std::clamp(x, min, max): 将数据限制在正常音频范围 [-1.0, 1.0] 内。
            //  * 这是一个防爆音的重要步骤。
            //  */
            // mix_buffer[i] = std::clamp(mix_buffer[i], -1.0f, 1.0f);
            if (mix_buffer[i] > 1.0f)
            {
                mix_buffer[i] = 1.0f +
                    std::tanh(mix_buffer[i] - 1.0f) * 0.1f;
            }
            else if (mix_buffer[i] < -1.0f)
            {
                mix_buffer[i] = -1.0f -
                    std::tanh(-mix_buffer[i] - 1.0f) * 0.1f;
            }

            // 记录这波数据里的最大振幅，用于驱动界面底部的那个总输出音量跳动条
            float abs_sample = std::fabs(mix_buffer[i]);
            if (abs_sample > peak) peak = abs_sample;
        }

        // 平滑更新总峰值电平指标
        float current_peak = peak_level_.load();
        if (peak > current_peak) {
            peak_level_.store(peak);
        } else {
            peak_level_.store(current_peak * 0.95f + peak * 0.05f);
        }

        /**
         * 数据写入：把我们刚才混好的成品 float 数组写进声卡的原始内存里。
         */
        BYTE* data = nullptr;
        hr = render_client_->GetBuffer(frames_available, &data); // 预定一段要填写的声卡内存
        if (SUCCEEDED(hr) && data) {
            if (is_float && mix_format_->wBitsPerSample == 32) {
                // 情况 A: 硬件原生支持我们的 32bit float
                float* float_data = (float*)data;
                for (UINT32 i = 0; i < frames_available; ++i) {
                    for (int ch = 0; ch < channels; ++ch) {
                        float_data[i * channels + ch] = mix_buffer[i];
                    }
                }
            } else if (mix_format_->wBitsPerSample == 16) {
                // 情况 B: 硬件较老，只支持 16bit 整型，我们需要进行量化转换
                short* short_data = (short*)data;
                for (UINT32 i = 0; i < frames_available; ++i) {
                    // // 将 [-1.0, 1.0] 映射回 [-32768, 32767] 的整数范围
                    // short sample = (short)(mix_buffer[i] * 32767.0f);
                    // for (int ch = 0; ch < channels; ++ch) {
                    //     short_data[i * channels + ch] = sample;
                    // }

                    // 防止超过16位PCM范围
                    float sample = std::clamp(mix_buffer[i], -1.0f, 1.0f);

                    short pcm = static_cast<short>(sample * 32767.0f);

                    for (int ch = 0; ch < channels; ++ch)
                    {
                        short_data[i * channels + ch] = pcm;
                    }
                    
                }
            }

            // ReleaseBuffer: 告诉 Windows 我们填完了，把它播出来吧。
            render_client_->ReleaseBuffer(frames_available, 0);
        }

        // 适当的小憩，防止无意义的 CPU 占用率过高
        Sleep(5);
    }

    // 退出前停止流并注销 COM
    audio_client_->Stop();
    CoUninitialize();
}
