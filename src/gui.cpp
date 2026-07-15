#include "gui.h"
#include "resource.h"
#include "language.h"
#include <windowsx.h> // 提供宏简化 Win32 编程（如 GET_X_LPARAM）
#include <uxtheme.h>   // 用于窗口主题美化

// 告诉链接器：我们需要这两个库来支持标准控件美化和主题
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

// 构造函数：创建音频引擎实例，引擎会在此时在后台初始化 WASAPI 环境
VoiceProGui::VoiceProGui() {
    engine_ = std::make_unique<AudioEngine>();
}

// 析构函数：释放所有在程序运行期间申请的绘图资源（GDI 对象）
// 在 Windows 中，如果不手动释放 HFONT、HBRUSH 等，会造成内存泄漏甚至系统变慢。
VoiceProGui::~VoiceProGui() {
    if (hFont_) DeleteObject(hFont_);
    if (hFontBold_) DeleteObject(hFontBold_);
    if (hBrushBg_) DeleteObject(hBrushBg_);
    if (hBrushPanel_) DeleteObject(hBrushPanel_);
    if (hBrushControl_) DeleteObject(hBrushControl_);
}

// 混音面板滚动位置的全局记录变量
static int s_mixer_scroll_pos = 0;

/**
 * 界面重布局逻辑
 * 当你调整主窗口大小时，或者动态添加/删除了音源滑块时，需要重新计算所有按钮和文本的位置。
 * Win32 没有 HTML/CSS 的流式布局，所有坐标都需要手动计算。
 */
void VoiceProGui::reposition_controls() {
    if (!hwnd_ || !combo_output_) return;

    // 获取当前主窗口的“客户区”大小（不含边框和标题栏的部分）
    RECT rc; GetClientRect(hwnd_, &rc);
    int width = rc.right;
    int height = rc.bottom;

    // BeginDeferWindowPos 是一种性能优化技术。
    // 它允许我们把一堆调整大小的操作“打包”，最后一次性通知 Windows 更新，减少闪烁。
    HDWP hdwp = BeginDeferWindowPos(21 + (int)source_controls_.size() * 6);

    // 1. 顶部输出设备选择区的布局
    hdwp = DeferWindowPos(hdwp, static_output_title_, nullptr, 15, 15, 220, 22, SWP_NOZORDER | SWP_NOACTIVATE);
    hdwp = DeferWindowPos(hdwp, combo_output_, nullptr, 240, 13, width - 240 - 310, 200, SWP_NOZORDER | SWP_NOACTIVATE);
    hdwp = DeferWindowPos(hdwp, btn_lang_, nullptr, width - 295, 13, 90, 28, SWP_NOZORDER | SWP_NOACTIVATE);
    hdwp = DeferWindowPos(hdwp, btn_start_stop_, nullptr, width - 200, 13, 90, 28, SWP_NOZORDER | SWP_NOACTIVATE);
    hdwp = DeferWindowPos(hdwp, btn_help_, nullptr, width - 105, 13, 90, 28, SWP_NOZORDER | SWP_NOACTIVATE);

    // 2. 状态栏和主音量控制区的布局
    hdwp = DeferWindowPos(hdwp, static_status_, nullptr, 15, 55, width - 350, 22, SWP_NOZORDER | SWP_NOACTIVATE);
    hdwp = DeferWindowPos(hdwp, static_master_vol_title_, nullptr, width - 330 - 75, 55, 70, 22, SWP_NOZORDER | SWP_NOACTIVATE);
    hdwp = DeferWindowPos(hdwp, slider_master_, nullptr, width - 330, 53, 160, 26, SWP_NOZORDER | SWP_NOACTIVATE);
    hdwp = DeferWindowPos(hdwp, progress_master_, nullptr, width - 160, 57, 145, 18, SWP_NOZORDER | SWP_NOACTIVATE);

    // 分隔线
    hdwp = DeferWindowPos(hdwp, static_sep_, nullptr, 15, 95, width - 30, 2, SWP_NOZORDER | SWP_NOACTIVATE);

    // 3. 左侧选择面板布局（上下分割：麦克风列表与应用列表）
    int left_h = (height - 110 - 60 - 45) / 2; // 计算左侧两个列表平分的高度
    hdwp = DeferWindowPos(hdwp, static_input_title_, nullptr, 15, 110, 150, 22, SWP_NOZORDER | SWP_NOACTIVATE);
    hdwp = DeferWindowPos(hdwp, list_microphones_, nullptr, 15, 140, 260, left_h, SWP_NOZORDER | SWP_NOACTIVATE);
    hdwp = DeferWindowPos(hdwp, btn_add_mic_, nullptr, 15, 140 + left_h + 5, 260, 30, SWP_NOZORDER | SWP_NOACTIVATE);

    int app_y = 140 + left_h + 50;
    hdwp = DeferWindowPos(hdwp, static_app_title_, nullptr, 15, app_y, 150, 22, SWP_NOZORDER | SWP_NOACTIVATE);
    hdwp = DeferWindowPos(hdwp, list_applications_, nullptr, 15, app_y + 30, 260, height - app_y - 80, SWP_NOZORDER | SWP_NOACTIVATE);
    hdwp = DeferWindowPos(hdwp, btn_add_app_, nullptr, 15, height - 45, 260, 30, SWP_NOZORDER | SWP_NOACTIVATE);

    // 4. 右侧混音核心面板的布局
    hdwp = DeferWindowPos(hdwp, static_mixer_title_, nullptr, 295, 110, 150, 22, SWP_NOZORDER | SWP_NOACTIVATE);
    hdwp = DeferWindowPos(hdwp, panel_mixer_, nullptr, 295, 140, width - 295 - 15, height - 140 - 15, SWP_NOZORDER | SWP_NOACTIVATE);

    // 执行所有的打包位置调整动作
    EndDeferWindowPos(hdwp);

    /**
     * 5. 混音面板内部滚动逻辑
     * panel_mixer_ 内部有很多动态创建的控件（每行一个音源）。
     * 我们需要根据当前滚动条的位置 (si.nPos)，手动计算这些子控件的 y 偏移。
     */
    RECT prc; GetClientRect(panel_mixer_, &prc);
    int panel_w = prc.right - prc.left;
    int panel_h = prc.bottom - prc.top;

    // 计算面板内所有控件排在一起的总高度
    int y_total = 10 + (int)source_controls_.size() * (28 + 38 + 10);

    // 设置或更新垂直滚动条参数
    SCROLLINFO si = { sizeof(si), SIF_ALL };
    GetScrollInfo(panel_mixer_, SB_VERT, &si);

    si.nMin = 0;
    si.nMax = y_total;      // 文档总高度
    si.nPage = panel_h;    // 窗口能看到的高度

    int maxScroll = si.nMax - (int)si.nPage;
    if (maxScroll < 0) maxScroll = 0;

    // 确保滚动条位置不会越界（比如删除了几个音源后，位置可能大于总高度）
    if (si.nPos > maxScroll) {
        si.nPos = maxScroll;
    }
    if ((int)si.nMax <= (int)si.nPage) {
        si.nPos = 0;
    }

    // 正式更新滚动条状态
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    SetScrollInfo(panel_mixer_, SB_VERT, &si, TRUE);

    // 6. 重新移动混音面板内的每一个子控件
    int y_offset = -si.nPos; // 基础 y 值为反向滚动位移
    int y_pos = 10;

    for (auto& sc : source_controls_) {
        int rel_y = y_pos + y_offset;
        // 这一排：音源名称 | 垃圾桶按钮
        SetWindowPos(sc.label, nullptr, 10, rel_y, panel_w - 40, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(sc.btn_remove, nullptr, panel_w - 50, rel_y - 2, 35, 24, SWP_NOZORDER | SWP_NOACTIVATE);
        y_pos += 28;

        rel_y = y_pos + y_offset;
        // 这一排：百分比标签 | 音量滑条 | 跳动的电平计
        SetWindowPos(sc.label_vol, nullptr, 10, rel_y + 2, 55, 20, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(sc.slider, nullptr, 70, rel_y, 195, 24, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(sc.progress, nullptr, 270, rel_y + 3, panel_w - 290, 18, SWP_NOZORDER | SWP_NOACTIVATE);
        y_pos += 38;

        rel_y = y_pos + y_offset;
        // 底部：淡灰色的分隔线
        SetWindowPos(sc.separator, nullptr, 10, rel_y, panel_w - 30, 2, SWP_NOZORDER | SWP_NOACTIVATE);
        y_pos += 10;
    }
}

/**
 * 子类过程：MixerPanelProc
 * 专门负责处理混音面板 (panel_mixer_) 的特殊消息。
 * 作用：让它支持鼠标滚轮滚动，并将本该发给面板的按钮点击命令转发给主窗口处理。
 */
LRESULT CALLBACK MixerPanelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    (void)uIdSubclass;
    VoiceProGui* gui = (VoiceProGui*)dwRefData;
    switch (msg) {
    // 处理鼠标滚轮
    case WM_MOUSEWHEEL: {
        int zDelta = (short)HIWORD(wParam);
        // 转发为标准的垂直滚动命令
        SendMessage(hwnd, WM_VSCROLL, zDelta > 0 ? SB_LINEUP : SB_LINEDOWN, 0);
        return 0;
    }
    // 处理垂直滚动条的操作（点击上下箭头、拖动滑块等）
    case WM_VSCROLL: {
        SCROLLINFO si = { sizeof(si), SIF_ALL };
        GetScrollInfo(hwnd, SB_VERT, &si);
        int oldPos = si.nPos;
        switch (LOWORD(wParam)) {
        case SB_TOP: si.nPos = si.nMin; break;
        case SB_BOTTOM: si.nPos = si.nMax; break;
        case SB_LINEUP: si.nPos -= 20; break;
        case SB_LINEDOWN: si.nPos += 20; break;
        case SB_PAGEUP: si.nPos -= si.nPage; break;
        case SB_PAGEDOWN: si.nPos += si.nPage; break;
        case SB_THUMBTRACK: si.nPos = si.nTrackPos; break;
        }

        // 限制滚动范围
        int maxScroll = si.nMax - (int)si.nPage;
        if (maxScroll < 0) maxScroll = 0;
        if (si.nPos < 0) si.nPos = 0;
        if (si.nPos > maxScroll) si.nPos = maxScroll;

        si.fMask = SIF_POS;
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        if (si.nPos != oldPos) {
            // 位置变了，告诉 GUI 界面需要重新排列各个滑条
            gui->reposition_controls();
            // 立即重绘，减少白影闪烁
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        }
        return 0;
    }
    // “命令”转发：当面板里的按钮被点时，消息会发给面板，我们需要丢给主窗口处理逻辑
    case WM_COMMAND:
        SendMessage(gui->get_hwnd(), WM_COMMAND, wParam, lParam);
        return 0;
    case WM_HSCROLL:
        SendMessage(gui->get_hwnd(), WM_HSCROLL, wParam, lParam);
        return 0;
    case WM_ERASEBKGND:
        return 1; // 告诉系统不要擦除背景，防止列表滚动时产生强烈的白光闪烁
    }
    // 其他普通消息交给系统的默认窗口函数处理
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

/**
 * 核心：创建窗口与初始化
 * 该函数负责注册窗口类、创建物理窗口、初始化字体和画笔，并启动音频引擎。
 */
bool VoiceProGui::create(HINSTANCE hInstance) {
    hInstance_ = hInstance;

    // 初始化 Windows 通用控件库（按钮美化、进度条等需要它）
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);

    // 1. 注册窗口类：告诉 Windows 我们窗口的属性（图标、光标、处理函数）
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc; // 指定回调函数
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // 我们使用自定义背景绘图，所以不设默认背景
    wc.lpszClassName = L"VoiceProWindow";
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON)); // 加载资源里的图标
    wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));

    if (!RegisterClassExW(&wc)) {
        return false;
    }

    // 2. 创建物理窗口：固定大小的窗口，不允许最大化（为了 UI 布局稳定）
    hwnd_ = CreateWindowExW(
        0,
        L"VoiceProWindow",
        LanguageManager::Instance().Get(L"window_title").c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        950, 700,
        nullptr, nullptr, hInstance, this // 将 this 指针传给窗口，方便回调函数访问成员
    );

    if (!hwnd_) {
        return false;
    }

    /**
     * 3. 创建 UI 字体与色彩画笔
     * 为了让界面看起来像现代应用，我们需要自定义字体（Segoe UI）和配色。
     */
    hFont_ = CreateFontW(
        -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI"
    );

    hFontBold_ = CreateFontW(
        -16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI"
    );

    // 设定配色方案（类 Apple/Modern 风格的浅色系）
    hBrushBg_ = CreateSolidBrush(RGB(245, 245, 247));          // 窗口大底色（浅灰）
    hBrushPanel_ = CreateSolidBrush(RGB(255, 255, 255));       // 核心面板区（白色）
    hBrushControl_ = CreateSolidBrush(RGB(252, 252, 252));     // 控件装饰色

    // 创建窗口内部的各个子控件（按钮、列表等）
    create_controls();

    // 4. 初始化底层的音频核心
    if (!engine_->initialize()) {
        auto& lm = LanguageManager::Instance();
        MessageBoxW(hwnd_, lm.Get(L"err_init_engine").c_str(), lm.Get(L"error").c_str(), MB_OK | MB_ICONERROR);
    }

    // 初次刷新设备清单
    refresh_device_lists();

    /**
     * 5. 启动界面刷新定时器
     * TIMER_UPDATE (50ms): 负责每秒 20 次刷新那些绿色的音量跳动条。
     * TIMER_REFRESH (2s): 负责每隔 2 秒检查一下电脑音哪些新软件打开了。
     */
    SetTimer(hwnd_, TIMER_UPDATE, 50, nullptr);
    SetTimer(hwnd_, TIMER_REFRESH, 2000, nullptr);

    // 显示并更新窗口
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    return true;
}

/**
 * 运行消息循环
 * 将死循环地从 Windows 队列拿消息，直到程序退出。
 */
int VoiceProGui::run() {
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}

/**
 * 全局窗口回调函数 (WndProc)
 * Windows 是基于事件驱动的。每当用户动一下鼠标，系统就会调用这个函数。
 * 因为这是一个全局函数，无法直接访问类里的变量，所以我们通过 GWLP_USERDATA 拿回 this 指针。
 */
LRESULT CALLBACK VoiceProGui::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    VoiceProGui* gui = nullptr;

    // 当窗口刚刚创建时 (WM_NCCREATE)，把 this 指针存进窗口的“私有口袋”里
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        gui = static_cast<VoiceProGui*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(gui));
        gui->hwnd_ = hwnd;
    } else {
        // 后续的消息，直接从口袋里把 this 指针拿出来用
        gui = reinterpret_cast<VoiceProGui*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    // 如果成功拿到了对象指针，就把消息转交给类内部的句柄处理
    if (gui) {
        return gui->handle_message(msg, wParam, lParam);
    }

    // 如果窗口还没完全初始化好，交给系统默认处理
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/**
 * 消息处理中心 (handle_message)
 * 这里是解析“用户到底干了什么”的核心逻辑。
 */
LRESULT VoiceProGui::handle_message(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    // 1. 处理点按钮、拉下菜单等指令
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        switch (id) {
        case ID_BTN_START_STOP:
            on_start_stop();
            return 0;
        case ID_BTN_HELP:
            on_help();
            return 0;
        case ID_BTN_LANG:
            on_language_toggle();
            return 0;
        case ID_BTN_ADD_MIC:
            on_add_microphone();
            return 0;
        case ID_BTN_ADD_APP:
            on_add_application();
            return 0;
        case ID_COMBO_OUTPUT:
            // 当下拉框的选择发生改变时 (CBN_SELCHANGE)
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                on_output_device_change();
            }
            return 0;
        default:
            if (id >= ID_SOURCE_REMOVE_BASE && id < ID_SOURCE_REMOVE_BASE + (int)source_controls_.size()) {
                on_remove_source(id - ID_SOURCE_REMOVE_BASE);
                return 0;
            }
            break;
        }
        break;
    }

    // 2. 处理水平滑块（主音量和每个音源音量）
    case WM_HSCROLL: {
        HWND sender = reinterpret_cast<HWND>(lParam);
        if (sender == slider_master_) {
            int value = (int)SendMessage(slider_master_, TBM_GETPOS, 0, 0);
            on_master_volume_change(value);
            return 0;
        }

        for (size_t i = 0; i < source_controls_.size(); ++i) {
            if (sender == source_controls_[i].slider) {
                int value = (int)SendMessage(source_controls_[i].slider, TBM_GETPOS, 0, 0);
                on_source_volume_change((int)i, value);
                return 0;
            }
        }
        break;
    }

    // 3. 定时刷新电平和应用程序列表
    case WM_TIMER:
        if (wParam == TIMER_UPDATE) {
            update_peak_meters();
            return 0;
        }
        if (wParam == TIMER_REFRESH) {
            refresh_session_list();
            return 0;
        }
        break;

    // 4. 窗口尺寸变化时重新布局
    case WM_SIZE:
        reposition_controls();
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = 760;
        mmi->ptMinTrackSize.y = 520;
        return 0;
    }

    // 5. 自绘浅色背景，减少闪烁
    case WM_ERASEBKGND: {
        if (!hBrushBg_) break;
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc;
        GetClientRect(hwnd_, &rc);
        FillRect(hdc, &rc, hBrushBg_);
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        if (!hBrushBg_) break;
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(hBrushBg_);
    }

    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT: {
        if (!hBrushPanel_) break;
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(hBrushPanel_);
    }

    case WM_DESTROY:
        KillTimer(hwnd_, TIMER_UPDATE);
        KillTimer(hwnd_, TIMER_REFRESH);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void VoiceProGui::create_controls() {
    auto& lm = LanguageManager::Instance();
    int y = 15;

    status_state_ = StatusState::Ready;
    status_app_name_.clear();

    // 1. 顶部输出设备区
    static_output_title_ = CreateWindowW(L"STATIC", lm.Get(L"output_device_title").c_str(),
        WS_CHILD | WS_VISIBLE, 15, y, 220, 22, hwnd_, nullptr, hInstance_, nullptr);
    SendMessage(static_output_title_, WM_SETFONT, (WPARAM)hFontBold_, TRUE);

    // 下拉选择框
    combo_output_ = CreateWindowW(L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        240, y - 2, 380, 200, hwnd_, (HMENU)ID_COMBO_OUTPUT, hInstance_, nullptr);

    // 语言切换按钮
    btn_lang_ = CreateWindowW(L"BUTTON", lm.Get(L"lang_toggle").c_str(),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        640, y - 2, 90, 28, hwnd_, (HMENU)ID_BTN_LANG, hInstance_, nullptr);

    // 启动/停止按钮
    btn_start_stop_ = CreateWindowW(L"BUTTON", lm.Get(L"start").c_str(),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        735, y - 2, 90, 28, hwnd_, (HMENU)ID_BTN_START_STOP, hInstance_, nullptr);

    // 帮助按钮
    btn_help_ = CreateWindowW(L"BUTTON", lm.Get(L"help").c_str(),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        830, y - 2, 90, 28, hwnd_, (HMENU)ID_BTN_HELP, hInstance_, nullptr);

    y += 40;

    // 2. 状态栏与主滑块
    static_status_ = CreateWindowW(L"STATIC", lm.Get(L"status_ready").c_str(),
        WS_CHILD | WS_VISIBLE, 15, y, 500, 22, hwnd_, nullptr, hInstance_, nullptr);

    static_master_vol_title_ = CreateWindowW(L"STATIC", lm.Get(L"master_volume").c_str(),
        WS_CHILD | WS_VISIBLE, 550, y, 70, 22, hwnd_, nullptr, hInstance_, nullptr);

    // 总闸滑条
    slider_master_ = CreateWindowW(TRACKBAR_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        620, y - 2, 160, 26, hwnd_, (HMENU)ID_SLIDER_MASTER, hInstance_, nullptr);
    SendMessage(slider_master_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 500));
    SendMessage(slider_master_, TBM_SETPOS, TRUE, 100);

    // 总闸对应的绿色电平跳动条
    progress_master_ = CreateWindowW(PROGRESS_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        790, y + 2, 130, 18, hwnd_, nullptr, hInstance_, nullptr);
    SendMessage(progress_master_, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

    y += 40;

    // 装饰性分隔线
    static_sep_ = CreateWindowW(L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
        15, y, 905, 2, hwnd_, nullptr, hInstance_, nullptr);
    y += 15;

    // 3. 左侧面板：设备选择区
    static_input_title_ = CreateWindowW(L"STATIC", lm.Get(L"input_device").c_str(),
        WS_CHILD | WS_VISIBLE, 15, y, 150, 22, hwnd_, nullptr, hInstance_, nullptr);
    SendMessage(static_input_title_, WM_SETFONT, (WPARAM)hFontBold_, TRUE);
    y += 28;

    list_microphones_ = CreateWindowW(L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
        15, y, 260, 130, hwnd_, (HMENU)ID_LIST_MICROPHONES, hInstance_, nullptr);

    btn_add_mic_ = CreateWindowW(L"BUTTON", lm.Get(L"add_mic").c_str(),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        15, y + 135, 260, 30, hwnd_, (HMENU)ID_BTN_ADD_MIC, hInstance_, nullptr);

    y += 180;

    // 应用音频扫描区
    static_app_title_ = CreateWindowW(L"STATIC", lm.Get(L"app_audio").c_str(),
        WS_CHILD | WS_VISIBLE, 15, y, 150, 22, hwnd_, nullptr, hInstance_, nullptr);
    SendMessage(static_app_title_, WM_SETFONT, (WPARAM)hFontBold_, TRUE);
    y += 28;

    list_applications_ = CreateWindowW(L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
        15, y, 260, 240, hwnd_, (HMENU)ID_LIST_APPLICATIONS, hInstance_, nullptr);

    btn_add_app_ = CreateWindowW(L"BUTTON", lm.Get(L"add_app").c_str(),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        15, y + 245, 260, 30, hwnd_, (HMENU)ID_BTN_ADD_APP, hInstance_, nullptr);

    // 4. 右侧混音核心标题 + 大面板
    static_mixer_title_ = CreateWindowW(L"STATIC", lm.Get(L"mixer").c_str(),
        WS_CHILD | WS_VISIBLE, 295, 110, 150, 22, hwnd_, nullptr, hInstance_, nullptr);
    SendMessage(static_mixer_title_, WM_SETFONT, (WPARAM)hFontBold_, TRUE);

    // SS_NOTIFY 表示该面板可以接收点击，WS_VSCROLL 开启垂直滚动条
    panel_mixer_ = CreateWindowW(L"STATIC", nullptr,
        WS_CHILD | WS_VISIBLE | WS_BORDER | SS_NOTIFY | WS_VSCROLL | WS_CLIPCHILDREN,
        295, 140, 620, 520, hwnd_, nullptr, hInstance_, nullptr);

    /**
     * 子类化 (Subclassing):
     * 我们通过 SetWindowSubclass 强行"接管"了 panel_mixer_ 的内部逻辑，
     * 这样我们刚才写的 MixerPanelProc 函数就能处理它的滚动消息了。
     */
    SetWindowSubclass(panel_mixer_, MixerPanelProc, 0, (DWORD_PTR)this);

    // 最后一步：统一给所有子控件设置现代字体，防止显示丑陋的"宋体"或像素字体
    EnumChildWindows(hwnd_, [](HWND child, LPARAM lParam) -> BOOL {
        SendMessage(child, WM_SETFONT, (WPARAM)lParam, TRUE);
        return TRUE;
    }, (LPARAM)hFont_);
}

/**
 * 刷新设备清单 (refresh_device_lists)
 * 询问声卡：现在有多少喇叭可以响，有多少麦克风在插着？
 */
void VoiceProGui::refresh_device_lists() {
    // 1. 获取所有输出设备（如：扬声器、虚拟音频线）
    output_devices_ = engine_->get_output_devices();
    SendMessage(combo_output_, CB_RESETCONTENT, 0, 0); // 先清空下拉框
    for (const auto& dev : output_devices_) {
        // 将设备名字填入下拉框
        SendMessageW(combo_output_, CB_ADDSTRING, 0, (LPARAM)dev.name.c_str());
    }
    // 默认选中第一个
    if (!output_devices_.empty()) {
        SendMessage(combo_output_, CB_SETCURSEL, 0, 0);
        selected_output_index_ = 0;
    }

    // 2. 获取所有麦克风设备
    microphones_ = engine_->get_microphones();
    SendMessage(list_microphones_, LB_RESETCONTENT, 0, 0); // 清空列表框
    for (const auto& mic : microphones_) {
        SendMessageW(list_microphones_, LB_ADDSTRING, 0, (LPARAM)mic.name.c_str());
    }

    // 3. 同时刷新一下正在播放声音的应用列表
    refresh_session_list();
}

/**
 * 刷新程序列表 (refresh_session_list)
 * 它是被定时器每 2 秒调用一次的。
 * 作用：如果你新打开了一个网易云音乐，它会自动出现在列表里，不需要手动刷新。
 */
void VoiceProGui::refresh_session_list() {
    sessions_ = engine_->get_audio_sessions();

    // 记住用户之前选中的是第几个，防止刷新后选中的高亮消失
    int sel = (int)SendMessage(list_applications_, LB_GETCURSEL, 0, 0);

    SendMessage(list_applications_, LB_RESETCONTENT, 0, 0);
    for (const auto& session : sessions_) {
        std::wstring display = session.name;
        // 如果程序没设置名字，就使用它的文件名 (如 chrome.exe)
        if (display.empty()) {
            display = session.executable_name;
        }
        SendMessageW(list_applications_, LB_ADDSTRING, 0, (LPARAM)display.c_str());
    }

    // 恢复之前的选中状态
    if (sel >= 0 && sel < (int)sessions_.size()) {
        SendMessage(list_applications_, LB_SETCURSEL, sel, 0);
    }
}

/**
 * 混音面板动态更新 (update_mixer_panel)
 * 每当你点一次“添加麦克风”或“移除音源”，这个函数就会大动干戈：
 * 它会销毁面板里所有的旧按钮，然后根据 engine 里当前的音源列表，从头开始“手绘”一整套滑条。
 */
void VoiceProGui::update_mixer_panel() {
    // 1. 暴力清理：销毁之前显示的所有子控件，释放窗口句柄资源
    for (auto& sc : source_controls_) {
        if (sc.label) DestroyWindow(sc.label);
        if (sc.label_vol) DestroyWindow(sc.label_vol);
        if (sc.slider) DestroyWindow(sc.slider);
        if (sc.progress) DestroyWindow(sc.progress);
        if (sc.btn_remove) DestroyWindow(sc.btn_remove);
        if (sc.separator) DestroyWindow(sc.separator);
    }
    source_controls_.clear();

    // 获取音频引擎目前实际正在混音的所有源
    auto& sources = engine_->get_sources();

    RECT prc;
    GetClientRect(panel_mixer_, &prc);
    int panel_w = prc.right - prc.left;

    // 2. 循环创建新的一行行控制项
    for (size_t i = 0; i < sources.size(); ++i) {
        SourceControls sc;

        // 创建名字标签。SS_ENDELLIPSIS 表示名字太长时末尾显示 ...
        sc.label = CreateWindowW(L"STATIC", sources[i].name.c_str(),
            WS_CHILD | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS,
            10, 0, panel_w - 40, 22, panel_mixer_, nullptr, hInstance_, nullptr);

        // 创建“垃圾桶”删除按钮
        sc.btn_remove = CreateWindowW(L"BUTTON", L"✕",
            WS_CHILD | BS_PUSHBUTTON,
            panel_w - 50, 0, 35, 24, panel_mixer_, (HMENU)(ID_SOURCE_REMOVE_BASE + i), hInstance_, nullptr);

        // 音量百分比标签（显示如 "100%"）
        wchar_t vol_label[16];
        swprintf_s(vol_label, L"%d%%", (int)(sources[i].volume * 100));
        sc.label_vol = CreateWindowW(L"STATIC", vol_label,
            WS_CHILD, 10, 0, 55, 20, panel_mixer_, nullptr, hInstance_, nullptr);

        // 创建该音源专属的音量滑块
        // 范围 0-500 对应 0%-500% 的增益（最高 5 倍），以解决应用音频信号偏低的问题
        sc.slider = CreateWindowW(TRACKBAR_CLASSW, nullptr,
            WS_CHILD | TBS_HORZ | TBS_NOTICKS,
            65, 0, 200, 24, panel_mixer_, (HMENU)(ID_SOURCE_SLIDER_BASE + i), hInstance_, nullptr);
        SendMessage(sc.slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 500));
        SendMessage(sc.slider, TBM_SETPOS, TRUE, (int)(sources[i].volume * 100));

        // 创建该音源专属的电平进度条
        sc.progress = CreateWindowW(PROGRESS_CLASSW, nullptr,
            WS_CHILD | PBS_SMOOTH,
            270, 0, panel_w - 290, 18, panel_mixer_, nullptr, hInstance_, nullptr);
        SendMessage(sc.progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

        sc.separator = CreateWindowW(L"STATIC", nullptr, WS_CHILD | SS_ETCHEDHORZ,
            10, 0, panel_w - 30, 2, panel_mixer_, nullptr, hInstance_, nullptr);

        // 设置字体
        HWND hMixerKids[] = { sc.label, sc.btn_remove, sc.label_vol, sc.slider, sc.progress, sc.separator };
        for (HWND hk : hMixerKids) {
            SendMessage(hk, WM_SETFONT, (WPARAM)hFont_, FALSE);
        }

        source_controls_.push_back(sc);
    }

    // 3. 摆放并显示
    // 调用之前写的布局函数来设置这一大堆新控件的 x/y 坐标
    reposition_controls();

    // 正式显示它们
    for (auto& sc : source_controls_) {
        ShowWindow(sc.label, SW_SHOW);
        ShowWindow(sc.btn_remove, SW_SHOW);
        ShowWindow(sc.label_vol, SW_SHOW);
        ShowWindow(sc.slider, SW_SHOW);
        ShowWindow(sc.progress, SW_SHOW);
        ShowWindow(sc.separator, SW_SHOW);
    }

    // 通知面板重新绘制自己，确保没有残影
    InvalidateRect(panel_mixer_, nullptr, TRUE);
    UpdateWindow(panel_mixer_);
}

/**
 * 刷新电平表 (update_peak_meters)
 * 每 50 毫秒跳动一次。
 * 作用：从音频引擎拿取各条音轨当前的最高音量值，更新界面上的进度条。
 */
void VoiceProGui::update_peak_meters() {
    // 如果引擎没在跑，没必要更新
    if (!engine_->is_running()) return;

    // 驱动引擎从后台缓冲区抓取电平数据
    engine_->update_peak_levels();

    // 更新主输出电平计（画面最下方那个）
    float master_peak = engine_->get_master_peak_level();
    SendMessage(progress_master_, PBM_SETPOS, (int)(master_peak * 100), 0);

    // 更新混音面板里每一行对应的电平计和音量标签
    auto& sources = engine_->get_sources();
    for (size_t i = 0; i < sources.size() && i < source_controls_.size(); ++i) {
        // 更新电平进度条
        int peak = (int)(sources[i].peak_level * 100);
        SendMessage(source_controls_[i].progress, PBM_SETPOS, peak, 0);

        // 实时刷新音量百分比标签（每次定时器触发时同步）
        wchar_t vol_label[16];
        swprintf_s(vol_label, L"%d%%", (int)(sources[i].volume * 100));
        SetWindowTextW(source_controls_[i].label_vol, vol_label);
    }
}

void VoiceProGui::update_ui_text() {
    auto& lm = LanguageManager::Instance();

    if (hwnd_) SetWindowTextW(hwnd_, lm.Get(L"window_title").c_str());
    if (btn_lang_) SetWindowTextW(btn_lang_, lm.Get(L"lang_toggle").c_str());
    if (btn_help_) SetWindowTextW(btn_help_, lm.Get(L"help").c_str());
    if (btn_add_mic_) SetWindowTextW(btn_add_mic_, lm.Get(L"add_mic").c_str());
    if (btn_add_app_) SetWindowTextW(btn_add_app_, lm.Get(L"add_app").c_str());

    if (btn_start_stop_) {
        SetWindowTextW(btn_start_stop_, lm.Get(engine_->is_running() ? L"stop" : L"start").c_str());
    }

    if (static_output_title_) SetWindowTextW(static_output_title_, lm.Get(L"output_device_title").c_str());
    if (static_master_vol_title_) SetWindowTextW(static_master_vol_title_, lm.Get(L"master_volume").c_str());
    if (static_input_title_) SetWindowTextW(static_input_title_, lm.Get(L"input_device").c_str());
    if (static_app_title_) SetWindowTextW(static_app_title_, lm.Get(L"app_audio").c_str());
    if (static_mixer_title_) SetWindowTextW(static_mixer_title_, lm.Get(L"mixer").c_str());

    update_status_text();
}

void VoiceProGui::update_status_text() {
    if (!static_status_) return;

    auto& lm = LanguageManager::Instance();
    std::wstring text;

    switch (status_state_) {
    case StatusState::Ready:
        text = lm.Get(L"status_ready");
        break;
    case StatusState::Stopped:
        text = lm.Get(L"status_stopped");
        break;
    case StatusState::Running:
        text = lm.Get(L"status_running");
        break;
    case StatusState::AppAdded:
        text = lm.Get(L"status_app_added") + status_app_name_;
        break;
    }

    SetWindowTextW(static_status_, text.c_str());
}

void VoiceProGui::on_language_toggle() {
    auto& lm = LanguageManager::Instance();
    lm.SetLanguage(lm.GetLanguage() == Language::Chinese ? Language::English : Language::Chinese);

    update_ui_text();
    reposition_controls();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * 按钮回调：启动/停止引擎 (on_start_stop)
 */
void VoiceProGui::on_start_stop() {
    auto& lm = LanguageManager::Instance();

    // 如果已经在跑，就执行停止
    if (engine_->is_running()) {
        engine_->stop();
        status_state_ = StatusState::Stopped;
        status_app_name_.clear();
        update_ui_text();
        return;
    }

    // 1. 检查有没有选喇叭
    if (selected_output_index_ < 0 || selected_output_index_ >= (int)output_devices_.size()) {
        MessageBoxW(hwnd_, lm.Get(L"err_select_output").c_str(), lm.Get(L"hint").c_str(), MB_OK | MB_ICONWARNING);
        return;
    }

    // 2. 检查有没有添加音源
    if (engine_->get_sources().empty()) {
        MessageBoxW(hwnd_, lm.Get(L"err_add_sources").c_str(), lm.Get(L"hint").c_str(), MB_OK | MB_ICONWARNING);
        return;
    }

    // 3. 正式启动引擎
    if (engine_->start(output_devices_[selected_output_index_].id)) {
        status_state_ = StatusState::Running;
        status_app_name_.clear();
        update_ui_text();
    } else {
        MessageBoxW(hwnd_, lm.Get(L"err_start_failed").c_str(), lm.Get(L"error").c_str(), MB_OK | MB_ICONERROR);
    }
}

/**
 * 按钮回调：添加麦克风 (on_add_microphone)
 */
void VoiceProGui::on_add_microphone() {
    auto& lm = LanguageManager::Instance();
    int sel = (int)SendMessage(list_microphones_, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)microphones_.size()) {
        MessageBoxW(hwnd_, lm.Get(L"err_select_mic").c_str(), lm.Get(L"hint").c_str(), MB_OK | MB_ICONINFORMATION);
        return;
    }

    // 通知引擎把这个麦克风加入混音名单
    if (engine_->add_microphone(microphones_[sel].id, microphones_[sel].name)) {
        // 引擎名单变了，界面滑条也要重新“画”一遍
        update_mixer_panel();
    } else {
        MessageBoxW(hwnd_, lm.Get(L"err_mic_exists").c_str(), lm.Get(L"hint").c_str(), MB_OK | MB_ICONINFORMATION);
    }
}

/**
 * 按钮回调：添加应用音频 (on_add_application)
 */
void VoiceProGui::on_add_application() {
    auto& lm = LanguageManager::Instance();
    int sel = (int)SendMessage(list_applications_, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)sessions_.size()) {
        MessageBoxW(hwnd_, lm.Get(L"err_select_app").c_str(), lm.Get(L"hint").c_str(), MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::wstring name = sessions_[sel].name;
    if (name.empty()) {
        name = sessions_[sel].executable_name;
    }

    // 通知引擎：我要录这个 PID 的声音
    if (engine_->add_application(sessions_[sel].process_id, name)) {
        update_mixer_panel();
        status_state_ = StatusState::AppAdded;
        status_app_name_ = name;
        update_status_text();
    } else {
        MessageBoxW(hwnd_, lm.Get(L"err_app_exists").c_str(), lm.Get(L"hint").c_str(), MB_OK | MB_ICONINFORMATION);
    }
}

/**
 * 回调：移除音源 (on_remove_source)
 * 当用户点了某个滑条旁边的“✕”按钮时触发。
 */
void VoiceProGui::on_remove_source(int index) {
    if (index < 0 || index >= (int)engine_->get_sources().size()) return;

    // 为了防止移除时发生指针冲突，我们先停止引擎
    bool was_running = engine_->is_running();
    if (was_running) {
        engine_->stop();
    }

    // 移除指定位置的音源
    engine_->remove_source(index);
    update_mixer_panel(); // 重画界面

    // 如果之前在跑且还没删光，就自动重新启动
    if (engine_->get_sources().empty()) {
        status_state_ = StatusState::Ready;
        status_app_name_.clear();
    } else if (was_running && selected_output_index_ >= 0 && selected_output_index_ < (int)output_devices_.size()
        && engine_->start(output_devices_[selected_output_index_].id)) {
        status_state_ = StatusState::Running;
        status_app_name_.clear();
    } else if (was_running) {
        status_state_ = StatusState::Stopped;
        status_app_name_.clear();
    }

    update_ui_text();
}

/**
 * 回调：调节单个音源的音量 (on_source_volume_change)
 */
void VoiceProGui::on_source_volume_change(int index, int value) {
    // 将滑块的 0-500 整数值转换为 0.0-5.0 的增益倍数
    // 例如：滑块位置 100 = 1.0 倍（原声），250 = 2.5 倍，500 = 5.0 倍
    float volume = value / 100.0f;
    engine_->set_source_volume(index, volume);
}

/**
 * 按钮回调：显示帮助 (on_help)
 * 处理过程：
 * 1. 从程序的资源文件 (RC) 里读取 help.txt 文档。
 * 2. 动态注册一个新窗口类，弹出一个包含多行编辑框的小窗口。
 * 3. 使用“模态”等待机制，此时用户不能操作主窗口。
 */
void VoiceProGui::on_help() {
    auto& lm = LanguageManager::Instance();

    // --- 1. 从资源里捞数据 ---
    std::wstring text;
    HRSRC hRes = FindResourceW(hInstance_, MAKEINTRESOURCEW(IDR_HELP_TEXT), RT_RCDATA);
    if (hRes) {
        HGLOBAL hLog = LoadResource(hInstance_, hRes);
        if (hLog) {
            void* pData = LockResource(hLog);
            DWORD size = SizeofResource(hInstance_, hRes);
            if (pData && size > 0) {
                // 处理 UTF-8 编码，将其转为 Windows 常用的宽字符 (UTF-16)
                const unsigned char* pBytes = (const unsigned char*)pData;
                bool hasBOM = (size >= 3 && pBytes[0] == 0xEF && pBytes[1] == 0xBB && pBytes[2] == 0xBF);
                const char* pTextData = hasBOM ? (const char*)pData + 3 : (const char*)pData;
                int textLen = hasBOM ? size - 3 : size;

                int wideLen = MultiByteToWideChar(CP_UTF8, 0, pTextData, textLen, nullptr, 0);
                if (wideLen > 0) {
                    text.resize(wideLen);
                    MultiByteToWideChar(CP_UTF8, 0, pTextData, textLen, &text[0], wideLen);
                }
            }
        }
    }

    if (text.empty()) {
        text = lm.Get(L"err_load_help");
    } else {
        std::wstring full_text = text;
        std::wstring start_marker = lm.GetLanguage() == Language::English ? L"## English" : L"## 中文";
        std::wstring end_marker = lm.GetLanguage() == Language::English ? L"## 中文" : L"";
        size_t start = full_text.find(start_marker);
        if (start != std::wstring::npos) {
            size_t end = end_marker.empty() ? std::wstring::npos : full_text.find(end_marker, start + start_marker.size());
            text = full_text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        }
    }

    // --- 2. 注册并创建帮助小窗口 ---
    static const wchar_t* help_class = L"VoiceProHelp";
    WNDCLASSEXW wc = { sizeof(wc) };
    if (!GetClassInfoExW(hInstance_, help_class, &wc)) {
        wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
            if (msg == WM_SIZE) {
                HWND hEdit = GetWindow(hwnd, GW_CHILD);
                if (hEdit) {
                    RECT rc; GetClientRect(hwnd, &rc);
                    MoveWindow(hEdit, 10, 10, rc.right - 20, rc.bottom - 20, TRUE);
                }
            } else if (msg == WM_CLOSE) {
                DestroyWindow(hwnd);
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        };
        wc.hInstance = hInstance_;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIcon(hInstance_, MAKEINTRESOURCE(IDI_APP_ICON));
        wc.hIconSm = LoadIcon(hInstance_, MAKEINTRESOURCE(IDI_APP_ICON));
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = help_class;
        RegisterClassExW(&wc);
    }

    HWND hHelpDlg = CreateWindowExW(
        0, help_class, lm.Get(L"help_title").c_str(),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 500,
        hwnd_, nullptr, hInstance_, nullptr
    );

    if (!hHelpDlg) return;

    // 在帮助窗口里塞入一个大文本框
    RECT rc; GetClientRect(hHelpDlg, &rc);
    HWND hEdit = CreateWindowExW(
        0, L"EDIT", text.c_str(),
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        10, 10, rc.right - 20, rc.bottom - 20,
        hHelpDlg, nullptr, hInstance_, nullptr
    );

    SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont_, TRUE);

    // --- 3. 模态逻辑：禁用主窗口，开启独立小循环 ---
    EnableWindow(hwnd_, FALSE);

    MSG msg;
    while (IsWindow(hHelpDlg)) {
        if (GetMessage(&msg, nullptr, 0, 0)) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage((int)msg.wParam);
                break;
            }
            if (!IsDialogMessage(hHelpDlg, &msg)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
    }

    // 关掉小窗口后恢复主窗口操作
    EnableWindow(hwnd_, TRUE);
    SetActiveWindow(hwnd_);
}

/**
 * 回调：调节主总闸音量 (on_master_volume_change)
 */
void VoiceProGui::on_master_volume_change(int value) {
    float volume = value / 100.0f;
    engine_->set_master_volume(volume);
}

/**
 * 下拉框回调：切换扬声器效果 (on_output_device_change)
 */
void VoiceProGui::on_output_device_change() {
    selected_output_index_ = (int)SendMessage(combo_output_, CB_GETCURSEL, 0, 0);

    // 如果切换设备时引擎正在跑，我们需要重启引擎以应用新设备
    if (engine_->is_running()) {
        engine_->stop();
        if (selected_output_index_ >= 0 && selected_output_index_ < (int)output_devices_.size()) {
            engine_->start(output_devices_[selected_output_index_].id);
        }
    }
}
