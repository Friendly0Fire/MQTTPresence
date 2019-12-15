#pragma once
#include <wrl/client.h>
#include <mmdeviceapi.h>
#include <vector>
#include <audiopolicy.h>
#include <string>
#include <endpointvolume.h>
#include <unordered_set>

class volume_check {
public:
    volume_check() = default;

    explicit volume_check(bool check_all_devices) {
        using namespace Microsoft::WRL;

        ComPtr<IMMDeviceEnumerator> device_enumerator;
        if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(device_enumerator.GetAddressOf()))))
            return;

        if(check_all_devices) {
            ComPtr<IMMDeviceCollection> devices;
            device_enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.GetAddressOf());
            unsigned int device_count;
            devices->GetCount(&device_count);
            for(unsigned int dev_id = 0; dev_id < device_count; dev_id++) {
                auto& dev = devices_.emplace_back();
                devices->Item(dev_id, dev.GetAddressOf());
            }
        } else {
            auto& dev = devices_.emplace_back();
            device_enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, dev.GetAddressOf());
        }
    }

    void add_process_name(const std::wstring& name) {
        proc_names_.insert(name);
    }

    [[nodiscard]] bool poll() const {
        for(const auto& device : devices_)
            if(poll_device(device))
                return true;

        return false;
    }

private:
    [[nodiscard]] bool poll_device(const Microsoft::WRL::ComPtr<IMMDevice>& device) const {
        using namespace Microsoft::WRL;

        ComPtr<IAudioSessionManager2> session_manager;
        device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                      reinterpret_cast<void**>(session_manager.GetAddressOf()));
        ComPtr<IAudioSessionEnumerator> enumerator;
        session_manager->GetSessionEnumerator(enumerator.GetAddressOf());

        int session_count;
        enumerator->GetCount(&session_count);
        for (int session_index = 0; session_index < session_count; session_index++) {
            ComPtr<IAudioSessionControl> session_control;
            if (FAILED(enumerator->GetSession(session_index, session_control.GetAddressOf())))
                continue;
            ComPtr<IAudioSessionControl2> session_control2;
            if (FAILED(session_control->QueryInterface(IID_PPV_ARGS(session_control2.GetAddressOf()))))
                continue;
            ComPtr<IAudioMeterInformation> meter_information;
            if(FAILED(session_control->QueryInterface(IID_PPV_ARGS(meter_information.GetAddressOf()))))
                continue;

            float proc_value = 0.f;
            meter_information->GetPeakValue(&proc_value);
            if(proc_value < 0.00001f)
                continue;
            
            std::wstring proc_name;
            DWORD pid;
            session_control2->GetProcessId(&pid);
            HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
            if(proc) {
                wchar_t proc_path[MAX_PATH];
                DWORD proc_len = MAX_PATH;
                if(QueryFullProcessImageNameW(proc, 0, proc_path, &proc_len)) {
                    wchar_t filename[MAX_PATH];
                    wchar_t fileext[MAX_PATH];
                    _wsplitpath_s(proc_path, nullptr, 0, nullptr, 0, filename, MAX_PATH, fileext, MAX_PATH);
                    proc_name = filename;
                    proc_name += fileext;
                }
                CloseHandle(proc);
            }

            if(proc_name.empty())
                continue;

            if(proc_names_.empty() || proc_names_.count(proc_name) > 0)
                return true;
        }

        return false;
    }

    std::vector<Microsoft::WRL::ComPtr<IMMDevice>> devices_;
    std::unordered_set<std::wstring> proc_names_;
};
