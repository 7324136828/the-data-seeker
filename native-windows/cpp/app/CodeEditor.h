#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace native_app {
struct TableMeta;
enum class EditorLanguage { PlainText, Sql, Json, Shell, JavaScript };

// Windows Rich Edit with native syntax colors and a keyboard-accessible completion list.
class CodeEditor {
public:
    static HWND Create(HWND parent, UINT id, EditorLanguage language, bool readOnly = false);
    static void SetLanguage(HWND editor, EditorLanguage language);
    static void SetCompletions(HWND editor, const std::vector<std::wstring>& words);
    static void SetSqlSchema(HWND editor, const std::vector<TableMeta>& tables, const std::string& dialect);
    static std::wstring GetText(HWND editor);
    static void SetText(HWND editor, const std::wstring& text);
    static void ApplyTheme(HWND editor);
    static bool IsEditor(HWND window);
    static bool RunSelfTests(std::wstring& failure);
};
}
