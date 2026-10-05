#include <windows.h>

namespace
{
    constexpr wchar_t WindowClassName[] = L"SampleCppWindowClass";

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            HDC deviceContext = BeginPaint(window, &paint);

            RECT clientArea{};
            GetClientRect(window, &clientArea);
            HGDIOBJ previousFont = SelectObject(deviceContext, GetStockObject(DEFAULT_GUI_FONT));
            SetBkMode(deviceContext, TRANSPARENT);
            DrawTextW(deviceContext, L"Hello from Visual Studio C++!", -1, &clientArea,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(deviceContext, previousFont);

            EndPaint(window, &paint);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    WNDCLASSW windowClass{};
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = WindowClassName;

    if (!RegisterClassW(&windowClass))
    {
        MessageBoxW(nullptr, L"Could not register the window class.", L"Sample C++ Window", MB_OK | MB_ICONERROR);
        return 1;
    }

    HWND window = CreateWindowExW(
        0, WindowClassName, L"Sample C++ Window", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 640, 400,
        nullptr, nullptr, instance, nullptr);

    if (!window)
    {
        MessageBoxW(nullptr, L"Could not create the window.", L"Sample C++ Window", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    BOOL result = 0;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (result == -1)
    {
        MessageBoxW(window, L"Could not read a window message.", L"Sample C++ Window", MB_OK | MB_ICONERROR);
        return 1;
    }

    return static_cast<int>(message.wParam);
}
