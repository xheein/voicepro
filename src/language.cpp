#include "language.h"

LanguageManager& LanguageManager::Instance()
{
    static LanguageManager instance;
    return instance;
}

void LanguageManager::SetLanguage(Language lang)
{
    current_ = lang;
}

Language LanguageManager::GetLanguage() const
{
    return current_;
}

LanguageManager::LanguageManager()
{
    current_ = Language::Chinese;

    // --- Chinese Translations ---
    zh_[L"start"] = L"启动";
    zh_[L"stop"] = L"停止";
    zh_[L"help"] = L"帮助";
    zh_[L"status_ready"] = L"状态: 就绪";
    zh_[L"add_mic"] = L"添加麦克风";
    zh_[L"add_app"] = L"添加应用音频";
    zh_[L"output_device_title"] = L"输出设备 (虚拟音频线)：";
    zh_[L"lang_toggle"] = L"English";
    zh_[L"status_stopped"] = L"状态: 已停止";
    zh_[L"status_running"] = L"状态: 运行中";
    zh_[L"status_app_added"] = L"状态: 应用已添加 - ";
    zh_[L"master_volume"] = L"主音量:";
    zh_[L"input_device"] = L"输入设备";
    zh_[L"app_audio"] = L"应用程序音频";
    zh_[L"mixer"] = L"混音器";
    zh_[L"window_title"] = L"VoicePro - 实时音频混音器";
    zh_[L"help_title"] = L"VoicePro - 帮助";
    zh_[L"error"] = L"错误";
    zh_[L"hint"] = L"提示";
    zh_[L"err_init_engine"] = L"音频引擎初始化失败，请检查驱动是否正常。";
    zh_[L"err_select_output"] = L"请选择一个输出设备";
    zh_[L"err_add_sources"] = L"请添加麦克风或应用程序音频到混音器中。";
    zh_[L"err_start_failed"] = L"启动失败！\n\n原因可能是：\n1. 系统不支持 Loopback API\n2. 输出设备被其他程序独占";
    zh_[L"err_select_mic"] = L"请选中一个麦克风设备再点击添加。";
    zh_[L"err_mic_exists"] = L"该麦克风已经存在于混音列表中了。";
    zh_[L"err_select_app"] = L"请选中一个正在播放声音的程序。";
    zh_[L"err_app_exists"] = L"该应用已经在混音列表中了。";
    zh_[L"err_load_help"] = L"无法加载内置帮助文档。";

    // --- English Translations ---
    en_[L"start"] = L"Start";
    en_[L"stop"] = L"Stop";
    en_[L"help"] = L"Help";
    en_[L"status_ready"] = L"Status: Ready";
    en_[L"add_mic"] = L"Add Microphone";
    en_[L"add_app"] = L"Add Application";
    en_[L"output_device_title"] = L"Output Device (Virtual Audio Cable):";
    en_[L"lang_toggle"] = L"中文";
    en_[L"status_stopped"] = L"Status: Stopped";
    en_[L"status_running"] = L"Status: Running";
    en_[L"status_app_added"] = L"Status: App Added - ";
    en_[L"master_volume"] = L"Master Vol:";
    en_[L"input_device"] = L"Input Device";
    en_[L"app_audio"] = L"App Audio";
    en_[L"mixer"] = L"Mixer";
    en_[L"window_title"] = L"VoicePro - Real-time Audio Mixer";
    en_[L"help_title"] = L"VoicePro - Help";
    en_[L"error"] = L"Error";
    en_[L"hint"] = L"Hint";
    en_[L"err_init_engine"] = L"Audio engine initialization failed. Please check if the driver is installed properly.";
    en_[L"err_select_output"] = L"Please select an output device.";
    en_[L"err_add_sources"] = L"Please add a microphone or application audio to the mixer.";
    en_[L"err_start_failed"] = L"Start failed!\n\nPossible reasons:\n1. System does not support Loopback API\n2. Output device is occupied exclusively by another application.";
    en_[L"err_select_mic"] = L"Please select a microphone device and then click add.";
    en_[L"err_mic_exists"] = L"This microphone is already in the mix list.";
    en_[L"err_select_app"] = L"Please select a running application that is playing audio.";
    en_[L"err_app_exists"] = L"This application is already in the mix list.";
    en_[L"err_load_help"] = L"Failed to load built-in help document.";
}

const std::wstring& LanguageManager::Get(const std::wstring& key) const
{
    if (current_ == Language::Chinese)
    {
        auto it = zh_.find(key);
        if (it != zh_.end()) return it->second;
    }
    else
    {
        auto it = en_.find(key);
        if (it != en_.end()) return it->second;
    }
    
    // Fallback: search key in both
    auto it_zh = zh_.find(key);
    if (it_zh != zh_.end()) return it_zh->second;
    auto it_en = en_.find(key);
    if (it_en != en_.end()) return it_en->second;
    
    static std::wstring empty;
    return empty;
}