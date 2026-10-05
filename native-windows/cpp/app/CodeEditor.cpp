#include "CodeEditor.h"
#include "Theme.h"
#include "Types.h"
#include <commctrl.h>
#include <richedit.h>
#include <richole.h>
#include <tom.h>
#include <algorithm>
#include <cwctype>
#include <memory>
#include <regex>
#include <set>
#include <stdexcept>

namespace native_app {
namespace {
constexpr UINT_PTR EditorSubclass = 0xDF01;
constexpr UINT_PTR PopupSubclass = 0xDF02;
constexpr UINT_PTR HighlightTimer = 0xDF03;
enum class Kind { Keyword, String, Number, Comment, Variable };
struct Span { LONG start; LONG end; Kind kind; };
struct Completion { std::wstring label, text; };
struct SqlTable { std::wstring name; std::vector<std::wstring> columns; };
struct State {
    HWND editor = nullptr;
    HWND popup = nullptr;
    EditorLanguage language = EditorLanguage::PlainText;
    bool formatting = false;
    bool completing = false;
    bool readOnly = false;
    LONG prefixStart = 0;
    std::vector<std::wstring> words;
    std::vector<Completion> matches;
    std::vector<SqlTable> sqlTables;
    std::string sqlDialect;
};
State* GetState(HWND hwnd) { return reinterpret_cast<State*>(GetPropW(hwnd, L"DataForge.CodeEditor")); }
std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}
bool Word(wchar_t c) { return iswalnum(c) || c == L'_' || c == L'$' || c > 127; }
size_t DollarDelimiter(const std::wstring& text, size_t start);
const std::vector<std::wstring>& SqlWords() {
    static const std::vector<std::wstring> words = {L"SELECT", L"FROM", L"WHERE", L"JOIN", L"LEFT JOIN", L"INNER JOIN", L"ON", L"AS", L"AND", L"OR", L"NOT", L"NULL", L"IS", L"IN", L"LIKE", L"BETWEEN", L"DISTINCT", L"ORDER BY", L"GROUP BY", L"HAVING", L"LIMIT", L"OFFSET", L"ASC", L"DESC", L"COUNT", L"SUM", L"AVG", L"MIN", L"MAX", L"COALESCE", L"CASE", L"WHEN", L"THEN", L"ELSE", L"END", L"WITH", L"UNION", L"ALL", L"INSERT INTO", L"VALUES", L"UPDATE", L"SET", L"DELETE FROM", L"CREATE TABLE", L"ALTER TABLE", L"DROP TABLE", L"PRIMARY KEY", L"FOREIGN KEY", L"REFERENCES", L"EXPLAIN QUERY PLAN", L"BEGIN", L"COMMIT", L"ROLLBACK", L"INTEGER", L"TEXT", L"REAL", L"BLOB"};
    return words;
}
std::vector<Span> Tokenize(const std::wstring& text, EditorLanguage language) {
    std::vector<Span> result;
    static const std::set<std::wstring> sqlKeywords = [] {
        std::set<std::wstring> words{L"true", L"false", L"null"};
        for (auto s : SqlWords()) {
            size_t start = 0;
            while (start < s.size()) { auto end = s.find(L' ', start); words.insert(Lower(s.substr(start, end - start))); if (end == s.npos) break; start = end + 1; }
        }
        return words;
    }();
    static const std::set<std::wstring> scriptKeywords{L"true",L"false",L"null",L"const",L"let",L"var",L"function",L"return",L"if",L"else",L"for",L"while",L"throw",L"new",L"try",L"catch",L"finally",L"undefined",L"class",L"async",L"await",L"typeof",L"of",L"in",L"this",L"switch",L"case",L"break",L"continue",L"do",L"delete",L"instanceof",L"void",L"yield",L"extends",L"super",L"static",L"get",L"set"};
    static const std::set<std::wstring> jsonKeywords{L"true",L"false",L"null"},shellKeywords{L"curl"};
    const auto& keywords=language==EditorLanguage::Sql?sqlKeywords:language==EditorLanguage::JavaScript?scriptKeywords:language==EditorLanguage::Json?jsonKeywords:shellKeywords;
    size_t i = 0;
    while (i < text.size()) {
        const size_t start = i;
        Kind kind = Kind::Keyword;
        bool emit = true;
        if (text.compare(i, 2, L"{{") == 0) {
            auto end = text.find(L"}}", i + 2); i = end == text.npos ? text.size() : end + 2; kind = Kind::Variable;
        } else if ((language == EditorLanguage::Sql && text.compare(i, 2, L"--") == 0) || (language == EditorLanguage::JavaScript && text.compare(i,2,L"//")==0) || (language == EditorLanguage::Shell && text[i] == L'#')) {
            auto end = text.find_first_of(L"\r\n", i); i = end == text.npos ? text.size() : end; kind = Kind::Comment;
        } else if ((language == EditorLanguage::Sql||language==EditorLanguage::JavaScript) && text.compare(i, 2, L"/*") == 0) {
            auto end = text.find(L"*/", i + 2); i = end == text.npos ? text.size() : end + 2; kind = Kind::Comment;
        } else if (language == EditorLanguage::Sql && DollarDelimiter(text, i)) {
            const auto length = DollarDelimiter(text, i), end = text.find(text.substr(i, length), i + length);
            i = end == text.npos ? text.size() : end + length; kind = Kind::String;
        } else if (text[i] == L'\"' || (language != EditorLanguage::Json && (text[i] == L'\'' || text[i] == L'`')) || (language==EditorLanguage::Sql&&text[i]==L'[')) {
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
            if (language == EditorLanguage::Shell) {
                const auto newline = text.find_last_of(L"\r\n", start); size_t first = newline == text.npos ? 0 : newline + 1;
                while (first < start && iswspace(text[first])) ++first;
                emit = first == start;
            }
        } else { ++i; emit = false; }
        if (emit && (language != EditorLanguage::PlainText || kind == Kind::Variable)) result.push_back({static_cast<LONG>(start), static_cast<LONG>(i), kind});
    }
    return result;
}
COLORREF Color(Kind kind) {
    HIGHCONTRASTW contrast{sizeof(contrast)};if(SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0)&&(contrast.dwFlags&HCF_HIGHCONTRASTON))return Theme::Get().textPrimary;
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
    SendMessageW(state.editor, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(state.matches[index].text.c_str()));
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
struct SqlToken { std::wstring value, raw; size_t start, end; bool identifier, quoted; };
struct SqlReference { std::vector<std::wstring> parts; std::wstring alias; };
size_t DollarDelimiter(const std::wstring& text, size_t start) {
    if (text[start] != L'$') return 0;
    size_t end = start + 1;
    while (end < text.size() && (iswalnum(text[end]) || text[end] == L'_')) ++end;
    return end < text.size() && text[end] == L'$' ? end - start + 1 : 0;
}
std::vector<SqlToken> SqlTokens(const std::wstring& text, size_t caret) {
    std::vector<SqlToken> tokens;
    for (size_t i = 0; i < text.size();) {
        if (iswspace(text[i])) { ++i; continue; }
        if (text.compare(i, 2, L"--") == 0) { const auto end = text.find(L'\n', i); i = end == text.npos ? text.size() : end; continue; }
        if (text.compare(i, 2, L"/*") == 0) { const auto end = text.find(L"*/", i + 2); i = end == text.npos ? text.size() : end + 2; continue; }
        const size_t start = i;
        if (const size_t length = DollarDelimiter(text, i)) {
            const auto end = text.find(text.substr(i, length), i + length);
            i = end == text.npos ? text.size() : end + length;
            tokens.push_back({L"", text.substr(start, i - start), start, i, false, true}); continue;
        }
        const wchar_t opening = text[i], closing = opening == L'[' ? L']' : opening;
        if (opening == L'\'' || opening == L'"' || opening == static_cast<wchar_t>(96) || opening == L'[') {
            std::wstring value; ++i;
            while (i < text.size()) {
                if (text[i] == closing) { if (i + 1 < text.size() && text[i + 1] == closing) { value += closing; i += 2; continue; } ++i; break; }
                value += text[i++];
            }
            tokens.push_back({value, text.substr(start, i - start), start, i, opening != L'\'', true});
        } else if (Word(text[i])) {
            while (i < text.size() && Word(text[i])) ++i;
            const auto value = text.substr(start, i - start); tokens.push_back({value, value, start, i, true, false});
        } else { const std::wstring value(1, text[i++]); tokens.push_back({value, value, start, i, false, false}); }
    }
    size_t begin = 0, end = tokens.size();
    for (size_t i = 0; i < tokens.size(); ++i) if (!tokens[i].identifier && tokens[i].raw == L";") {
        if (tokens[i].end <= caret) begin = i + 1; else { end = i; break; }
    }
    return std::vector<SqlToken>(tokens.begin() + begin, tokens.begin() + end);
}
bool SqlKeyword(const std::wstring& word) {
    static const std::set<std::wstring> keywords = [] {
        std::set<std::wstring> result{L"right", L"full", L"cross", L"outer", L"natural", L"using", L"fetch", L"first", L"rows", L"only", L"returning", L"window", L"except", L"intersect", L"lateral"};
        for (const auto& phrase : SqlWords()) {
            size_t start = 0;
            while (start < phrase.size()) { const auto end = phrase.find(L' ', start); result.insert(Lower(phrase.substr(start, end - start))); if (end == phrase.npos) break; start = end + 1; }
        }
        return result;
    }();
    return keywords.count(Lower(word)) != 0;
}
std::vector<SqlReference> SqlReferences(const std::vector<SqlToken>& tokens) {
    std::vector<SqlReference> result;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (!tokens[i].identifier || tokens[i].quoted || (Lower(tokens[i].value) != L"from" && Lower(tokens[i].value) != L"join")) continue;
        size_t source = i + 1;
        if (source < tokens.size() && !tokens[source].quoted && Lower(tokens[source].value) == L"only") ++source;
        do {
            if (source >= tokens.size() || !tokens[source].identifier) break;
            SqlReference reference; reference.parts.push_back(tokens[source++].value);
            while (source + 1 < tokens.size() && tokens[source].raw == L"." && tokens[source + 1].identifier) { reference.parts.push_back(tokens[source + 1].value); source += 2; }
            if (source < tokens.size() && !tokens[source].quoted && Lower(tokens[source].value) == L"as") { ++source; if (source < tokens.size() && tokens[source].identifier) reference.alias = tokens[source++].value; }
            else if (source < tokens.size() && tokens[source].identifier && (tokens[source].quoted || !SqlKeyword(tokens[source].value))) reference.alias = tokens[source++].value;
            result.push_back(std::move(reference));
            if (source >= tokens.size() || tokens[source].raw != L",") break;
            ++source;
        } while (true);
    }
    return result;
}
const SqlTable* FindSqlTable(const State& state, const SqlReference& reference) {
    if (reference.parts.empty()) return nullptr;
    const SqlTable* found = nullptr;
    for (const auto& table : state.sqlTables) if (table.name == reference.parts.back()) return &table;
    for (const auto& table : state.sqlTables) if (Lower(table.name) == Lower(reference.parts.back())) { if (found) return nullptr; found = &table; }
    return found;
}
bool ReferenceMatches(const SqlReference& reference, const std::vector<std::wstring>& qualifier) {
    if (qualifier.size() == 1 && (Lower(qualifier[0]) == Lower(reference.alias) || Lower(qualifier[0]) == Lower(reference.parts.back()))) return true;
    if (qualifier.size() > reference.parts.size()) return false;
    const auto offset = reference.parts.size() - qualifier.size();
    for (size_t i = 0; i < qualifier.size(); ++i) if (Lower(qualifier[i]) != Lower(reference.parts[offset + i])) return false;
    return true;
}
bool SqlColumnPrefix(const std::wstring& text, size_t caret, const std::vector<SqlToken>& tokens,
    std::vector<std::wstring>& qualifier, std::wstring& raw, std::wstring& suffix, size_t& start) {
    for (size_t i = 0; i < tokens.size(); ++i) {
        size_t dot = i;
        if (tokens[i].identifier && !tokens[i].quoted && tokens[i].start < caret && caret <= tokens[i].end) {
            if (!i || tokens[i - 1].raw != L".") continue;
            dot = i - 1; suffix = Lower(text.substr(tokens[i].start, caret - tokens[i].start));
        } else if (tokens[i].raw == L"." && tokens[i].end == caret) suffix.clear();
        else continue;
        if (!dot || !tokens[dot - 1].identifier) return false;
        size_t first = dot - 1; qualifier.insert(qualifier.begin(), tokens[first].value);
        while (first >= 2 && tokens[first - 1].raw == L"." && tokens[first - 2].identifier) { first -= 2; qualifier.insert(qualifier.begin(), tokens[first].value); }
        start = tokens[first].start; raw = text.substr(start, tokens[dot].start - start); return true;
    }
    return false;
}
std::wstring SqlIdentifier(const std::wstring& value, const std::string& dialect) {
    const wchar_t opening = dialect == "mysql" || dialect == "mariadb" ? static_cast<wchar_t>(96) : dialect == "mssql" ? L'[' : L'"';
    const wchar_t closing = opening == L'[' ? L']' : opening;
    std::wstring result(1, opening); for (wchar_t character : value) { result += character; if (character == closing) result += character; } return result + closing;
}
bool PlainSqlIdentifier(const std::wstring& value) {
    return !value.empty() && !iswdigit(value.front()) && std::all_of(value.begin(), value.end(), Word) && !SqlKeyword(value);
}
void AddCompletion(State& state, const std::wstring& label, const std::wstring& value) {
    if (state.matches.size() >= 60 || std::any_of(state.matches.begin(), state.matches.end(), [&](const auto& match) { return match.text == value; })) return;
    state.matches.push_back({label, value});
}
bool ExpandSqlStar(State& state, const std::wstring& text, size_t caret, const std::vector<SqlToken>& tokens, const std::vector<SqlReference>& references) {
    const auto star = std::find_if(tokens.begin(), tokens.end(), [&](const auto& token) { return token.raw == L"*" && token.end == caret; });
    if (star == tokens.end()) return false;
    size_t start = static_cast<size_t>(star - tokens.begin());
    std::vector<std::wstring> qualifier;
    while (start >= 2 && tokens[start - 1].raw == L"." && tokens[start - 2].identifier) { qualifier.insert(qualifier.begin(), tokens[start - 2].value); start -= 2; }
    // Expand a projection wildcard, never arithmetic, COUNT(*) or UPDATE text.
    if (!start || (Lower(tokens[start - 1].value) != L"select" && Lower(tokens[start - 1].value) != L"distinct" && tokens[start - 1].raw != L",")) return true;
    const SqlReference* reference = nullptr;
    for (const auto& item : references) if (qualifier.empty() || ReferenceMatches(item, qualifier)) { if (reference) return true; reference = &item; }
    if (!reference) return true;
    const auto* table = FindSqlTable(state, *reference); if (!table || table->columns.empty()) return true;
    const std::wstring prefix = qualifier.empty() ? L"" : text.substr(tokens[start].start, star->start - tokens[start].start);
    std::wstring expansion;
    for (const auto& column : table->columns) { if (!expansion.empty()) expansion += L", "; expansion += prefix + SqlIdentifier(column, state.sqlDialect); }
    state.prefixStart = static_cast<LONG>(tokens[start].start); AddCompletion(state, L"Expand * to columns", expansion); return true;
}
std::wstring JsStringFragment(const std::wstring& value, wchar_t quote) {
    std::wstring result;
    for (wchar_t character : value) {
        if (character == L'\\' || character == quote) { result += L'\\'; result += character; }
        else if (character == L'\n') result += L"\\n"; else if (character == L'\r') result += L"\\r";
        else if (character == L'\t') result += L"\\t"; else if (character == 0x2028) result += L"\\u2028"; else if (character == 0x2029) result += L"\\u2029"; else result += character;
    }
    return result;
}
const std::vector<std::wstring>& ScriptWords() {
    static const auto words = [] {
        std::vector<std::wstring> result{L"const",L"let",L"var",L"function",L"return",L"JSON.parse",L"JSON.stringify",L"Math",L"Date",L"console.log",L"console.info",L"console.warn",L"console.error",L"pm.test",L"pm.expect",L"pm.request.method",L"pm.request.url",L"pm.request.url.update",L"pm.request.url.toString",L"pm.request.body.raw",L"pm.request.body.update",L"pm.request.body.toString",L"pm.response.json",L"pm.response.text",L"pm.response.code",L"pm.response.status",L"pm.response.responseTime",L"pm.response.responseSize",L"pm.response.to.have.status",L"pm.info.requestName",L"pm.info.requestId",L"pm.info.iteration"};
        for (const auto* scope : {L"variables",L"environment",L"collectionVariables",L"globals"}) for (const auto* method : {L"get",L"set",L"unset",L"has",L"replaceIn",L"toObject"}) result.push_back(std::wstring(L"pm.") + scope + L"." + method);
        for (const auto* method : {L"get",L"has",L"toObject"}) result.push_back(std::wstring(L"pm.iterationData.") + method);
        for (const auto* resource : {L"pm.request.headers.",L"pm.request.url.query.",L"pm.request.url.variables."}) for (const auto* method : {L"get",L"has",L"all",L"add",L"upsert",L"remove"}) result.push_back(std::wstring(resource) + method);
        for (const auto* method : {L"get",L"has",L"all"}) result.push_back(std::wstring(L"pm.response.headers.") + method);
        return result;
    }();
    return words;
}
void Complete(State& state, bool explicitRequest) {
    if (state.readOnly || (state.language == EditorLanguage::PlainText && state.words.empty())) return;
    CHARRANGE selection{}; SendMessageW(state.editor, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    if (selection.cpMin != selection.cpMax) { Hide(state); return; }
    const auto text = CodeEditor::GetText(state.editor);
    if (text.size() > 512 * 1024) { Hide(state); return; }
    const size_t end = (std::min)(text.size(), static_cast<size_t>((std::max)(0L, selection.cpMin)));
    size_t start = end; while (start && (Word(text[start - 1]) || text[start - 1] == L'.' || text[start - 1] == L'{')) --start;
    auto prefix = Lower(text.substr(start, end - start));
    const auto spans = Tokenize(text, state.language);
    const Span* current = nullptr;
    for (const auto& span : spans) {
        bool inside = end > static_cast<size_t>(span.start) && end < static_cast<size_t>(span.end);
        if (end == text.size() && end == static_cast<size_t>(span.end) && span.kind == Kind::Comment)
            inside = text.compare(span.start, 2, L"/*") != 0 || text.size() < 2 || text.compare(text.size() - 2, 2, L"*/") != 0;
        if (end == text.size() && end == static_cast<size_t>(span.end) && span.kind == Kind::String) {
            const wchar_t opening = text[span.start], closing = opening == L'[' ? L']' : opening; bool closed = false;
            if (state.language == EditorLanguage::Sql && opening == L'$') {
                const auto length = DollarDelimiter(text, span.start); closed = text.find(text.substr(span.start, length), span.start + length) != text.npos;
            } else for (size_t i = static_cast<size_t>(span.start) + 1; i < end; ++i) {
                if (state.language != EditorLanguage::Sql && text[i] == L'\\') { ++i; continue; }
                if (text[i] == closing) { if (state.language == EditorLanguage::Sql && i + 1 < end && text[i + 1] == closing) { ++i; continue; } closed = true; break; }
            }
            if (!closed) inside = true;
        }
        if (inside) { current = &span; break; }
    }
    if (current && current->kind == Kind::Comment) { Hide(state); return; }
    bool argument = false; wchar_t argumentQuote = 0;
    if (state.language == EditorLanguage::JavaScript) {
        static const std::wregex scopedArgument(LR"(\bpm\.(?:variables|environment|collectionVariables|globals|iterationData)\.(?:get|set|has|unset)\(\s*(["']))");
        const auto before = text.substr(0, end);
        if (current && current->kind == Kind::String) for (auto item = std::wsregex_iterator(before.begin(), before.end(), scopedArgument); item != std::wsregex_iterator(); ++item)
            if (static_cast<size_t>(current->start) == static_cast<size_t>(item->position(1))) {
                argument = true; argumentQuote = item->str(1)[0]; start = static_cast<size_t>(item->position(1)) + 1; prefix = Lower(text.substr(start, end - start)); break;
            }
    }
    if (current && current->kind == Kind::String && state.language != EditorLanguage::Json && !argument && prefix.find(L"{{") != 0) { Hide(state); return; }
    state.matches.clear(); state.prefixStart = static_cast<LONG>(start);
    bool star = false;
    if (state.language == EditorLanguage::Sql && explicitRequest) {
        const auto tokens = SqlTokens(text, end); const auto references = SqlReferences(tokens);
        star = ExpandSqlStar(state, text, end, tokens, references);
    }
    if (!star && !explicitRequest && !argument && prefix.size() < 2) { Hide(state); return; }
    if (argument) {
        for (const auto& word : state.words) { const auto value = JsStringFragment(word, argumentQuote); if (Lower(value).find(prefix) == 0 && Lower(value) != prefix) AddCompletion(state, word, value); }
    } else if (!star) {
        std::vector<std::wstring> candidates = state.language == EditorLanguage::JavaScript ? ScriptWords() : state.words;
        if (state.language == EditorLanguage::Sql) {
            candidates.insert(candidates.end(), SqlWords().begin(), SqlWords().end());
            const auto tokens = SqlTokens(text, end); std::vector<std::wstring> qualifierParts; std::wstring qualifier, suffix;
            if (SqlColumnPrefix(text, end, tokens, qualifierParts, qualifier, suffix, start)) {
                state.prefixStart = static_cast<LONG>(start); prefix = Lower(text.substr(start, end - start));
                const auto references = SqlReferences(tokens);
                const SqlReference* reference = nullptr;
                for (const auto& item : references) if (ReferenceMatches(item, qualifierParts)) { if (reference) { reference = nullptr; break; } reference = &item; }
                if (reference) {
                    if (const auto* table = FindSqlTable(state, *reference)) {
                        for (const auto& column : table->columns) if (Lower(column).find(suffix) == 0) {
                            const auto value = qualifier + L"." + (PlainSqlIdentifier(column) ? column : SqlIdentifier(column, state.sqlDialect));
                            AddCompletion(state, qualifier + L"." + column, value);
                        }
                    } else {
                        const auto tablePrefix = Lower(reference->parts.back()) + L".";
                        for (const auto& word : state.words) if (Lower(word).find(tablePrefix) == 0) candidates.push_back(qualifier + L"." + word.substr(tablePrefix.size()));
                    }
                }
            }
        } else if (state.language == EditorLanguage::Json) candidates.insert(candidates.end(), {L"true", L"false", L"null"});
        else if (state.language == EditorLanguage::Shell) candidates.insert(candidates.end(), {L"curl", L"--request", L"--header", L"--data", L"--url"});
        std::sort(candidates.begin(), candidates.end()); candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        for (const auto& word : candidates) if (Lower(word).find(prefix) == 0 && Lower(word) != prefix) AddCompletion(state, word, word);
    }
    if (state.matches.empty()) { Hide(state); return; }
    if (!state.popup) {
        state.popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"LISTBOX", L"Code completions",
            WS_POPUP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            0, 0, 100, 100, state.editor, nullptr, GetModuleHandleW(nullptr), nullptr);
        if(!state.popup){state.matches.clear();return;}
        SetWindowSubclass(state.popup, PopupProc, PopupSubclass, reinterpret_cast<DWORD_PTR>(&state));
    }
    SendMessageW(state.popup, LB_RESETCONTENT, 0, 0);
    SendMessageW(state.popup, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetCodeFont()), FALSE);
    for (const auto& word : state.matches) SendMessageW(state.popup, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(word.label.c_str()));
    SendMessageW(state.popup, LB_SETCURSEL, 0, 0);
    POINT caret{}; SendMessageW(state.editor, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&caret), selection.cpMin);
    ClientToScreen(state.editor, &caret);
    const int popupW = Theme::Scale(320);
    const int popupH = Theme::Scale((std::min)(8, static_cast<int>(state.matches.size())) * 24 + 4);
    RECT work{}; MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(MonitorFromWindow(state.editor, MONITOR_DEFAULTTONEAREST), &info)) work = info.rcWork;
    int px = (std::max)(work.left, (std::min)(caret.x, work.right - popupW));
    int py = caret.y + Theme::Scale(24); if (py + popupH > work.bottom) py = caret.y - popupH;
    SetWindowPos(state.popup, HWND_TOP, px, py, popupW, popupH, SWP_NOACTIVATE | (IsWindowVisible(state.editor)?SWP_SHOWWINDOW:0));
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
    if(msg==WM_CHAR && (GetKeyState(VK_CONTROL)&0x8000) && (wp==L' '||wp==0))return 0;
    if (msg == WM_CHAR && wp == L'\t' && !state.readOnly) {
        SendMessageW(hwnd, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"    ")); SetTimer(hwnd, HighlightTimer, 120, nullptr); return 0;
    }
    if (msg == WM_KILLFOCUS || msg == WM_LBUTTONDOWN || msg == WM_MOUSEWHEEL) Hide(state);
    if(msg==WM_PASTE && !state.readOnly){SendMessageW(hwnd,EM_PASTESPECIAL,CF_UNICODETEXT,0);SetTimer(hwnd,HighlightTimer,120,nullptr);return 0;}
    if (msg == WM_TIMER && wp == HighlightTimer) { KillTimer(hwnd, HighlightTimer); Highlight(state); Complete(state, false); return 0; }
    const auto result = DefSubclassProc(hwnd, msg, wp, lp);
    if ((msg == WM_CHAR || msg == WM_PASTE || msg == WM_CUT || msg == WM_CLEAR || msg == WM_UNDO || msg == EM_UNDO || msg == EM_REDO || msg == WM_SETTEXT || msg == EM_REPLACESEL) && !state.formatting) SetTimer(hwnd, HighlightTimer, 120, nullptr);
    if (msg == WM_SIZE) {
        RECT rect{}; GetClientRect(hwnd, &rect); const int verticalPadding=Theme::Scale(rect.bottom<Theme::Scale(80)?2:8);rect.left = Theme::Scale(56); rect.top += verticalPadding; rect.right -= Theme::Scale(8); rect.bottom -= verticalPadding;
        SendMessageW(hwnd, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&rect)); Hide(state);
    }
    if (msg == WM_PAINT) { HDC dc = GetDC(hwnd); Gutter(hwnd, dc); ReleaseDC(hwnd, dc); }
    if (msg == WM_PRINTCLIENT) Gutter(hwnd,reinterpret_cast<HDC>(wp));
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
    SendMessageW(hwnd, EM_SETTARGETDEVICE, 0, 1); // MSFTEDIT: horizontal scrolling, without wrapping to the window.
    ApplyTheme(hwnd);
    return hwnd;
}
void CodeEditor::SetLanguage(HWND hwnd, EditorLanguage language) { if (auto* state = GetState(hwnd)) { state->language = language; Highlight(*state); Hide(*state); } }
void CodeEditor::SetCompletions(HWND hwnd, const std::vector<std::wstring>& words) { if (auto* state = GetState(hwnd)) state->words = words; }
void CodeEditor::SetSqlSchema(HWND hwnd, const std::vector<TableMeta>& tables, const std::string& dialect) {
    if (auto* state = GetState(hwnd)) {
        const auto wide = [](const std::string& value) {
            if (value.empty()) return std::wstring{};
            const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
            if (!length) throw std::runtime_error("The database schema contains invalid UTF-8.");
            std::wstring result(length, L'\0'); MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length); return result;
        };
        std::vector<SqlTable> schema;
        for (const auto& metadata : tables) { SqlTable table; table.name = wide(metadata.name); for (const auto& column : metadata.columns) table.columns.push_back(wide(column.name)); schema.push_back(std::move(table)); }
        state->sqlTables = std::move(schema); state->sqlDialect = dialect; Hide(*state);
    }
}
std::wstring CodeEditor::GetText(HWND hwnd) {
    // GT_RAWTEXT uses the same character positions as TOM, including paragraph CRs.
    GETTEXTLENGTHEX length{GTL_NUMCHARS | GTL_PRECISE, 1200};
    auto count = SendMessageW(hwnd, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&length), 0);
    if (count < 0 || count > 16 * 1024 * 1024) throw std::runtime_error("The editor cannot read this text within its 16 MiB character limit.");
    std::wstring text(static_cast<size_t>(count) + 1, L'\0');
    GETTEXTEX get{static_cast<DWORD>(text.size() * sizeof(wchar_t)), GT_RAWTEXT, 1200, nullptr, nullptr};
    auto actual = SendMessageW(hwnd, EM_GETTEXTEX, reinterpret_cast<WPARAM>(&get), reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<size_t>((std::max)(0LL, static_cast<long long>(actual))));for(auto& character:text)if(character==L'\r')character=L'\n';return text;
}
void CodeEditor::SetText(HWND hwnd, const std::wstring& text) {if(text.size()>16*1024*1024)throw std::runtime_error("Text exceeds the 16 MiB character limit of this editor.");if(text.find(L'\0')!=std::wstring::npos)throw std::runtime_error("Binary text cannot be edited. Use a file attachment or export the response body.");SetWindowTextW(hwnd, text.c_str()); if (auto* state = GetState(hwnd)) { KillTimer(hwnd, HighlightTimer); Highlight(*state); Hide(*state); } }
bool CodeEditor::IsEditor(HWND hwnd) { return GetState(hwnd) != nullptr; }
void CodeEditor::ApplyTheme(HWND hwnd) {
    if (auto* state = GetState(hwnd)) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetCodeFont()), FALSE);
        SendMessageW(hwnd, EM_SETBKGNDCOLOR, 0, Theme::Get().bgPrimary);
        CHARFORMAT2W format{}; format.cbSize = sizeof(format); format.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE; format.crTextColor = Theme::Get().textPrimary;
        format.yHeight = MulDiv(12 * 20, Theme::GetTextScale(), 100); wcscpy_s(format.szFaceName, L"Consolas");
        SendMessageW(hwnd, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&format));
        IRichEditOle* ole=nullptr;ITextDocument* document=nullptr;SendMessageW(hwnd,EM_GETOLEINTERFACE,0,reinterpret_cast<LPARAM>(&ole));
        if(ole){ole->QueryInterface(__uuidof(ITextDocument),reinterpret_cast<void**>(&document));ole->Release();}
        if(document){LONG unused=0;document->Undo(tomSuspend,&unused);document->Freeze(&unused);state->formatting=true;
            SendMessageW(hwnd,EM_SETCHARFORMAT,SCF_ALL,reinterpret_cast<LPARAM>(&format));
            state->formatting=false;document->Unfreeze(&unused);document->Undo(tomResume,&unused);document->Release();}
        Highlight(*state);
    }
}
bool CodeEditor::RunSelfTests(std::wstring& failure) {
    auto check = [&](bool ok, const wchar_t* label) { if (!ok) failure += std::wstring(label) + L"; "; };
    const auto sql = Tokenize(L"SELECT 'it''s -- text', 42 /* comment */ FROM products", EditorLanguage::Sql);
    check(sql.size() == 5 && sql[1].kind == Kind::String && sql[3].kind == Kind::Comment, L"SQL literal and comment lexer");
    const auto json = Tokenize(L"{\"escaped\\\"key\": true, \"n\": -12.5}", EditorLanguage::Json);
    check(json.size() == 4 && json.back().kind == Kind::Number, L"JSON escape and number lexer");
    const auto script=Tokenize(L"const values = [true, 7]; // script",EditorLanguage::JavaScript);check(script.size()==4&&script[1].kind==Kind::Keyword&&script[2].kind==Kind::Number&&script[3].kind==Kind::Comment,L"JavaScript arrays and comments highlight correctly");
    const auto commands=Tokenize(L"GET \"item\"\nSCAN 0 COUNT 100",EditorLanguage::Shell);
    check(commands.size()==5&&commands[0].kind==Kind::Keyword&&commands[1].kind==Kind::String&&commands[2].kind==Kind::Keyword
        &&commands[3].kind==Kind::Number&&commands[4].kind==Kind::Number,L"Redis command highlighting leaves argument words uncolored");
    HWND parent = CreateWindowExW(0, L"STATIC", L"Editor tests", WS_OVERLAPPED, 0, 0, 600, 400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND editor = Create(parent, 1, EditorLanguage::Sql);
    if (!editor) { failure += L"Rich Edit unavailable"; if (parent) DestroyWindow(parent); return false; }
    SetWindowPos(editor,nullptr,0,0,260,160,SWP_NOZORDER);SetText(editor,L"SELECT a_very_long_column_name, another_long_column_name FROM products WHERE id = 42;");
    check(SendMessageW(editor,EM_GETLINECOUNT,0,0)==1,L"code line numbers do not count visual wrapping");
    SetWindowPos(editor,nullptr,0,0,220,160,SWP_NOZORDER);check(SendMessageW(editor,EM_GETLINECOUNT,0,0)==1,L"code horizontal scrolling survives resize");
    SetText(editor,L"-- first line\nSELECT 42;");check(GetText(editor)==L"-- first line\nSELECT 42;",L"editor paragraph readback uses LF without changing character positions");
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
    const auto table = [](const std::string& name, const std::vector<std::string>& columns) {
        TableMeta result; result.name = name; for (const auto& columnName : columns) { ColumnMeta column; column.name = columnName; result.columns.push_back(std::move(column)); } return result;
    };
    const auto products = table("products", {"id", "name", "unit price", "quote\"column"});
    const auto at = [&](const std::wstring& text, size_t caret) {
        SetText(editor, text); CHARRANGE point{static_cast<LONG>(caret), static_cast<LONG>(caret)}; SendMessageW(editor, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&point)); Complete(*GetState(editor), true);
    };
    SetSqlSchema(editor, {products, table("orders", {"id", "product_id"})}, "postgresql");
    const std::wstring starQuery=L"SELECT * FROM \"sales\".\"products\"";
    at(starQuery, 8);check(GetState(editor)->matches.size()==1&&GetState(editor)->matches[0].label==L"Expand * to columns",L"SQL wildcard exposes a named expansion");
    Accept(*GetState(editor));check(GetText(editor)==L"SELECT \"id\", \"name\", \"unit price\", \"quote\"\"column\" FROM \"sales\".\"products\"",
        L"SQL wildcard preserves schema column order and escaped provider quoting");
    Highlight(*GetState(editor));SendMessageW(editor,EM_UNDO,0,0);check(GetText(editor)==starQuery,L"wildcard completion and syntax coloring preserve a single undo");
    at(L"SELECT p.* FROM \"sales\".\"products\" AS p JOIN \"sales\".\"orders\" AS o ON o.product_id=p.id",10);
    Accept(*GetState(editor));check(GetText(editor)==L"SELECT p.\"id\", p.\"name\", p.\"unit price\", p.\"quote\"\"column\" FROM \"sales\".\"products\" AS p JOIN \"sales\".\"orders\" AS o ON o.product_id=p.id",
        L"qualified SQL wildcard resolves an alias among schema-qualified joins");
    at(L"SELECT p.na FROM \"sales\".\"products\" p",11);Accept(*GetState(editor));
    check(GetText(editor)==L"SELECT p.name FROM \"sales\".\"products\" p",L"column completion resolves a schema-qualified table alias");
    at(L"SELECT p.un FROM \"sales\".\"products\" p",11);Accept(*GetState(editor));
    check(GetText(editor)==L"SELECT p.\"unit price\" FROM \"sales\".\"products\" p",L"alias completion quotes columns that contain spaces");
    const std::wstring quotedAlias=L"SELECT \"product p\".na";at(quotedAlias+L" FROM \"sales\".\"products\" AS \"product p\"",quotedAlias.size());Accept(*GetState(editor));
    check(GetText(editor)==L"SELECT \"product p\".name FROM \"sales\".\"products\" AS \"product p\"",L"SQL completion preserves quoted aliases with spaces");
    const std::wstring quotedStar=L"SELECT \"product p\".*";at(quotedStar+L" FROM \"sales\".\"products\" AS \"product p\"",quotedStar.size());Accept(*GetState(editor));
    check(GetText(editor)==L"SELECT \"product p\".\"id\", \"product p\".\"name\", \"product p\".\"unit price\", \"product p\".\"quote\"\"column\" FROM \"sales\".\"products\" AS \"product p\"",
        L"SQL wildcard preserves a quoted alias when expanding columns");
    for(const auto& query:std::vector<std::wstring>{L"SELECT * FROM products p JOIN orders o ON p.id=o.product_id",L"SELECT * FROM products, orders"}) {
        at(query,8);check(GetState(editor)->matches.empty(),L"unqualified wildcard is suppressed with ambiguous table references");
    }
    at(L"UPDATE *",8);check(GetState(editor)->matches.empty(),L"UPDATE text does not expand a SQL wildcard");
    at(L"SELECT 2* FROM products",9);check(GetState(editor)->matches.empty(),L"arithmetic multiplication does not expand a SQL wildcard");
    at(L"SELECT COUNT(*) FROM products",14);check(GetState(editor)->matches.empty(),L"aggregate wildcard retains its original expression");
    at(L"SELECT * FROM products; SELECT * FROM orders",8);Accept(*GetState(editor));
    check(GetText(editor)==L"SELECT \"id\", \"name\", \"unit price\", \"quote\"\"column\" FROM products; SELECT * FROM orders",
        L"wildcard references are scoped to the current SQL statement");
    at(L"SELECT *, 'FROM imaginary x' FROM products /* JOIN imaginary y */",8);Accept(*GetState(editor));
    check(GetText(editor).find(L"SELECT \"id\", \"name\"")==0,L"wildcard reference parsing ignores SQL literal and comment text");
    SetSqlSchema(editor,{table("order\"items",{"id","name"})},"postgresql");
    at(L"SELECT o.na FROM \"sales\"\"reports\".\"order\"\"items\" AS o",11);Accept(*GetState(editor));
    check(GetText(editor)==L"SELECT o.name FROM \"sales\"\"reports\".\"order\"\"items\" AS o",L"SQL alias parsing decodes escaped quoted namespace identifiers");
    SetSqlSchema(editor,{table("products",{"id","unit]price"})},"mssql");
    at(L"SELECT p.* FROM [sales]]reports].[products] AS p",10);Accept(*GetState(editor));
    check(GetText(editor)==L"SELECT p.[id], p.[unit]]price] FROM [sales]]reports].[products] AS p",L"SQL Server wildcard uses escaped bracket identifiers");
    SetSqlSchema(editor,{table("products",{"id","unit`price"})},"mysql");
    at(L"SELECT p.* FROM `sales`.`products` AS p",10);Accept(*GetState(editor));
    check(GetText(editor)==L"SELECT p.`id`, p.`unit``price` FROM `sales`.`products` AS p",L"MySQL wildcard uses escaped backtick identifiers");
    SetSqlSchema(editor,{products},"sqlite");
    for(const auto& query:std::vector<std::wstring>{L"-- SEL",L"/* SEL",L"SELECT 'SEL",L"SELECT 'it''",L"SELECT $tag$ SEL"}) {
        at(query,query.size());check(GetState(editor)->matches.empty(),L"SQL completion stays suppressed inside comments and unterminated literals");
    }
    SetLanguage(editor,EditorLanguage::Json);SetCompletions(editor,{L"{{token}}"});SetText(editor,L"{\"value\": \"{{to\"}");range={15,15};SendMessageW(editor,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&range));
    Complete(*GetState(editor),true);Accept(*GetState(editor));check(GetText(editor)==L"{\"value\": \"{{token}}\"}",L"variables complete inside JSON strings");
    SetCompletions(editor,{L"operation",L"$match"});
    at(L"{\"oper\"}",6);Accept(*GetState(editor));check(GetText(editor)==L"{\"operation\"}",L"Mongo JSON property names complete inside quoted strings");
    at(L"{\"$mat\"}",6);Accept(*GetState(editor));check(GetText(editor)==L"{\"$match\"}",L"Mongo JSON operator completion recognizes dollar prefixes");
    SetLanguage(editor,EditorLanguage::JavaScript);SetCompletions(editor,{});SetText(editor,L"pm.response.j");SendMessageW(editor,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&end));Complete(*GetState(editor),true);Accept(*GetState(editor));check(GetText(editor)==L"pm.response.json",L"native request script API completion");
    for(const auto& method:std::vector<std::wstring>{L"pm.variables.unset",L"pm.environment.replaceIn",L"pm.collectionVariables.toObject",L"pm.globals.has",L"pm.iterationData.get",L"pm.request.headers.remove",L"pm.response.headers.has",L"pm.info.iteration",L"console.warn"}) {
        const auto prefix=method.substr(0,method.size()-1);at(prefix,prefix.size());
        check(std::any_of(GetState(editor)->matches.begin(),GetState(editor)->matches.end(),[&](const auto& candidate){return candidate.text==method;}),L"native script completion includes original pm scope and request methods");
    }
    SetCompletions(editor,{L"_nonce",L"$tenant",L"nonce",L"team\"name\\path",L"team'name\\path",L"space key"});
    for(const auto* scope:{L"variables",L"environment",L"collectionVariables",L"globals",L"iterationData"})for(const auto* method:{L"get",L"set",L"has",L"unset"}) {
        const std::wstring prefix=std::wstring(L"pm.")+scope+L"."+method+L"(\"no";
        at(prefix+L"\")",prefix.size());Accept(*GetState(editor));
        check(GetText(editor)==std::wstring(L"pm.")+scope+L"."+method+L"(\"nonce\")",L"scoped script key completion works in each original string argument");
    }
    const std::wstring underscore=L"pm.variables.get(\"_n";at(underscore+L"\")",underscore.size());Accept(*GetState(editor));check(GetText(editor)==L"pm.variables.get(\"_nonce\")",L"script variable keys preserve underscore prefixes");
    const std::wstring dollar=L"pm.iterationData.get('$t";at(dollar+L"')",dollar.size());Accept(*GetState(editor));
    check(GetText(editor)==L"pm.iterationData.get('$tenant')",L"script variable keys preserve dollar prefixes");
    const std::wstring doubleQuoted=L"pm.variables.get(\"team\\\"";at(doubleQuoted+L"\")",doubleQuoted.size());Accept(*GetState(editor));
    check(GetText(editor)==L"pm.variables.get(\"team\\\"name\\\\path\")",L"script completion escapes active double quotes and backslashes");
    const std::wstring singleQuoted=L"pm.variables.get('team\\'";at(singleQuoted+L"')",singleQuoted.size());Accept(*GetState(editor));
    check(GetText(editor)==L"pm.variables.get('team\\'name\\\\path')",L"script completion escapes active single quotes and backslashes");
    SendMessageW(editor,EM_UNDO,0,0);check(GetText(editor)==singleQuoted+L"')",L"script string completion remains undoable");
    const std::wstring spaced=L"pm.variables.get(\"space ";at(spaced+L"\")",spaced.size());Accept(*GetState(editor));
    check(GetText(editor)==L"pm.variables.get(\"space key\")",L"script string arguments complete arbitrary keys with spaces");
    for(const auto& scriptText:std::vector<std::wstring>{L"// pm.variables.get(\"no",L"/* pm.variables.get(\"no",L"\"pm.variables.get('no",L"\"ordinary no"}) {
        at(scriptText,scriptText.size());check(GetState(editor)->matches.empty(),L"script completion suppresses comments and ordinary string literals");
    }
    DestroyWindow(parent); return failure.empty();
}
}
