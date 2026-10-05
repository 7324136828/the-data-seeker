#pragma once
#include "CodeEditor.h"
#include <functional>
#include <string>

namespace native_app {
// Validation returns an actionable error, or empty text when the value is accepted.
bool EditNativeText(HWND owner, const std::wstring& title, std::wstring& text,
    EditorLanguage language = EditorLanguage::PlainText,
    const std::function<std::wstring(const std::wstring&)>& validate = {});
}
