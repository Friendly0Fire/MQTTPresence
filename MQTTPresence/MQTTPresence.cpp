#include "framework.h"
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <strsafe.h>
#include <Shlobj.h>
#include <Shlwapi.h>

#include "MQTTPresence.h"
#include "MQTTClient.h"

#include <fstream>
#include <vector>

#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <cxxopts.hpp>

#include "Registry.h"
#include "VolumeCheck.h"


#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Constants //
TCHAR g_startup_reg_key[] = TEXT("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run");
UINT const WMAPP_NOTIFYCALLBACK = WM_APP + 1;

// Load Once //
TCHAR g_program_path[MAX_PATH];
bool g_startup = false, g_elevated = false;
HINSTANCE g_hinst = nullptr;

// Main Loop //
std::atomic<bool> g_running = true;
std::atomic<bool> g_restart = true;

mqtt_client* g_mqtt = nullptr;
TCHAR g_config_dir[MAX_PATH];
TCHAR g_config_path[MAX_PATH];
HANDLE g_config_watch;
std::string g_mqtt_host, g_mqtt_port, g_mqtt_topic, g_mqtt_username, g_mqtt_password;
std::vector<std::string> g_volume_processes;
bool g_enable_volume = true, g_enable_activity = true;
bool g_volume_check_all_devices = false;

bool g_user_active = true, g_sound_active = false;


template <typename... Args>
void fatal_message_box(Args&&... args) {
    MessageBox(std::forward<Args>(args)...);
    exit(1);
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
    if (FAILED(SHGetFolderPath(nullptr, CSIDL_APPDATA, nullptr, 0, g_config_dir)))
        fatal_message_box(nullptr, TEXT("Could not locate AppData folder."), TEXT("Fatal Error"), MB_OK | MB_ICONERROR);

    PathAppend(g_config_dir, CHOOSE_TSTR(g_unique_identifier));

    auto ret = SHCreateDirectory(nullptr, g_config_dir);
    if (ret != ERROR_SUCCESS && ret != ERROR_ALREADY_EXISTS)
        fatal_message_box(nullptr, TEXT("Could not create config folder."), TEXT("Fatal Error"), MB_OK | MB_ICONERROR);

    _tcscpy_s(g_config_path, g_config_dir);
    PathAppend(g_config_path, TEXT("config.json"));

    if (PathFileExists(g_config_path)) {
        std::ifstream config(g_config_path);
        rapidjson::IStreamWrapper config_isw(config);
        rapidjson::Document doc;
        doc.ParseStream(config_isw);

        auto load_val = [&](const char* name, const std::string& def) {
            if(doc.HasMember(name) && doc[name].IsString())
                return std::string(doc[name].GetString());
            else
                return def;
        };

        g_mqtt_host = load_val("mqttHost", "localhost");
        g_mqtt_port = load_val("mqttPort", "1883");
        g_mqtt_topic = load_val("mqttTopic", "winmqttpresence");
        g_mqtt_username = load_val("mqttUsername", "");
        g_mqtt_password = load_val("mqttPassword", "");

        if(doc.HasMember("volumeProcesses")) {
            const auto& processes = doc["volumeProcesses"];
            if(processes.IsArray()) {
                for(const auto& proc : processes.GetArray()) {
                    if(proc.IsString())
                        g_volume_processes.push_back(proc.GetString());
                }
            }
        }

        auto load_bool = [&](const char* name, bool def) {
            return load_val(name, def ? "true" : "false") == "true";
        };
        
        g_enable_volume = load_bool("enableVolumeCheck", true);
        g_enable_activity = load_bool("enableActivityCheck", true);
        g_volume_check_all_devices = load_bool("enableVolumeCheckAllDevices", false);
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
    }

    g_config_watch = FindFirstChangeNotification(g_config_dir, true, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
}

void parse_options(int argc, LPWSTR* wargv) {
    std::vector<std::string> argv(argc);
    std::transform(wargv, &wargv[argc - 1], std::begin(argv), [](LPWSTR p) { return ws2s(p); });
    std::vector<char*> rargv(argc);
    std::transform(std::begin(argv), std::end(argv), std::begin(rargv), [](const std::string& s) {
        return const_cast<char*>(s.c_str());
    });
    auto* raw_argv = rargv.data();

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
    NOTIFYICONDATA nid = { sizeof(NOTIFYICONDATA) };
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

void SetNotificationIconTooltip(HWND hwnd, const TCHAR* msg) {
    NOTIFYICONDATA nid = { sizeof(NOTIFYICONDATA) };
    nid.hWnd = hwnd;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    nid.uID = IDI_NOTIFICATIONICON;
    _tcscpy_s(nid.szTip, msg);

    Shell_NotifyIcon(NIM_MODIFY, &nid);
}

void SetNotificationIconMessage(HWND hwnd, const TCHAR* msg) {
    NOTIFYICONDATA nid = { sizeof(NOTIFYICONDATA) };
    nid.hWnd = hwnd;
    nid.uFlags = NIF_INFO;
    nid.uID = IDI_NOTIFICATIONICON;
    _tcscpy_s(nid.szInfo, msg);
    LoadString(g_hinst, IDS_TOOLTIP, nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle));
    
    Shell_NotifyIcon(NIM_MODIFY, &nid);
}

BOOL DeleteNotificationIcon(HWND hwnd) {
    NOTIFYICONDATA nid = { sizeof(NOTIFYICONDATA) };
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
    case WM_QUERYENDSESSION:
        return true;
    case WM_ENDSESSION:
        if (!wParam)
            break;

        g_running = false;
        g_restart = false;
        g_mqtt->disconnect();
        g_mqtt = nullptr;
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

void main_loop(HINSTANCE hInstance) {
    load_config();

    WCHAR window_title[100];
    LoadString(hInstance, IDS_APP_TITLE, window_title, ARRAYSIZE(window_title));
    HWND hwnd = CreateWindow(CHOOSE_TSTR(g_unique_identifier), window_title, WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, 0, 250, 200, NULL, NULL, g_hinst, nullptr);
    if (hwnd) {
        auto* powerNotify = g_enable_activity ? RegisterPowerSettingNotification(hwnd, &GUID_SESSION_USER_PRESENCE, DEVICE_NOTIFY_WINDOW_HANDLE) : nullptr;

        ShowWindow(hwnd, SW_HIDE);

        mqtt_client mqtt(g_mqtt_host, g_mqtt_port, g_mqtt_username, g_mqtt_password, g_mqtt_topic);
        g_mqtt = &mqtt;

        // ReSharper disable once CppJoinDeclarationAndAssignment
        volume_check volume;
        std::atomic<bool> volume_thread_signal = true;
        std::thread volume_thread;
        if(g_enable_volume) {
            // ReSharper disable once CppJoinDeclarationAndAssignment
            volume = volume_check(g_volume_check_all_devices);
            for(const auto& proc : g_volume_processes)
                volume.add_process_name(s2ws(proc));

            volume_thread = std::thread([&volume, &volume_thread_signal]() {
                using namespace std::chrono_literals;
                std::this_thread::sleep_for(5s);

                while(volume_thread_signal) {
                    bool new_active = volume.poll();
                    if(new_active != g_sound_active) {
                        g_sound_active = new_active;
                        if(g_mqtt)
                            g_mqtt->sound_active(g_sound_active);
                    }
                    std::this_thread::sleep_for(5s);
                }
            });
        }
        
        std::atomic<bool> config_thread_signal = true;
        std::thread config_thread = std::thread([&config_thread_signal, hwnd]() {
            while(config_thread_signal) {
                DWORD wait_status = WaitForSingleObject(g_config_watch, 1000);
                if(wait_status == WAIT_OBJECT_0) {
                    SetNotificationIconMessage(hwnd, TEXT("Configuration reloading..."));
                    g_running = false;
                    g_restart = true;

                    FindNextChangeNotification(g_config_watch);
                    break;
                }
            }
        });

        mqtt.connect();

        bool last_sound_active = false;
        bool last_user_active = false;
        MSG msg;
        while (g_running) {
            if(mqtt.status() == mqtt_status::DISCONNECTED) {
                g_running = false;
                g_restart = true;
                SetNotificationIconMessage(hwnd, TEXT("Connection lost, reconnecting..."));
                break;
            }

            if (MsgWaitForMultipleObjects(0, nullptr, false, 1000, QS_ALLINPUT) == WAIT_OBJECT_0) {
                while(PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                    if(msg.message == WM_QUIT) {
                        g_running = false;
                        break;
                    }
                }
            }

            if (last_sound_active != g_sound_active || last_user_active != g_user_active) {
                last_sound_active = g_sound_active;
                last_user_active = g_user_active;

                auto notification = std::format(L"User active: {}\nSound active: {}", g_user_active, g_sound_active);

                SetNotificationIconTooltip(hwnd, notification.c_str());
            }
        }

        volume_thread_signal = false;
        if(volume_thread.joinable())
            volume_thread.join();

        config_thread_signal = false;
        config_thread.join();

        FindCloseChangeNotification(g_config_watch);

        if(g_enable_activity)
            UnregisterPowerSettingNotification(powerNotify);

        if (g_mqtt) {
            g_mqtt->sound_active(false);
            g_mqtt->user_active(false);
        }

        DestroyWindow(hwnd);
    }
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int /*nCmdShow*/) {
    if (!SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
    {
        MessageBox(nullptr, TEXT("Could not initialize COM library!"), TEXT("Fatal Error"), MB_OK | MB_ICONERROR);
        exit(1);
    }

    GetModuleFileName(nullptr, g_program_path, MAX_PATH);
    g_hinst = hInstance;
    g_startup = get_startup();
    g_elevated = is_elevated();

    {
        int argc;
        auto argv = CommandLineToArgvW(lpCmdLine, &argc);
        parse_options(argc, argv);
        LocalFree(argv);
    }
    
    RegisterWindowClass(g_unique_identifierW, MAKEINTRESOURCE(IDC_MQTTPRESENCE), WndProc);

    while(g_restart) {
        g_running = true;
        g_restart = false;
        main_loop(hInstance);
    }

    CoUninitialize();

    return 0;
}