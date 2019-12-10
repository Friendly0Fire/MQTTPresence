#pragma once
#include <windows.h>
#include <exception>
#include <string>
#include <tchar.h>
#include <variant>

struct errcode_exception {
    explicit errcode_exception(LSTATUS code) : code_(code) {
        char* buf = nullptr;
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, nullptr, code, 0, buf, 1, nullptr);

        msg_ = buf;
        LocalFree(buf);
    }

    LSTATUS code() const { return code_; }
    const char* msg() const { return msg_.c_str(); }

private:
    std::string msg_;
    LSTATUS code_;
};

class registry_key {
public:
    registry_key(HKEY base, TCHAR* key) : base_key_(base), key_name_(key) {}
    
    template<typename T>
    void set_value(const TCHAR* name, DWORD type, const T& data) const {
        auto kr = open_key(KEY_WRITE);
        if(std::holds_alternative<LSTATUS>(kr))
            throw errcode_exception(std::get<LSTATUS>(kr));
        else {
            auto key = std::get<HKEY>(kr);
            auto status = RegSetValueEx(key, name, 0,
                            type, reinterpret_cast<const BYTE*>(&data), sizeof(T));
            close_key(key);
            if(status != ERROR_SUCCESS)
                throw status;
        }
    }
    
    void set_value(const TCHAR* name, DWORD type, const TCHAR* data) const {
        auto kr = open_key(KEY_WRITE);
        if(std::holds_alternative<LSTATUS>(kr))
            throw errcode_exception(std::get<LSTATUS>(kr));
        else {
            auto key = std::get<HKEY>(kr);
            auto status = RegSetValueEx(key, name, 0,
                            type, reinterpret_cast<const BYTE*>(data), (_tcslen(data) + 1) * sizeof(TCHAR));
            close_key(key);
            if(status != ERROR_SUCCESS)
                throw status;
        }
    }
    
    template<typename T>
    T get_value(const TCHAR* name) const {
        auto kr = open_key(KEY_READ);
        if(std::holds_alternative<LSTATUS>(kr))
            throw errcode_exception(std::get<LSTATUS>(kr));
        else {
            auto key = std::get<HKEY>(kr);
            T data;
            DWORD _;
            auto status = RegQueryValueEx(key, name, 0, nullptr, reinterpret_cast<BYTE*>(&data), &_);
            close_key(key);
            if(status != ERROR_SUCCESS) {
                if(status == ERROR_FILE_NOT_FOUND)
                    return T();
                else
                    throw status;
            } else
                return data;
        }
    }
    
    void get_value(const TCHAR* name, TCHAR* data, DWORD& data_length) const {
        auto kr = open_key(KEY_READ);
        if(std::holds_alternative<LSTATUS>(kr))
            throw errcode_exception(std::get<LSTATUS>(kr));
        else {
            auto key = std::get<HKEY>(kr);
            auto status = RegQueryValueEx(key, name, 0, nullptr, reinterpret_cast<BYTE*>(data), &data_length);
            close_key(key);
            if(status != ERROR_SUCCESS) {
                if(status == ERROR_FILE_NOT_FOUND && data)
                    ZeroMemory(data, data_length);
                else
                    throw status;
            }
        }
    }

    std::basic_string<TCHAR> get_value(const TCHAR* name) const {
        auto kr = open_key(KEY_READ);
        if(std::holds_alternative<LSTATUS>(kr))
            throw errcode_exception(std::get<LSTATUS>(kr));
        else {
            auto key = std::get<HKEY>(kr);
            DWORD data_length;
            auto status = RegQueryValueEx(key, name, 0, nullptr, nullptr, &data_length);
            std::vector<TCHAR> out;
            if(status == ERROR_SUCCESS) {
                out.resize(data_length / sizeof(TCHAR));
                status = RegQueryValueEx(key, name, 0, nullptr, reinterpret_cast<BYTE*>(out.data()), &data_length);
            }
            close_key(key);
            if(status != ERROR_SUCCESS) {
                if(status == ERROR_FILE_NOT_FOUND)
                    return TEXT("");
                else
                    throw status;
            }

            return out.data();
        }
    }

    void delete_value(const TCHAR* name) const {
        auto kr = open_key(KEY_WRITE);
        if(std::holds_alternative<LSTATUS>(kr))
            throw errcode_exception(std::get<LSTATUS>(kr));
        else {
            auto key = std::get<HKEY>(kr);
            auto status = RegDeleteValue(key, name);
            close_key(key);
            if(status != ERROR_SUCCESS)
                throw status;
        }
    }

private:
    std::variant<HKEY, LSTATUS> open_key(REGSAM access) const {
        HKEY key;
        auto status = RegCreateKeyEx(base_key_, key_name_.c_str(), 0, nullptr, 0, access, nullptr, &key, nullptr);
        if(status != ERROR_SUCCESS)
            return status;

        return key;
    }

    static void close_key(HKEY key) {
        RegCloseKey(key);
    }

    const HKEY base_key_;
    const std::basic_string<TCHAR> key_name_;
};
