#include "CodeEditor.h"
#include "Theme.h"
#include <commctrl.h>
#include <richedit.h>
#include <richole.h>
#include <tom.h>
#include <algorithm>
#include <cwctype>
#include <memory>
#include <regex>
#include <set>

namespace native_app {
namespace {
constexpr UINT_PTR EditorSubclass = 0xDF01;
constexpr UINT_PTR PopupSubclass = 0xDF02;
constexpr UINT_PTR HighlightTimer = 0xDF03;
enum class Kind { Keyword, String, Number, Comment, Variable };
struct Span { LONG start; LONG end; Kind kind; };
struct State {
    HWND editor = nullptr;
    HWND popup = nullptr;
    EditorLanguage language = EditorLanguage::PlainText;
    bool formatting = false;
    bool completing = false;
    bool readOnly = false;
    LONG prefixStart = 0;
    std::vector<std::wstring> words;
    std::vector<std::wstring> matches;
};
State* GetState(HWND hwnd) { return reinterpret_cast<State*>(GetPropW(hwnd, L"DataForge.CodeEditor")); }
std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}
bool Word(wchar_t c) { return iswalnum(c) || c == L'_' || c > 127; }
const std::vector<std::wstring>& SqlWords() {
    static const std::vector<std::wstring> words = {L"SELECT", L"FROM", L"WHERE", L"JOIN", L"LEFT JOIN", L"INNER JOIN", L"ON", L"AS", L"AND", L"OR", L"NOT", L"NULL", L"IS", L"IN", L"LIKE", L"BETWEEN", L"DISTINCT", L"ORDER BY", L"GROUP BY", L"HAVING", L"LIMIT", L"OFFSET", L"ASC", L"DESC", L"COUNT", L"SUM", L"AVG", L"MIN", L"MAX", L"COALESCE", L"CASE", L"WHEN", L"THEN", L"ELSE", L"END", L"WITH", L"UNION", L"ALL", L"INSERT INTO", L"VALUES", L"UPDATE", L"SET", L"DELETE FROM", L"CREATE TABLE", L"ALTER TABLE", L"DROP TABLE", L"PRIMARY KEY", L"FOREIGN KEY", L"REFERENCES", L"EXPLAIN QUERY PLAN", L"BEGIN", L"COMMIT", L"ROLLBACK", L"INTEGER", L"TEXT", L"REAL", L"BLOB"};
    return words;
}
std::vector<Span> Tokenize(const std::wstring& text, EditorLanguage language) {
    std::vector<Span> result;
    static const std::set<std::wstring> keywords = [] {
        std::set<std::wstring> words{L"true", L"false", L"null", L"curl"};
        for (auto s : SqlWords()) {
            size_t start = 0;
            while (start < s.size()) { auto end = s.find(L' ', start); words.insert(Lower(s.substr(start, end - start))); if (end == s.npos) break; start = end + 1; }
        }
        return words;
    }();
    size_t i = 0;
    while (i < text.size()) {
        const size_t start = i;
        Kind kind = Kind::Keyword;
        bool emit = true;
        if (text.compare(i, 2, L"{{") == 0) {
            auto end = text.find(L"}}", i + 2); i = end == text.npos ? text.size() : end + 2; kind = Kind::Variable;
        } else if ((language == EditorLanguage::Sql && text.compare(i, 2, L"--") == 0) || (language == EditorLanguage::Shell && text[i] == L'#')) {
            auto end = text.find_first_of(L"\r\n", i); i = end == text.npos ? text.size() : end; kind = Kind::Comment;
        } else if (language == EditorLanguage::Sql && text.compare(i, 2, L"/*") == 0) {
            auto end = text.find(L"*/", i + 2); i = end == text.npos ? text.size() : end + 2; kind = Kind::Comment;
        } else if (text[i] == L'\"' || (language != EditorLanguage::Json && (text[i] == L'\'' || text[i] == L'`' || text[i] == L'['))) {
            wchar_t quote = text[i] == L'[' ? L']' : text[i]; ++i;
            while (i < text.size()) {
                if (text[i] == L'\\' && language != EditorLanguage::Sql) { i = (std::min)(text.size(), i + 2); continue; }
                if (text[i++] == quote) { if (language == EditorLanguage::Sql && i < text.size() && text[i] == quote) { ++i; continue; } break; }
            }
            kind = Kind::String;
        } else if (iswdigit(text[i]) || (text[i] == L'-' && i + 1 < text.size() && iswdigit(text[i + 1]))) {
            ++i; while (i < text.size() && (iswdigit(text[i]) || text[i] == L'.' || text[i] == L'e' || text[i] == L'E' || text[i] == L'+' || text[i] == L'-')) ++i; kind = Kind::Number;
        } else if (Word(text[i])) {
            ++i; while (i < text.size() && Word(text[i])) ++i;
            emit = language != EditorLanguage::PlainText && keywords.count(Lower(text.substr(start, i - start))) > 0;
        } else { ++i; emit = false; }
        if (emit && (language != EditorLanguage::PlainText || kind == Kind::Variable)) result.push_back({static_cast<LONG>(start), static_cast<LONG>(i), kind});
    }
    return result;
}
COLORREF Color(Kind kind) {
    const bool dark = Theme::GetCurrentTheme() == AppTheme::Dark;
    switch (kind) {
    case Kind::Keyword: return dark ? RGB(196,181,253) : RGB(109,40,217);
    case Kind::String: return dark ? RGB(110,231,183) : RGB(4,120,87);
    case Kind::Number: return dark ? RGB(125,211,252) : RGB(14,116,144);
    case Kind::Comment: return Theme::Get().textMuted;
    case Kind::Variable: return dark ? RGB(253,230,138) : RGB(146,64,14);
    }
    return Theme::Get().textPrimary;
}
void Highlight(State& state) {
    if (state.formatting) return;
    state.formatting = true;
    const auto text = CodeEditor::GetText(state.editor);
    IRichEditOle* ole = nullptr;
    ITextDocument* document = nullptr;
    SendMessageW(state.editor, EM_GETOLEINTERFACE, 0, reinterpret_cast<LPARAM>(&ole));
    if (ole) { ole->QueryInterface(__uuidof(ITextDocument), reinterpret_cast<void**>(&document)); ole->Release(); }
    if (document) {
        LONG unused = 0;
        document->Undo(tomSuspend, &unused);
        document->Freeze(&unused);
        auto colorRange = [&](LONG start, LONG end, COLORREF color) {
            ITextRange* range = nullptr;
            if (SUCCEEDED(document->Range(start, end, &range)) && range) {
                ITextFont* font = nullptr;
                if (SUCCEEDED(range->GetFont(&font)) && font) { font->SetForeColor(static_cast<LONG>(color)); font->Release(); }
                range->Release();
            }
        };
        colorRange(0, static_cast<LONG>(text.size()), Theme::Get().textPrimary);
        // Bound formatting cost for large responses without truncating their text.
        if (text.size() <= 512 * 1024) for (const auto& span : Tokenize(text, state.language)) colorRange(span.start, span.end, Color(span.kind));
        document->Unfreeze(&unused);
        document->Undo(tomResume, &unused);
        document->Release();
    }
    state.formatting = false;
    InvalidateRect(state.editor, nullptr, FALSE);
}
void Hide(State& state) { if (state.popup) ShowWindow(state.popup, SW_HIDE); state.matches.clear(); }
void Accept(State& state) {
    if (!state.popup || state.matches.empty()) return;
    const auto index = static_cast<int>(SendMessageW(state.popup, LB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(state.matches.size())) return;
    CHARRANGE range{}; SendMessageW(state.editor, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&range));
    range.cpMin = state.prefixStart;
    SendMessageW(state.editor, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
    SendMessageW(state.editor, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(state.matches[index].c_str()));
    Hide(state); SetFocus(state.editor);
    SetTimer(state.editor, HighlightTimer, 120, nullptr);
}
LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& state = *reinterpret_cast<State*>(data);
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_LBUTTONDOWN) {
        auto result = DefSubclassProc(hwnd, msg, wp, lp);
        Accept(state); return result;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
void Complete(State& state, bool explicitRequest) {
    if (state.readOnly || (state.language == EditorLanguage::PlainText && state.words.empty())) return;
    CHARRANGE selection{}; SendMessageW(state.editor, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    if (selection.cpMin != selection.cpMax) { Hide(state); return; }
    auto text = CodeEditor::GetText(state.editor);
    size_t end = (std::min)(text.size(), static_cast<size_t>((std::max)(0L, selection.cpMin)));
    size_t start = end;
    while (start && (Word(text[start - 1]) || text[start - 1] == L'.' || text[start - 1] == L'{')) --start;
    auto prefix = Lower(text.substr(start, end - start));
    if (!explicitRequest && prefix.size() < 2) { Hide(state); return; }
    // Avoid distracting suggestions inside comments and string literals.
    for (const auto& span : Tokenize(text, state.language)) if (selection.cpMin > span.start && selection.cpMin < span.end && (span.kind == Kind::Comment || (span.kind == Kind::String && prefix.find(L"{{") != 0))) { Hide(state); return; }
    std::vector<std::wstring> candidates = state.words;
    if (state.language == EditorLanguage::Sql) {
        candidates.insert(candidates.end(), SqlWords().begin(), SqlWords().end());
        const auto dot = prefix.find(L'.');
        if (dot != prefix.npos) {
            const auto alias = prefix.substr(0, dot);
            // Alias completion: schema table.column entries become alias.column.
            const std::wregex aliases(LR"(\b(?:from|join)\s+["`\[]?([\w]+)["`\]]?\s+(?:as\s+)?([\w]+))", std::regex::icase);
            for (auto it = std::wsregex_iterator(text.begin(), text.end(), aliases); it != std::wsregex_iterator(); ++it) {
                if (Lower((*it)[2].str()) != alias) continue;
                const auto table = Lower((*it)[1].str()) + L".";
                for (const auto& word : state.words) if (Lower(word).find(table) == 0) candidates.push_back(text.substr(start, dot) + word.substr(table.size() - 1));
            }
        }
    } else if (state.language == EditorLanguage::Json) {
        candidates.insert(candidates.end(), {L"true", L"false", L"null"});
    } else if (state.language == EditorLanguage::Shell) {
        candidates.insert(candidates.end(), {L"curl", L"--request", L"--header", L"--data", L"--url"});
    }
    std::sort(candidates.begin(), candidates.end()); candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    state.matches.clear();
    for (const auto& word : candidates) if (Lower(word).find(prefix) == 0 && Lower(word) != prefix) {
        state.matches.push_back(word); if (state.matches.size() == 60) break;
    }
    if (state.matches.empty()) { Hide(state); return; }
    state.prefixStart = static_cast<LONG>(start);
    if (!state.popup) {
        state.popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"LISTBOX", L"Code completions",
            WS_POPUP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            0, 0, 100, 100, state.editor, nullptr, GetModuleHandleW(nullptr), nullptr);
        if(!state.popup){state.matches.clear();return;}
        SetWindowSubclass(state.popup, PopupProc, PopupSubclass, reinterpret_cast<DWORD_PTR>(&state));
    }
    SendMessageW(state.popup, LB_RESETCONTENT, 0, 0);
    SendMessageW(state.popup, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetCodeFont()), FALSE);
    for (const auto& word : state.matches) SendMessageW(state.popup, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(word.c_str()));
    SendMessageW(state.popup, LB_SETCURSEL, 0, 0);
    POINT caret{}; SendMessageW(state.editor, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&caret), selection.cpMin);
    ClientToScreen(state.editor, &caret);
    const int popupW = Theme::Scale(320);
    const int popupH = Theme::Scale((std::min)(8, static_cast<int>(state.matches.size())) * 24 + 4);
    RECT work{}; MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(MonitorFromWindow(state.editor, MONITOR_DEFAULTTONEAREST), &info)) work = info.rcWork;
    int px = (std::max)(work.left, (std::min)(caret.x, work.right - popupW));
    int py = caret.y + Theme::Scale(24); if (py + popupH > work.bottom) py = caret.y - popupH;
    SetWindowPos(state.popup, HWND_TOP, px, py, popupW, popupH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
void Gutter(HWND hwnd, HDC dc) {
    RECT rect{}; GetClientRect(hwnd, &rect); rect.right = Theme::Scale(48);
    FillRect(dc, &rect, Theme::GetBgSecondaryBrush());
    const auto oldFont = SelectObject(dc, Theme::GetCodeFont());
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, Theme::Get().textMuted);
    const LONG first = static_cast<LONG>(SendMessageW(hwnd, EM_GETFIRSTVISIBLELINE, 0, 0));
    const LONG count = static_cast<LONG>(SendMessageW(hwnd, EM_GETLINECOUNT, 0, 0));
    TEXTMETRICW metrics{}; GetTextMetricsW(dc, &metrics);
    for (LONG line = first; line < count; ++line) {
        const auto index = SendMessageW(hwnd, EM_LINEINDEX, line, 0);
        POINT pos{}; SendMessageW(hwnd, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&pos), index);
        if (pos.y > rect.bottom) break;
        RECT number{0, pos.y, rect.right - Theme::Scale(8), pos.y + metrics.tmHeight};
        const auto label = std::to_wstring(line + 1); DrawTextW(dc, label.c_str(), -1, &number, DT_RIGHT | DT_SINGLELINE);
    }
    SelectObject(dc, oldFont);
}
LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& state = *reinterpret_cast<State*>(data);
    if (msg == WM_NCDESTROY) {
        KillTimer(hwnd, HighlightTimer);
        if (state.popup) DestroyWindow(state.popup);
        RemovePropW(hwnd, L"DataForge.CodeEditor"); RemoveWindowSubclass(hwnd, EditorProc, EditorSubclass);
        delete &state; return DefSubclassProc(hwnd, msg, wp, lp);
    }
    if (msg == WM_GETDLGCODE) {
        const auto* message=reinterpret_cast<const MSG*>(lp);
        const bool special=message && message->message==WM_KEYDOWN && (message->wParam==VK_RETURN || (message->wParam==VK_ESCAPE && !state.matches.empty()));
        return DLGC_WANTCHARS | DLGC_WANTARROWS | (state.matches.empty() ? 0 : DLGC_WANTTAB) | (special?DLGC_WANTMESSAGE:0);
    }
    if (msg == WM_CTLCOLORLISTBOX) {
        auto dc = reinterpret_cast<HDC>(wp); SetTextColor(dc, Theme::Get().textPrimary); SetBkColor(dc, Theme::Get().bgSecondary);
        return reinterpret_cast<LRESULT>(Theme::GetBgSecondaryBrush());
    }
    if (msg == WM_KEYDOWN) {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl && wp == VK_SPACE) { Complete(state, true); return 0; }
        if (ctrl && wp == L'A') { CHARRANGE all{0, -1}; SendMessageW(hwnd, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&all)); return 0; }
        if (!state.matches.empty()) {
            if (wp == VK_ESCAPE) { Hide(state); return 0; }
            if (wp == VK_RETURN || wp == VK_TAB) { Accept(state); state.completing = true; return 0; }
            if (wp == VK_UP || wp == VK_DOWN) {
                int index = static_cast<int>(SendMessageW(state.popup, LB_GETCURSEL, 0, 0));
                index = (std::max)(0, (std::min)(index + (wp == VK_UP ? -1 : 1), static_cast<int>(state.matches.size()) - 1));
                SendMessageW(state.popup, LB_SETCURSEL, index, 0); return 0;
            }
        }
        if (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_HOME || wp == VK_END) Hide(state);
    }
    if (msg == WM_CHAR && state.completing) { state.completing = false; if (wp == L'\r' || wp == L'\t') return 0; }
    if (msg == WM_CHAR && wp == L'\t' && !state.readOnly) {
        SendMessageW(hwnd, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"    ")); SetTimer(hwnd, HighlightTimer, 120, nullptr); return 0;
    }
    if (msg == WM_KILLFOCUS || msg == WM_LBUTTONDOWN || msg == WM_MOUSEWHEEL) Hide(state);
    if (msg == WM_TIMER && wp == HighlightTimer) { KillTimer(hwnd, HighlightTimer); Highlight(state); Complete(state, false); return 0; }
    const auto result = DefSubclassProc(hwnd, msg, wp, lp);
    if ((msg == WM_CHAR || msg == WM_PASTE || msg == WM_CUT || msg == WM_CLEAR || msg == WM_UNDO || msg == EM_UNDO || msg == EM_REDO || msg == WM_SETTEXT || msg == EM_REPLACESEL) && !state.formatting) SetTimer(hwnd, HighlightTimer, 120, nullptr);
    if (msg == WM_SIZE) {
        RECT rect{}; GetClientRect(hwnd, &rect); rect.left = Theme::Scale(56); rect.top += Theme::Scale(8); rect.right -= Theme::Scale(8); rect.bottom -= Theme::Scale(8);
        SendMessageW(hwnd, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&rect)); Hide(state);
    }
    if (msg == WM_PAINT) { HDC dc = GetDC(hwnd); Gutter(hwnd, dc); ReleaseDC(hwnd, dc); }
    return result;
}
}

HWND CodeEditor::Create(HWND parent, UINT id, EditorLanguage language, bool readOnly) {
    static const HMODULE richEdit = LoadLibraryW(L"Msftedit.dll");
    if (!richEdit) return nullptr;
    HWND hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
        WS_CHILD | WS_TABSTOP | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL | (readOnly ? ES_READONLY : 0),
        0, 0, 100, 100, parent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return nullptr;
    auto* state = new State; state->editor = hwnd; state->language = language; state->readOnly = readOnly;
    SetPropW(hwnd, L"DataForge.CodeEditor", state);
    SetWindowSubclass(hwnd, EditorProc, EditorSubclass, reinterpret_cast<DWORD_PTR>(state));
    SendMessageW(hwnd, EM_EXLIMITTEXT, 0, 16 * 1024 * 1024);
    SendMessageW(hwnd, EM_SETUNDOLIMIT, 200, 0);
    SendMessageW(hwnd, EM_SETEDITSTYLE, SES_EMULATESYSEDIT | SES_NOEALINEHEIGHTADJUST, SES_EMULATESYSEDIT | SES_NOEALINEHEIGHTADJUST);
    SendMessageW(hwnd, EM_SETTARGETDEVICE, 0, 0); // No automatic line breaks for code.
    ApplyTheme(hwnd);
    return hwnd;
}
void CodeEditor::SetLanguage(HWND hwnd, EditorLanguage language) { if (auto* state = GetState(hwnd)) { state->language = language; Highlight(*state); Hide(*state); } }
void CodeEditor::SetCompletions(HWND hwnd, const std::vector<std::wstring>& words) { if (auto* state = GetState(hwnd)) state->words = words; }
std::wstring CodeEditor::GetText(HWND hwnd) {
    // GT_RAWTEXT uses the same character positions as TOM, including paragraph CRs.
    GETTEXTLENGTHEX length{GTL_NUMCHARS | GTL_PRECISE, 1200};
    auto count = SendMessageW(hwnd, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&length), 0);
    if (count < 0 || count > 16 * 1024 * 1024) return {};
    std::wstring text(static_cast<size_t>(count) + 1, L'\0');
    GETTEXTEX get{static_cast<DWORD>(text.size() * sizeof(wchar_t)), GT_RAWTEXT, 1200, nullptr, nullptr};
    auto actual = SendMessageW(hwnd, EM_GETTEXTEX, reinterpret_cast<WPARAM>(&get), reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<size_t>((std::max)(0LL, static_cast<long long>(actual)))); return text;
}
void CodeEditor::SetText(HWND hwnd, const std::wstring& text) { SetWindowTextW(hwnd, text.c_str()); if (auto* state = GetState(hwnd)) { KillTimer(hwnd, HighlightTimer); Highlight(*state); Hide(*state); } }
bool CodeEditor::IsEditor(HWND hwnd) { return GetState(hwnd) != nullptr; }
void CodeEditor::ApplyTheme(HWND hwnd) {
    if (auto* state = GetState(hwnd)) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetCodeFont()), FALSE);
        SendMessageW(hwnd, EM_SETBKGNDCOLOR, 0, Theme::Get().bgPrimary);
        CHARFORMAT2W format{}; format.cbSize = sizeof(format); format.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE; format.crTextColor = Theme::Get().textPrimary;
        format.yHeight = MulDiv(12 * 20, Theme::GetTextScale(), 100); wcscpy_s(format.szFaceName, L"Consolas");
        SendMessageW(hwnd, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&format));
        Highlight(*state);
    }
}
bool CodeEditor::RunSelfTests(std::wstring& failure) {
    auto check = [&](bool ok, const wchar_t* label) { if (!ok) failure += std::wstring(label) + L"; "; };
    const auto sql = Tokenize(L"SELECT 'it''s -- text', 42 /* comment */ FROM products", EditorLanguage::Sql);
    check(sql.size() == 5 && sql[1].kind == Kind::String && sql[3].kind == Kind::Comment, L"SQL literal and comment lexer");
    const auto json = Tokenize(L"{\"escaped\\\"key\": true, \"n\": -12.5}", EditorLanguage::Json);
    check(json.size() == 4 && json.back().kind == Kind::Number, L"JSON escape and number lexer");
    HWND parent = CreateWindowExW(0, L"STATIC", L"Editor tests", WS_OVERLAPPED, 0, 0, 600, 400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND editor = Create(parent, 1, EditorLanguage::Sql);
    if (!editor) { failure += L"Rich Edit unavailable"; if (parent) DestroyWindow(parent); return false; }
    SetText(editor, L"SELECT 'hello', 42;");
    CHARRANGE range{0, 6}; SendMessageW(editor, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
    CHARFORMAT2W format{}; format.cbSize = sizeof(format); SendMessageW(editor, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
    check(format.crTextColor == Color(Kind::Keyword), L"native syntax color applied");
    CHARRANGE end{-1, -1}; SendMessageW(editor, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&end));
    SendMessageW(editor, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L" -- test"));
    Highlight(*GetState(editor)); SendMessageW(editor, EM_UNDO, 0, 0);
    check(GetText(editor) == L"SELECT 'hello', 42;", L"highlighting preserves undo");
    SetText(editor, L"SEL"); SendMessageW(editor, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&end)); Complete(*GetState(editor), true);
    check(!GetState(editor)->matches.empty(), L"completion popup populated"); Accept(*GetState(editor));
    check(GetText(editor) == L"SELECT", L"completion inserts keyword");
    SetCompletions(editor, {L"products", L"products.name", L"products.id"});
    SetText(editor, L"SELECT p.na FROM products AS p"); range = {11, 11}; SendMessageW(editor, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
    Complete(*GetState(editor), true); Accept(*GetState(editor));
    check(GetText(editor) == L"SELECT p.name FROM products AS p", L"alias-aware column completion");
    SetLanguage(editor,EditorLanguage::Json);SetCompletions(editor,{L"{{token}}"});SetText(editor,L"{\"value\": \"{{to\"}");range={15,15};SendMessageW(editor,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&range));
    Complete(*GetState(editor),true);Accept(*GetState(editor));check(GetText(editor)==L"{\"value\": \"{{token}}\"}",L"variables complete inside JSON strings");
    DestroyWindow(parent); return failure.empty();
}
}
