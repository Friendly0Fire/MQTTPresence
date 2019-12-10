#pragma once

#define _WIN32_WINNT 0x0A00

#include "Resource.h"
#include <string>
#include <codecvt>

#define DEF_TSTR(name, val) \
    const char name[] = val; \
    const wchar_t name ## W[] = L ## val;

#ifdef UNICODE
#define CHOOSE_TSTR(name) name ## W
#else
#define CHOOSE_TSTR(name) name
#endif

/// <summary>
/// Converts a std::wstring into the equivalent std::string. Possible loss of character information.
/// </summary>
/// <param name="wstr">The std::wstring.</param>
/// <returns>The converted std::string.</returns>
inline std::string ws2s(const std::wstring& wstr) {
    typedef std::codecvt_utf8<wchar_t>           convert_typeX;
    std::wstring_convert<convert_typeX, wchar_t> converterX;

    return converterX.to_bytes(wstr);
}

DEF_TSTR(g_unique_identifier, "MQTTPresenceWindows");