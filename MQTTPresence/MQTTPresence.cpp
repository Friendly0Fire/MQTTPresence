#include "MQTTPresence.h"
#include "MQTTClient.h"

#include <ryml.hpp>
#include <ryml_std.hpp>
#include <windows.h>
#include <windowsx.h>
#include <winnt.h>
#include <shellapi.h>
#include <commctrl.h>
#include <strsafe.h>
#include <Shlobj.h>
#include <Shlwapi.h>
#include <fstream>
#include <cxxopts.hpp>
#include <tchar.h>
#include <vector>
#include "Registry.h"
#include "VolumeCheck.h"

#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

TCHAR g_startup_reg_key[] = TEXT("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run");
TCHAR g_program_path[MAX_PATH];

HINSTANCE g_hinst = nullptr;

UINT const WMAPP_NOTIFYCALLBACK = WM_APP + 1;

mqtt_client* g_mqtt = nullptr;
TCHAR g_config_path[MAX_PATH];
std::string g_mqtt_host, g_mqtt_port, g_mqtt_topic, g_mqtt_username, g_mqtt_password;
std::vector<std::string> g_volume_processes;
bool g_startup = false, g_elevated = false;
bool g_enable_volume = true, g_enable_activity = true;
bool g_volume_check_all_devices = false;

bool g_user_active = true, g_sound_active = false;

// Forward declarations of functions included in this code module:
void RegisterWindowClass(PCWSTR pszClassName, PCWSTR pszMenuName, WNDPROC lpfnWndProc);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void ShowContextMenu(HWND hwnd, POINT pt);
BOOL AddNotificationIcon(HWND hwnd);
BOOL DeleteNotificationIcon(HWND hwnd);

template <typename... Args>
void fatal_message_box(Args&&... args) {
    MessageBox(std::forward<Args>(args)...);
    exit(1);
}

std::string from_substr(const c4::csubstr& s) {
    return std::string(s.str, s.size());
}

enum class elevated_commands_t {
    SET_STARTUP_ON = 1,
    SET_STARTUP_OFF = 2,
};

bool is_elevated() {
    bool ret = false;
    HANDLE h_token = nullptr;
    if(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY, &h_token)) {
        TOKEN_ELEVATION elevation;
        DWORD cb_size = sizeof(TOKEN_ELEVATION);
        if(GetTokenInformation(h_token, TokenElevation, &elevation, sizeof(elevation), &cb_size)) {
            ret = elevation.TokenIsElevated != 0;
        }
    }
    if(h_token)
        CloseHandle(h_token);
    return ret;
}

void open_file(TCHAR* path) {
    SHELLEXECUTEINFO i { sizeof(SHELLEXECUTEINFO) };
    i.fMask = SEE_MASK_DEFAULT;
    i.hwnd = nullptr;
    i.lpVerb = TEXT("open");
    i.lpFile = g_config_path;
    i.lpParameters = nullptr;
    i.lpDirectory = nullptr;
    i.nShow = SW_SHOWNORMAL;
    ShellExecuteEx(&i);

    exit(0);
}

void launch_elevated(elevated_commands_t cmds) {
    std::basic_string<TCHAR> command_string = TEXT("--cmd");
    if(cmds == elevated_commands_t::SET_STARTUP_ON)
        command_string += TEXT(" --startup");
    else if(cmds == elevated_commands_t::SET_STARTUP_OFF)
        command_string += TEXT(" --no-startup");

    SHELLEXECUTEINFO i { sizeof(SHELLEXECUTEINFO) };
    i.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NO_CONSOLE | SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS;
    i.hwnd = nullptr;
    i.lpVerb = TEXT("runas");
    i.lpFile = g_program_path;
    i.lpParameters = command_string.c_str();
    i.lpDirectory = nullptr;
    i.nShow = SW_HIDE;
    ShellExecuteEx(&i);
    WaitForSingleObject(i.hProcess, INFINITE);
    DWORD exit_code;
    GetExitCodeProcess(i.hProcess, &exit_code);

    if(exit_code != 0)
        throw errcode_exception(exit_code);
}

bool get_startup() {
    registry_key key(HKEY_CURRENT_USER, g_startup_reg_key);
    auto current_path = key.get_value(CHOOSE_TSTR(g_unique_identifier));
    return current_path == g_program_path;
}

void set_startup(bool startup) {
    try {
        registry_key key(HKEY_CURRENT_USER, g_startup_reg_key);
        if (startup) {
            key.set_value(CHOOSE_TSTR(g_unique_identifier), REG_SZ, g_program_path);
            g_startup = true;
        }
        else {
            key.delete_value(CHOOSE_TSTR(g_unique_identifier));
            g_startup = false;
        }
    } catch(const errcode_exception& e) {
        if(!g_elevated && e.code() == ERROR_ACCESS_DENIED)
            launch_elevated(startup ? elevated_commands_t::SET_STARTUP_ON : elevated_commands_t::SET_STARTUP_OFF);
        else
            throw e;
    }
}

inline void toggle_startup() {
    set_startup(!get_startup());
}

void load_config() {
    if (FAILED(SHGetFolderPath(nullptr, CSIDL_APPDATA, nullptr, 0, g_config_path)))
        fatal_message_box(nullptr, TEXT("Could not locate AppData folder."), TEXT("Fatal Error"), MB_OK | MB_ICONERROR);

    PathAppend(g_config_path, CHOOSE_TSTR(g_unique_identifier));

    auto ret = SHCreateDirectory(nullptr, g_config_path);
    if (ret != ERROR_SUCCESS && ret != ERROR_ALREADY_EXISTS)
        fatal_message_box(nullptr, TEXT("Could not create config folder."), TEXT("Fatal Error"), MB_OK | MB_ICONERROR);

    PathAppend(g_config_path, TEXT("config.json"));

    if (PathFileExists(g_config_path)) {
        std::ifstream config(g_config_path);
        std::stringstream ss;
        ss << config.rdbuf();
        auto tree = ryml::parse(c4::to_csubstr(ss.str()));

        auto read_node = [](const c4::yml::NodeRef& node, const std::string& def) {
            if(node.valid() && !node.is_seed() && node.has_val())
                return from_substr(node.val());
            else
                return def;
        };

        g_mqtt_host = read_node(tree["mqttHost"], "localhost");
        g_mqtt_port = read_node(tree["mqttPort"], "1883");
        g_mqtt_topic = read_node(tree["mqttTopic"], "winmqttpresence");
        g_mqtt_username = read_node(tree["mqttUsername"], "");
        g_mqtt_password = read_node(tree["mqttPassword"], "");

        auto processes = tree["volumeProcesses"];
        if(processes.valid() && !processes.is_seed() && processes.is_seq()) {
            for(auto proc : processes.children())
                g_volume_processes.push_back(from_substr(proc.val()));
        }

        auto read_node_bool = [](const c4::yml::NodeRef& node, bool def) {
            if(node.valid() && !node.is_seed() && node.has_val())
                return node.val() == "true";
            else
                return def;
        };
        
        g_enable_volume = read_node_bool(tree["enableVolumeCheck"], true);
        g_enable_activity = read_node_bool(tree["enableActivityCheck"], true);
        g_volume_check_all_devices = read_node_bool(tree["enableVolumeCheckAllDevices"], false);
    }
    else {
        std::ofstream out(g_config_path);
        out <<
            R"MARK(
{
    "mqttHost": "localhost", // defaults to 'localhost'
    "mqttPort": 1883, // defaults to 1883
    "mqttUsername": "", // remove or leave blank if unneeded
    "mqttPassword": "", // remove or leave blank if unneeded
    "mqttTopic": "winmqttpresence", // defaults to 'winmqttpresence'
    "enableVolumeCheck": true, // defaults to true
    "volumeProcesses": [], // if empty, all processes are checked; otherwise, only check a list of executables (with extensions)
    "enableActivityCheck": true, // defaults to true
    "enableVolumeCheckAllDevices": false // defaults to false; if true, all audio output devices are checked for sound output, otherwise only the default one is
}
)MARK";
        out.close();
        if (MessageBox(nullptr, TEXT("A config file has been generated. Click OK to open it."),
                       TEXT("Configuration Required"), MB_OKCANCEL | MB_ICONINFORMATION) == IDOK)
            open_file(g_config_path);
        exit(0);
    }
}

void parse_options(int argc, LPWSTR* wargv) {
    std::vector<std::string> argv(argc);
    std::transform(wargv, &wargv[argc - 1], std::begin(argv), [](LPWSTR p) { return ws2s(p); });
    std::vector<char*> rargv(argc);
    std::transform(std::begin(argv), std::end(argv), std::begin(rargv), [](const std::string& s) {
        return const_cast<char*>(s.c_str());
    });
    auto raw_argv = rargv.data();

    cxxopts::Options options(g_unique_identifier, "");
    options.add_options()
        ("s,startup", "Launch on startup")
        ("n,no-startup", "Do not launch on startup")
        ("c,cmd", "Command line action only, will not install to tray");

    auto result = options.parse(argc, raw_argv);
    
    if (result["c"].as<bool>()) {
        if (result["s"].as<bool>() || result["n"].as<bool>())
            set_startup(result["s"].as<bool>());

        exit(0);
    }
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int /*nCmdShow*/) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    GetModuleFileName(nullptr, g_program_path, MAX_PATH);
    g_startup = get_startup();
    g_elevated = is_elevated();

    {
        int argc;
        auto argv = CommandLineToArgvW(lpCmdLine, &argc);
        parse_options(argc, argv);
        LocalFree(argv);
    }

    load_config();

    g_hinst = hInstance;
    RegisterWindowClass(g_unique_identifierW, MAKEINTRESOURCE(IDC_MQTTPRESENCE), WndProc);

    // Create the main window. This could be a hidden window if you don't need
    // any UI other than the notification icon.
    WCHAR window_title[100];
    LoadString(hInstance, IDS_APP_TITLE, window_title, ARRAYSIZE(window_title));
    HWND hwnd = CreateWindow(CHOOSE_TSTR(g_unique_identifier), window_title, WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, 0, 250, 200, NULL, NULL, g_hinst, nullptr);
    if (hwnd) {
        auto powerNotify = g_enable_activity ? RegisterPowerSettingNotification(hwnd, &GUID_SESSION_USER_PRESENCE, DEVICE_NOTIFY_WINDOW_HANDLE) : nullptr;

        ShowWindow(hwnd, SW_HIDE);

        mqtt_client mqtt(g_mqtt_host, g_mqtt_port, g_mqtt_username, g_mqtt_password, g_mqtt_topic);
        g_mqtt = &mqtt;

        volume_check volume;
        std::atomic<bool> volume_thread_signal = true;
        std::thread volume_thread;
        if(g_enable_volume) {
            volume = volume_check(g_volume_check_all_devices);
            for(const auto& proc : g_volume_processes)
                volume.add_process_name(s2ws(proc));

            volume_thread = std::thread([&volume, &volume_thread_signal]() {
                using namespace std::chrono_literals;

                while(volume_thread_signal) {
                    bool new_active = volume.poll();
                    if(new_active != g_sound_active) {
                        g_sound_active = new_active;
                        g_mqtt->sound_active(g_sound_active);
                    }
                    std::this_thread::sleep_for(5s);
                }
            });
        }

        mqtt.connect();

        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if(g_enable_volume)
            volume_thread_signal = false;

        if(g_enable_activity)
            UnregisterPowerSettingNotification(powerNotify);
    }

    CoUninitialize();

    return 0;
}

void RegisterWindowClass(PCWSTR pszClassName, PCWSTR pszMenuName, WNDPROC lpfnWndProc) {
    WNDCLASSEX wcex = {sizeof(wcex)};
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = lpfnWndProc;
    wcex.hInstance = g_hinst;
    wcex.hIcon = LoadIcon(g_hinst, MAKEINTRESOURCE(IDI_NOTIFICATIONICON));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = HBRUSH(COLOR_WINDOW + 1);
    wcex.lpszMenuName = pszMenuName;
    wcex.lpszClassName = pszClassName;
    RegisterClassEx(&wcex);
}

BOOL AddNotificationIcon(HWND hwnd) {
    NOTIFYICONDATA nid = {sizeof(nid)};
    nid.hWnd = hwnd;
    // add the icon, setting the icon, tooltip, and callback message.
    // the icon will be identified with the GUID
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uID = IDI_NOTIFICATIONICON;
    nid.uCallbackMessage = WMAPP_NOTIFYCALLBACK;
    LoadIconMetric(g_hinst, MAKEINTRESOURCE(IDI_NOTIFICATIONICON), LIM_SMALL, &nid.hIcon);
    LoadString(g_hinst, IDS_TOOLTIP, nid.szTip, ARRAYSIZE(nid.szTip));
    Shell_NotifyIcon(NIM_ADD, &nid);

    // NOTIFYICON_VERSION_4 is prefered
    nid.uVersion = NOTIFYICON_VERSION_4;
    return Shell_NotifyIcon(NIM_SETVERSION, &nid);
}

BOOL DeleteNotificationIcon(HWND hwnd) {
    NOTIFYICONDATA nid = {sizeof(nid)};
    nid.hWnd = hwnd;
    nid.uID = IDI_NOTIFICATIONICON;
    return Shell_NotifyIcon(NIM_DELETE, &nid);
}

void ShowContextMenu(HWND hwnd, POINT pt) {
    HMENU hMenu = LoadMenu(g_hinst, MAKEINTRESOURCE(IDC_CONTEXTMENU));
    if (hMenu) {
        HMENU hSubMenu = GetSubMenu(hMenu, 0);
        if (hSubMenu) {
            // our window must be foreground before calling TrackPopupMenu or the menu will not disappear when the user clicks away
            SetForegroundWindow(hwnd);

            // respect menu drop alignment
            UINT uFlags = TPM_RIGHTBUTTON;
            if (GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0) {
                uFlags |= TPM_RIGHTALIGN;
            }
            else {
                uFlags |= TPM_LEFTALIGN;
            }
            
            CheckMenuItem(hSubMenu, IDM_STARTUP, get_startup() ? MF_CHECKED : MF_UNCHECKED);
            TrackPopupMenuEx(hSubMenu, uFlags, pt.x, pt.y, hwnd, nullptr);
        }
        DestroyMenu(hMenu);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static HWND s_hwndFlyout = nullptr;
    static BOOL s_fCanShowFlyout = TRUE;

    switch (message) {
    case WM_CREATE:
        // add the notification icon
        if (!AddNotificationIcon(hwnd))
            return -1;
        break;
    case WM_COMMAND: {
        int const wmId = LOWORD(wParam);
        // Parse the menu selections:
        switch (wmId) {
        case IDM_STARTUP: {
            toggle_startup();
            break;
        }
        case IDM_SETTINGS:
            open_file(g_config_path);
            break;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            break;

        default:
            return DefWindowProc(hwnd, message, wParam, lParam);
        }
    } break;
    case WMAPP_NOTIFYCALLBACK:
        switch (LOWORD(lParam)) {
        case WM_CONTEXTMENU: {
            POINT const pt = {GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)};
            ShowContextMenu(hwnd, pt);
        } break;
        } break;
    case WM_DESTROY:
        DeleteNotificationIcon(hwnd);
        PostQuitMessage(0);
        break;
    case WM_POWERBROADCAST: {
        if(wParam == PBT_POWERSETTINGCHANGE) {
            auto data = reinterpret_cast<POWERBROADCAST_SETTING*>(lParam);
            bool new_active = *reinterpret_cast<DWORD*>(&data->Data) != PowerUserInactive;
            if(new_active != g_user_active) {
                g_user_active = new_active;
                g_mqtt->user_active(g_user_active);
            }
        }
    } break;
    default:
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
    return 0;
}
