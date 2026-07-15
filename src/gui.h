#pragma once

#include <windows.h>
#include <commctrl.h> // Windows Common Controls: 提供按钮、列表、进度条等标准控件支持
#include <memory>
#include <vector>
#include <string>
#include "audio_engine.h"
#include "language.h"

/**
 * 类名：VoiceProGui
 * 作用：整个程序的“脸面”。它是一个经典的 Win32 图形界面类。
 * 
 * 功能职责：
 * 1. 创建并维护一个 Windows 窗口。
 * 2. 在窗口上摆放按钮、下拉菜单、滑动条。
 * 3. 实时监听用户的点击和拖动事件，并下达指令给音频引擎。
 * 4. 使用定时器不停捕捉音频引擎产生的“峰值电平”，更新界面上的跳动进度条。
 */
class VoiceProGui {
public:
    VoiceProGui();
    ~VoiceProGui();
    
    /**
     * 创建主窗口
     * @param hInstance 程序实例句柄（Windows 给程序分配的 ID）
     * @return 成功返回 true
     */
    bool create(HINSTANCE hInstance);

    /**
     * 运行消息循环
     * 这是桌面程序的“心脏”，它会死循环地处理来自操作系统的所有鼠标、键盘消息。
     */
    int run();

    // 获取窗口原始句柄（HWND 是 Windows 中每个窗口的唯一编号）
    HWND get_hwnd() const { return hwnd_; }

    /**
     * 重新布局
     * 当你拉大或缩小窗口时，该函数会重新计算每个控件的 x, y 坐标，确保它们不会错位。
     */
    void reposition_controls();
    
    /**
     * 静态回调：WndProc (窗口过程)
     * 这是 Windows 操作系统的“硬性规定”。
     * 每当有消息（比如点击）发生时，Windows 会调用这个函数，由它决定如何反应。
     */
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    
private:
    /**
     * 实现细节：handle_message
     * 将 WndProc 收到的枯燥数字消息，解析为具体的业务逻辑。
     */
    LRESULT handle_message(UINT msg, WPARAM wParam, LPARAM lParam);
    
    // --- 界面构建逻辑 ---
    void create_controls();         // 初始化界面时创建所有的子按钮、文本框等
    void refresh_device_lists();    // 向 Windows 询问最新的麦克风和扬声器清单并填入列表
    void refresh_session_list();    // 扫描当前电脑开着的、带声音的软件名单
    void update_mixer_panel();      // 动态生成混音控制台（每增加一个音源，就多出一行滑条）
    void update_peak_meters();      // 定时刷新逻辑：让音量条动起来的关键
    void update_ui_text();          // 更新界面上所有静态控件和按钮的文本以实现国际化
    void update_status_text();      // 更新状态栏文本
    
    // --- 用户交互响应（回调函数）---
    void on_start_stop();           // 用户点击了“启动/停止引擎”按钮
    void on_add_microphone();       // 用户选了一个麦克风并点击“添加”按钮
    void on_add_application();      // 用户选了一个窗口（如音乐播放器）并点击“添加”按钮
    void on_help();                 // 用户点击“问号”图标显示的帮助弹窗
    void on_remove_source(int index);                   // 点击了混音面板中的“垃圾桶”按钮
    void on_source_volume_change(int index, int value);  // 拖动了某个音源的滑条（0-100）
    void on_master_volume_change(int value);            // 拖动了总输出音量的滑条
    void on_output_device_change();                     // 在输出列表下拉框里换了扬声器/耳机
    void on_language_toggle();      // 用户点击了“语言切换”按钮
    
    HWND hwnd_ = nullptr; // 主窗口句柄
    HINSTANCE hInstance_ = nullptr; // 程序实例句柄
    
    // --- 美化资源：GDI 绘图对象 ---
    HFONT hFont_ = nullptr;         // 默认 UI 字体
    HFONT hFontBold_ = nullptr;     // 粗体字（用于板块标题）
    HBRUSH hBrushBg_ = nullptr;     // 窗口背景画笔
    HBRUSH hBrushPanel_ = nullptr;  // 左侧/右侧面板专用的颜色画笔
    HBRUSH hBrushControl_ = nullptr; // 进度条等控件背景画笔
    
    /**
     * 控件 ID 定义
     * 在 Win32 中，按钮没有名字，只有唯一的整数 ID。
     */
    enum {
        ID_COMBO_OUTPUT = 100,      // 输出设备下拉框
        ID_BTN_START_STOP,          // “Start/Stop” 按钮
        ID_BTN_HELP,                // “?” 帮助按钮
        ID_BTN_LANG,                // 语言切换按钮
        ID_LIST_MICROPHONES,        // 麦克风列表
        ID_LIST_APPLICATIONS,       // 程序进程列表
        ID_BTN_ADD_MIC,             // “+” 麦克风图标
        ID_BTN_ADD_APP,             // “+” 应用图标
        ID_SLIDER_MASTER,           // 总闸滑条
        ID_SOURCE_SLIDER_BASE = 1000, // 动态生成的音源滑条起始 ID（ID 会依次累加）
        ID_SOURCE_REMOVE_BASE = 2000, // 动态生成的删除按钮起始 ID
    };

    // --- 存放控件实体的句柄（指针） ---
    HWND combo_output_ = nullptr;
    HWND btn_start_stop_ = nullptr;
    HWND btn_help_ = nullptr;
    HWND btn_lang_ = nullptr;       // 语言切换按钮
    HWND list_microphones_ = nullptr;
    HWND list_applications_ = nullptr;
    HWND btn_add_mic_ = nullptr;
    HWND btn_add_app_ = nullptr;
    HWND static_status_ = nullptr;      // 底部状态栏（显示运行中/停止）
    HWND slider_master_ = nullptr;      // 主输出量滑块
    HWND progress_master_ = nullptr;    // 主输出电平计
    HWND panel_mixer_ = nullptr;        // 中间那个可以流式添加滑条的长条容器
    
    // 标签文本句柄（用于控制显示）
    HWND static_input_title_ = nullptr;
    HWND static_app_title_ = nullptr;
    HWND static_mixer_title_ = nullptr;
    HWND static_output_title_ = nullptr;
    HWND static_master_vol_title_ = nullptr;
    HWND static_sep_ = nullptr;
    
    /**
     * 内部结构：SourceControls
     * 当你添加一个麦克风时，程序会自动生成一组由“名字、百分比、滑块、电平表、删除按钮”组成的 UI 排列。
     */
    struct SourceControls {
        HWND label = nullptr;       // 如 "麦克风"
        HWND label_vol = nullptr;   // 如 "50%"
        HWND slider = nullptr;      // 滑块控件
        HWND progress = nullptr;    // 动态跳动的条
        HWND btn_remove = nullptr;  // 叉号删除按钮
        HWND separator = nullptr;   // 分隔线
    };
    // 动态数组，维护当前界面上所有的混音控制单元
    std::vector<SourceControls> source_controls_; 
    
    // --- 界面缓存数据 ---
    std::vector<OutputDevice> output_devices_;  // 当前电脑能响的设备
    std::vector<MicrophoneDevice> microphones_; // 当前电脑能录的设备
    std::vector<AudioSessionInfo> sessions_;    // 当前电脑开着的软件
    
    // --- 后台核心 ---
    // GUI 与音频引擎的关系是：GUI 发布命令，engine_ 负责执行具体的音频流操作。
    std::unique_ptr<AudioEngine> engine_;
    
    int selected_output_index_ = -1; // 当前选中的输出设备在列表中的位置
    
    // 状态栏状态追踪
    enum class StatusState {
        Ready,
        Stopped,
        Running,
        AppAdded
    };
    StatusState status_state_ = StatusState::Ready;
    std::wstring status_app_name_;

    // 定时器编号定义
    static constexpr UINT_PTR TIMER_UPDATE = 1;  // 负责高频率刷新音量电平条 (30ms)
    static constexpr UINT_PTR TIMER_REFRESH = 2; // 负责低频率刷新窗口列表 (1000ms)
};
