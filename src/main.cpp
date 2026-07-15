// 包含 Windows 基础头文件，提供了诸如窗口管理、消息传递等核心 API
#include <windows.h>
// 包含项目自定义的 GUI 头文件，负责界面的显示和逻辑处理
#include "gui.h"

/**
 * Windows 程序的标准入口点函数 (取代了控制台程序中的 main 函数)
 * @param hInstance     当前应用程序实例的句柄。Windows 用它来在内存中标识我们的程序。
 * @param hPrevInstance 以前版本的 Windows 中用于标识前一个实例，在现代 Windows 中始终为 NULL。
 * @param lpCmdLine     一个字符串，包含了程序运行时的命令行参数（如果有）。
 * @param nCmdShow      一个整数标志，指示窗口应如何显示（例如：最小化、最大化或正常显示）。
 */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // 显式引用这些参数，防止编译器报“未使用变量”的警告
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    // CoInitializeEx 是初始化 COM (Component Object Model) 库的函数。
    // COM 是 Windows 的一种组件标准，本程序使用的 WASAPI (音频接口) 就是基于 COM 的。
    // nullptr: 表示由系统自动分配。
    // COINIT_MULTITHREADED: 开启多线程模式。音频处理通常在单独的线程运行，所以需要多线程支持。
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    
    // FAILED 宏用于检查函数返回的 HRESULT 是否表示失败（小于 0）
    if (FAILED(hr)) {
        // 如果 COM 初始化失败，弹出一个 Windows 标准消息框提示错误
        MessageBoxW(nullptr, L"COM 库初始化失败，程序无法启动。", L"系统错误", MB_OK | MB_ICONERROR);
        return 1; // 返回 1 表示异常退出
    }
    
    // 创建一个 GUI (图形用户界面) 对象
    // 这个对象在 gui.h 中定义，封装了窗口的所有操作
    VoiceProGui gui;
    
    // 调用 gui.create 方法来创建窗口
    // 传入 hInstance 让 GUI 层知道当前的程序实例
    if (!gui.create(hInstance)) {
        // 如果窗口创建失败（例如资源不足），则释放 COM 资源并退出
        CoUninitialize();
        return 1;
    }
    
    // 调用 gui.run 进入消息循环
    // 消息循环是 GUI 程序的核心：它不断检查是否有用户操作（点击、移动、按键），并分发处理
    // 只有当窗口关闭，消息循环结束时，该函数才会返回
    int result = gui.run();
    
    // 与 CoInitializeEx 对应，程序结束前必须调用此函数来释放 COM 库资源
    CoUninitialize();
    
    // 返回运行结果给 Windows 系统
    return result;
}
