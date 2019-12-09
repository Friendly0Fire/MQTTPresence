// we need commctrl v6 for LoadIconMetric()
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "comctl32.lib")

#define _WIN32_WINNT 0x0A00

#include "Resource.h"
#include <mqtt_client_cpp.hpp>
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <strsafe.h>
#include <Shlobj.h>
#include <Shlwapi.h>
#include <fstream>

HINSTANCE g_hInst = nullptr;

UINT const WMAPP_NOTIFYCALLBACK = WM_APP + 1;

wchar_t const szWindowClass[] = L"MQTTPresence";

TCHAR g_configPath[MAX_PATH];
std::string g_mqttHost, g_mqttPort, g_mqttTopic, g_mqttUsername, g_mqttPassword;

auto MakeMQTTClient(boost::asio::io_context& ioc)
{
    auto c = MQTT_NS::make_sync_client(ioc, g_mqttHost, g_mqttPort);
    using packet_id_t = typename std::remove_reference_t<decltype(*c)>::packet_id_t;

    c->set_client_id("MQTTPresenceWindows");
    if(!g_mqttUsername.empty())
        c->set_user_name(g_mqttUsername);
    if(!g_mqttPassword.empty())
        c->set_password(g_mqttPassword);
    c->set_clean_session(true);

    c->set_connack_handler(
        [c](bool sp, MQTT_NS::connect_return_code connack_return_code) {
            c->publish(g_mqttTopic + "/active", "true");
            return true;
        }
    );
    
    c->connect();

    return c;
}

// Forward declarations of functions included in this code module:
void                RegisterWindowClass(PCWSTR pszClassName, PCWSTR pszMenuName, WNDPROC lpfnWndProc);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
void                ShowContextMenu(HWND hwnd, POINT pt);
BOOL                AddNotificationIcon(HWND hwnd);
BOOL                DeleteNotificationIcon(HWND hwnd);

template<typename... Args>
void FatalMessageBox(Args&&... args)
{
    MessageBox(std::forward<Args>(args)...);
    exit(1);
}

std::string from_substr(const c4::csubstr& s)
{
    return std::string(s.str, s.size());
}

void SetupConfig()
{
    if(FAILED(SHGetFolderPath(nullptr, CSIDL_APPDATA, nullptr, 0, g_configPath)))
        FatalMessageBox(nullptr, TEXT("Could not locate AppData folder."), TEXT("Fatal Error"), MB_OK | MB_ICONERROR);

    PathAppend(g_configPath, TEXT("MQTTPresence"));

    auto ret = SHCreateDirectory(nullptr, g_configPath);
    if(ret != ERROR_SUCCESS && ret != ERROR_ALREADY_EXISTS)
        FatalMessageBox(nullptr, TEXT("Could not create config folder."), TEXT("Fatal Error"), MB_OK | MB_ICONERROR);

    PathAppend(g_configPath, TEXT("config.json"));

    if(PathFileExists(g_configPath))
    {
        std::ifstream config(g_configPath);
        std::stringstream ss;
        ss << config.rdbuf();
        auto tree = ryml::parse(c4::to_csubstr(ss.str()));
        g_mqttHost = from_substr(tree["mqttHost"].val());
        g_mqttPort = from_substr(tree["mqttPort"].val());
        g_mqttTopic = from_substr(tree["mqttTopic"].val());
        g_mqttUsername = from_substr(tree["mqttUsername"].val());
        g_mqttPassword = from_substr(tree["mqttPassword"].val());
    }
    else
    {
        std::ofstream out(g_configPath);
        out << R"MARK(
{
    "mqttHost": "localhost",
    "mqttPort": 1883,
    "mqttUsername": "",
    "mqttPassword": "",
    "mqttTopic": "mqttpresence"
}
)MARK";
        out.close();
        if(MessageBox(nullptr, TEXT("A config file has been generated. Click OK to open it."), TEXT("Configuration Required"), MB_OKCANCEL | MB_ICONINFORMATION) == IDOK)
            _wsystem(g_configPath);
        exit(0);
    }
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR /*lpCmdLine*/, int /*nCmdShow*/)
{
    g_hInst = hInstance;
    RegisterWindowClass(szWindowClass, MAKEINTRESOURCE(IDC_MQTTPRESENCE), WndProc);

    // Create the main window. This could be a hidden window if you don't need
    // any UI other than the notification icon.
    WCHAR szTitle[100];
    LoadString(hInstance, IDS_APP_TITLE, szTitle, ARRAYSIZE(szTitle));
    HWND hwnd = CreateWindow(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, 250, 200, NULL, NULL, g_hInst, nullptr);
    if (hwnd)
    {
        ShowWindow(hwnd, SW_HIDE);

        SetupConfig();

        boost::asio::io_context ioc;
        auto mqtt_client = MakeMQTTClient(ioc);

        std::thread mqtt_thread([&]()
        {
            ioc.run();
        });

        // Main message loop:
        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        
        mqtt_client->publish(g_mqttTopic + "/active", "false");
        ioc.stop();
        mqtt_thread.join();
    }
    return 0;
}

void RegisterWindowClass(PCWSTR pszClassName, PCWSTR pszMenuName, WNDPROC lpfnWndProc)
{
    WNDCLASSEX wcex     = {sizeof(wcex)};
    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = lpfnWndProc;
    wcex.hInstance      = g_hInst;
    wcex.hIcon          = LoadIcon(g_hInst, MAKEINTRESOURCE(IDI_NOTIFICATIONICON));
    wcex.hCursor        = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
    wcex.lpszMenuName   = pszMenuName;
    wcex.lpszClassName  = pszClassName;
    RegisterClassEx(&wcex);
}

BOOL AddNotificationIcon(HWND hwnd)
{
    NOTIFYICONDATA nid = {sizeof(nid)};
    nid.hWnd = hwnd;
    // add the icon, setting the icon, tooltip, and callback message.
    // the icon will be identified with the GUID
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uID = IDI_NOTIFICATIONICON;
    nid.uCallbackMessage = WMAPP_NOTIFYCALLBACK;
    LoadIconMetric(g_hInst, MAKEINTRESOURCE(IDI_NOTIFICATIONICON), LIM_SMALL, &nid.hIcon);
    LoadString(g_hInst, IDS_TOOLTIP, nid.szTip, ARRAYSIZE(nid.szTip));
    Shell_NotifyIcon(NIM_ADD, &nid);

    // NOTIFYICON_VERSION_4 is prefered
    nid.uVersion = NOTIFYICON_VERSION_4;
    return Shell_NotifyIcon(NIM_SETVERSION, &nid);
}

BOOL DeleteNotificationIcon(HWND hwnd)
{
    NOTIFYICONDATA nid = {sizeof(nid)};
    nid.hWnd = hwnd;
    nid.uID = IDI_NOTIFICATIONICON;
    return Shell_NotifyIcon(NIM_DELETE, &nid);
}

void ShowContextMenu(HWND hwnd, POINT pt)
{
    HMENU hMenu = LoadMenu(g_hInst, MAKEINTRESOURCE(IDC_CONTEXTMENU));
    if (hMenu)
    {
        HMENU hSubMenu = GetSubMenu(hMenu, 0);
        if (hSubMenu)
        {
            // our window must be foreground before calling TrackPopupMenu or the menu will not disappear when the user clicks away
            SetForegroundWindow(hwnd);

            // respect menu drop alignment
            UINT uFlags = TPM_RIGHTBUTTON;
            if (GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0)
            {
                uFlags |= TPM_RIGHTALIGN;
            }
            else
            {
                uFlags |= TPM_LEFTALIGN;
            }

            TrackPopupMenuEx(hSubMenu, uFlags, pt.x, pt.y, hwnd, nullptr);
        }
        DestroyMenu(hMenu);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    static HWND s_hwndFlyout = nullptr;
    static BOOL s_fCanShowFlyout = TRUE;

    switch (message)
    {
    case WM_CREATE:
        // add the notification icon
        if (!AddNotificationIcon(hwnd))
            return -1;
        break;
    case WM_COMMAND:
        {
            int const wmId = LOWORD(wParam);
            // Parse the menu selections:
            switch (wmId)
            {
            case IDM_SETTINGS:
                _wsystem(g_configPath);
                break;
            case IDM_EXIT:
                DestroyWindow(hwnd);
                break;

            default:
                return DefWindowProc(hwnd, message, wParam, lParam);
            }
        }
        break;

    case WMAPP_NOTIFYCALLBACK:
        switch (LOWORD(lParam))
        {
        case WM_CONTEXTMENU:
            {
                POINT const pt = { GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam) };
                ShowContextMenu(hwnd, pt);
            }
            break;
        }
        break;

    case WM_DESTROY:
        DeleteNotificationIcon(hwnd);
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
    return 0;
}