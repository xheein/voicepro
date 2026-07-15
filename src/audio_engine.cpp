#include "audio_engine.h"

/**
 * constexpr: C++ 关键字，表示该值在编译阶段就已确定，是真正的常量。
 * 这里定义每个音源对应的缓冲区大小：采样率(48000 Hz) * 2。
 * 采样率 48000 表示每秒 48000 个采样点。乘以 2 意味着缓冲区可以存放约 2 秒长度的音频。
 */
constexpr size_t BUFFER_SIZE = 48000 * 2; 

// 构造函数：目前没有任何初始化逻辑需要处理
AudioEngine::AudioEngine() {}

// 析构函数：在对象销毁时自动调用
AudioEngine::~AudioEngine() {
    shutdown(); // 析构时确保所有音频采集和播放任务已安全停止并释放
}

/**
 * 初始化音频引擎核心组件
 */
bool AudioEngine::initialize() {
    // 如果已经初始化过了，直接返回成功，避免重复申请资源
    if (initialized_) return true; 
    
    // 初始化系统音频会话枚举器
    // 这个组件负责获取 Windows 系统中哪些程序正在发出声音
    if (!session_enumerator_.initialize()) {
        return false; // 如果初始化失败（例如 COM 权限问题），返回 false
    }
    
    initialized_ = true; // 标记初始化成功
    return true;
}

/**
 * 彻底释放音频引擎资源
 */
void AudioEngine::shutdown() {
    stop();             // 1. 停止当前正在进行的混音和播放动作
    sources_.clear();   // 2. 清空所有音源对象。由于 sources_ 存储的是 AudioSource，
                        //   清空容器会触发每个 AudioSource 的析构，进而释放内部的采集器指针。
    initialized_ = false; // 重置初始化标志
}

/**
 * 调用 MicrophoneCapture 类的静态方法来获取系统录音设备
 * 静态方法可以直接通过类名调用，无需实例化类对象。
 */
std::vector<MicrophoneDevice> AudioEngine::get_microphones() {
    return MicrophoneCapture::enumerate_devices();
}

/**
 * 调用 AudioRenderer 类的静态方法来获取系统播放（扬声器/耳机）设备
 */
std::vector<OutputDevice> AudioEngine::get_output_devices() {
    return AudioRenderer::enumerate_devices();
}

/**
 * 获取当前正在发声的应用程序列表
 */
std::vector<AudioSessionInfo> AudioEngine::get_audio_sessions() {
    return session_enumerator_.enumerate_sessions();
}

/**
 * 刷新会话的方法，目前逻辑已在 enumerate_sessions 中实现
 */
void AudioEngine::refresh_sessions() {
    // 占位函数，保持接口一致性
}

/**
 * 向引擎中添加一个新的麦克风输入源
 * @param device_id 麦克风的硬件 ID
 * @param name 麦克风的显示名称
 */
bool AudioEngine::add_microphone(const std::wstring& device_id, const std::wstring& name) {
    /**
     * std::lock_guard: C++ 的一种异常安全锁管理工具（RAII模式）。
     * 在这个函数开始时上锁，在函数结束（无论正常返回还是报错）时自动解锁。
     * 作用：防止多个线程同时修改 sources_ 列表，避免内存错误（崩溃）。
     */
    std::lock_guard<std::mutex> lock(sources_mutex_); 
    
    // 遍历检查是否已经添加过该麦克风，避免同一个麦克风出现多次
    for (const auto& source : sources_) {
        if (source.type == SourceType::Microphone && source.id == device_id) {
            return false; // 已存在，不重复添加
        }
    }
    
    // 创建一个新的音源信息结构体
    AudioSource source;
    source.type = SourceType::Microphone;
    source.name = name;
    source.id = device_id;
    
    // std::make_shared: 创建一个共享智能指针。
    // 创建环形缓冲区，大小为之前定义的 BUFFER_SIZE。
    source.buffer = std::make_shared<RingBuffer<float>>(BUFFER_SIZE);
    
    // std::make_unique: 创建一个独占智能指针（唯一拥有内存所有权）。
    // 实例化麦克风采集器，并将数据输出端连接到刚创建的 buffer。
    source.mic_capture = std::make_unique<MicrophoneCapture>(device_id, source.buffer);
    
    /**
     * 如果用户在引擎“运行中”的状态下添加音源：
     * 1. 立即启动该麦克风的采集线程。
     * 2. 将该音源的缓冲区加载到正在运作的渲染（混音）器中。
     */
    if (running_) {
        source.mic_capture->start();
        if (renderer_) {
            renderer_->add_source(source.buffer);
        }
    }
    
    // std::move: 将 source 对象的所有权转移进 sources_ 容器。
    // 这样做可以避免大规模的数据拷贝，提高性能。
    sources_.push_back(std::move(source)); 
    return true;
}

/**
 * 添加应用程序音源（通过进程 ID 采集其音频）
 */
bool AudioEngine::add_application(DWORD process_id, const std::wstring& name) {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    
    // 检查是否已经添加了该进程
    for (const auto& source : sources_) {
        if (source.type == SourceType::Application && source.process_id == process_id) {
            return false;
        }
    }
    
    AudioSource source;
    source.type = SourceType::Application;
    source.name = name;
    source.process_id = process_id;
    
    // 设置缓冲区
    source.buffer = std::make_shared<RingBuffer<float>>(BUFFER_SIZE);
    
    // 实例化进程采集器（WASAPI Loopback 模式）
    // 它可以抓取指定程序的 PCM 音频流
    source.app_capture = std::make_unique<ProcessLoopbackCapture>(process_id, source.buffer);
    
    // 如果引擎已在运行，立即激活该采集任务并关联至渲染器
    if (running_) {
        source.app_capture->start();
        if (renderer_) {
            renderer_->add_source(source.buffer);
        }
    }
    
    sources_.push_back(std::move(source));
    return true;
}

/**
 * 移除指定的音源
 * @param index sources_ 向量中的索引位置
 */
void AudioEngine::remove_source(size_t index) {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    
    // 下标合法性检查，防止越界读写
    if (index >= sources_.size()) return;
    
    // 获取目标音源的引用
    auto& source = sources_[index];
    
    // 1. 从渲染（混音）器中撤下该音源的缓冲区，这样之后的混音就不会包含它了
    if (renderer_) {
        renderer_->remove_source(source.buffer);
    }
    
    // 2. 停止该音源的采集线程（如果是运行中状态）
    if (source.mic_capture) {
        source.mic_capture->stop();
    }
    if (source.app_capture) {
        source.app_capture->stop();
    }
    
    // 3. 将音源从 sources_ 动态数组中删除
    // erase 方法会调用 AudioSource 的析构，从而清理 unique_ptr 指向的内存
    sources_.erase(sources_.begin() + index);
}

/**
 * 调整特定音源的采集增益（在读取数据后立即进行乘法运算实现音量控制）
 */
void AudioEngine::set_source_volume(size_t index, float volume) {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    
    if (index >= sources_.size()) return;
    
    auto& source = sources_[index];
    source.volume = volume; // 保存音量状态
    
    // 将最新的音量倍率告知具体的采集模块
    if (source.mic_capture) {
        source.mic_capture->set_volume(volume);
    }
    if (source.app_capture) {
        source.app_capture->set_volume(volume);
    }
}

/**
 * 设置整个混音结果（主输出）的音量
 */
void AudioEngine::set_master_volume(float volume) {
    if (renderer_) {
        renderer_->set_master_volume(volume);
    }
}

/**
 * 获取主输出音量
 */
float AudioEngine::get_master_volume() const {
    if (renderer_) {
        return renderer_->get_master_volume();
    }
    return 1.0f; // 默认 100%
}

/**
 * 获取主输出的实时峰值。这个值由 AudioRenderer (播放端) 计算出来。
 */
float AudioEngine::get_master_peak_level() const {
    if (renderer_) {
        return renderer_->get_peak_level();
    }
    return 0.0f;
}

/**
 * 开启混合播放引擎
 * @param output_device_id 用户指定的扬声器或耳机硬件 ID
 */
bool AudioEngine::start(const std::wstring& output_device_id) {
    // 如果已经运行了，不需要重复启动
    if (running_) return true;
    
    std::lock_guard<std::mutex> lock(sources_mutex_);
    
    // 1. 创建音频渲染器模块
    // AudioRenderer 是本程序最底层的播放驱动，它会开启一个音频播放高优先级线程
    renderer_ = std::make_unique<AudioRenderer>(output_device_id);
    
    // 2. 预加载：把当前 sources_ 里所有音源的缓冲区都注册给渲染器
    for (auto& source : sources_) {
        renderer_->add_source(source.buffer);
    }
    
    // 3. 开启声卡输出（申请缓冲区、开始播放流）
    if (!renderer_->start()) {
        renderer_.reset(); // 如果启动播放失败（如声卡被占用），释放对象并返回
        return false;
    }
    
    // 4. 音效渲染启动成功后，开启所有音源的正式采集
    // 这里采用“先开播放、后开采集”的顺序，确保采集到的数据有地方能立即“流出去”
    for (auto& source : sources_) {
        if (source.mic_capture) {
            source.mic_capture->start();
        }
        if (source.app_capture) {
            source.app_capture->start();
        }
    }
    
    running_ = true; // 标记引擎进入工作状态
    return true;
}

/**
 * 停止所有的音频流程
 */
void AudioEngine::stop() {
    if (!running_) return;
    
    std::lock_guard<std::mutex> lock(sources_mutex_);
    
    // 1. 停止所有的采集过程
    for (auto& source : sources_) {
        if (source.mic_capture) {
            source.mic_capture->stop();
        }
        if (source.app_capture) {
            source.app_capture->stop();
        }
    }
    
    // 2. 停止渲染器并释放相关资源
    if (renderer_) {
        renderer_->stop();
        renderer_.reset(); // 将 point 设置为空，释放内存
    }
    
    running_ = false; // 标记引擎停止
}

// 查询引擎运行状态
bool AudioEngine::is_running() const {
    return running_;
}

/**
 * 更新峰值电平
 * 该函数由 GUI 线程以一定频率（如每秒 60 次）调用。
 * 它会分别访问每个采集器，获取当前的瞬时声音大小，供 UI 绘制音量柱。
 */
void AudioEngine::update_peak_levels() {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    
    for (auto& source : sources_) {
        if (source.mic_capture) {
            // 获取麦克风当前峰值
            source.peak_level = source.mic_capture->get_peak_level();
        }
        if (source.app_capture) {
            // 获取应用程序音频当前峰值
            source.peak_level = source.app_capture->get_peak_level();
        }
    }
}
