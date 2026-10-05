#pragma once
#include "CodeEditor.h"
#include <functional>
#include <string>
#include <vector>

namespace native_app {
std::wstring ChooseNativeFile(HWND owner,const wchar_t* title,const wchar_t* filter,bool save,const wchar_t* suggestedName=L"");
void WriteNativeFileAtomic(const std::wstring& path,const std::string& content);
int ChooseNativeItem(HWND owner,const std::wstring& title,const std::vector<std::wstring>& choices,int initial=0);
// Validation returns an actionable error, or empty text when the value is accepted.
bool EditNativeText(HWND owner, const std::wstring& title, std::wstring& text,
    EditorLanguage language = EditorLanguage::PlainText,
    const std::function<std::wstring(const std::wstring&)>& validate = {},
    const std::vector<std::wstring>& completions = {});
}
