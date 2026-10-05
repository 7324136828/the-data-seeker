#include <objbase.h>
#include "ApiClientView.h"
#include <windowsx.h>
#include <sstream>
#include <thread>
#include <json.hpp>
#include "CodeEditor.h"
#include "NativeDialogs.h"
#include "JsonSerialization.h"
#include "ScriptStore.h"
#include "OpenApiParser.h"
#include "VariableResolver.h"
#include <cwctype>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <set>
#include <wincrypt.h>
#pragma comment(lib,"crypt32.lib")

namespace native_app {

static constexpr wchar_t ApiClientViewClassName[] = L"DataForgeApiClientViewClass";
static constexpr UINT WM_APP_REQ_COMPLETED = WM_APP + 1;
static constexpr UINT WM_APP_RUNNER_COMPLETED = WM_APP + 2;

static std::wstring ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int sz = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring w(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), &w[0], sz);
    return w;
}

static std::string ToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int sz = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string s(sz, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), &s[0], sz, nullptr, nullptr);
    return s;
}

static std::string NewUiId(const char* prefix) {
    GUID id{}; if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("Windows could not create a unique item identity.");
    wchar_t text[40]{}; StringFromGUID2(id,text,40); return prefix+ToUtf8(text);
}

ApiClientView::ApiClientView(StoreManager* storeMgr, HttpEngine* httpEngine)
    : storeMgr_(storeMgr), httpEngine_(httpEngine) {}

ApiClientView::~ApiClientView() {
    RequestCancel();
    for(auto& job:jobs_)if(job->worker.joinable())job->worker.join();
    if(hRunner_&&IsWindow(hRunner_))DestroyWindow(hRunner_);
    if (hWnd_ && IsWindow(hWnd_)) DestroyWindow(hWnd_);
}

bool ApiClientView::Create(HWND hParent, int x, int y, int width, int height, UINT id) {
    hParent_ = hParent;
    width_ = width;
    height_ = height;

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = ApiClientViewClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    hWnd_ = CreateWindowExW(
        WS_EX_CONTROLPARENT, ApiClientViewClassName, L"API Client",
        WS_CHILD | WS_CLIPCHILDREN,
        x, y, width, height,
        hParent, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
        GetModuleHandle(nullptr), this
    );

    if (!hWnd_) return false;

    // Sidebar: Collections Tree
    hTreeCollections_ = CreateWindowExW(
        0, WC_TREEVIEWW, L"",
        WS_CHILD | WS_VISIBLE | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS | WS_BORDER,
        0, 0, 100, 100, hWnd_, reinterpret_cast<HMENU>(2001), GetModuleHandle(nullptr), nullptr
    );
    SendMessage(hTreeCollections_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);
    hTreeFilter_=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,0,0,1,1,hWnd_,reinterpret_cast<HMENU>(2003),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(hTreeFilter_,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"Search requests / collections"));SendMessageW(hTreeFilter_,EM_SETLIMITTEXT,256,0);

    // Sidebar: History List
    hListHistory_ = CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER,
        0, 0, 100, 100, hWnd_, reinterpret_cast<HMENU>(2002), GetModuleHandle(nullptr), nullptr
    );
    ListView_SetExtendedListViewStyle(hListHistory_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    LVCOLUMNW lvc{};
    lvc.mask = LVCF_TEXT | LVCF_WIDTH;
    lvc.cx = 60; lvc.pszText = const_cast<LPWSTR>(L"Method"); ListView_InsertColumn(hListHistory_, 0, &lvc);
    lvc.cx = 50; lvc.pszText = const_cast<LPWSTR>(L"Status"); ListView_InsertColumn(hListHistory_, 1, &lvc);
    lvc.cx = 150; lvc.pszText = const_cast<LPWSTR>(L"URL"); ListView_InsertColumn(hListHistory_, 2, &lvc);
    SendMessage(hListHistory_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);

    // Sidebar buttons
    hBtnAddCol_ = CreateWindowExW(0, L"BUTTON", L"+ Collection", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 90, 24, hWnd_, reinterpret_cast<HMENU>(2010), GetModuleHandle(nullptr), nullptr);
    hBtnAddReq_ = CreateWindowExW(0, L"BUTTON", L"+ Request", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 80, 24, hWnd_, reinterpret_cast<HMENU>(2011), GetModuleHandle(nullptr), nullptr);
    hBtnRunAll_ = CreateWindowExW(0, L"BUTTON", L"Run All", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 70, 24, hWnd_, reinterpret_cast<HMENU>(2012), GetModuleHandle(nullptr), nullptr);
    hBtnClearHist_ = CreateWindowExW(0, L"BUTTON", L"Clear", WS_CHILD | BS_PUSHBUTTON,
        0, 0, 60, 24, hWnd_, reinterpret_cast<HMENU>(2013), GetModuleHandle(nullptr), nullptr);

    SendMessage(hBtnAddCol_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetSmallFont()), TRUE);
    SendMessage(hBtnAddReq_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetSmallFont()), TRUE);
    SendMessage(hBtnRunAll_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetSmallFont()), TRUE);
    SendMessage(hBtnClearHist_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetSmallFont()), TRUE);

    // Request Bar: Method Combobox
    hComboMethod_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 90, 200, hWnd_, reinterpret_cast<HMENU>(2101), GetModuleHandle(nullptr), nullptr);
    static const wchar_t* methods[] = { L"GET", L"POST", L"PUT", L"DELETE", L"PATCH", L"HEAD", L"OPTIONS" };
    for (const auto* m : methods) SendMessage(hComboMethod_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(m));
    SendMessage(hComboMethod_, CB_SETCURSEL, 0, 0);
    SendMessage(hComboMethod_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetBoldFont()), TRUE);

    // Request Bar: URL Edit
    hEditUrl_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        0, 0, 300, 28, hWnd_, reinterpret_cast<HMENU>(2102), GetModuleHandle(nullptr), nullptr);
    SendMessage(hEditUrl_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);

    // Request Bar: Send and Code Buttons
    hBtnSend_ = CreateWindowExW(0, L"BUTTON", L"Send", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 75, 28, hWnd_, reinterpret_cast<HMENU>(2103), GetModuleHandle(nullptr), nullptr);
    hBtnCode_ = CreateWindowExW(0, L"BUTTON", L"Code", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 60, 28, hWnd_, reinterpret_cast<HMENU>(2104), GetModuleHandle(nullptr), nullptr);
    SendMessage(hBtnSend_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetBoldFont()), TRUE);
    SendMessage(hBtnCode_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);

    // Sub-tab: Params List
    hListParams_ = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | WS_BORDER,
        0, 0, 100, 100, hWnd_, reinterpret_cast<HMENU>(2201), GetModuleHandle(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(hListParams_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    lvc.cx = 120; lvc.pszText = const_cast<LPWSTR>(L"Key"); ListView_InsertColumn(hListParams_, 0, &lvc);
    lvc.cx = 200; lvc.pszText = const_cast<LPWSTR>(L"Value"); ListView_InsertColumn(hListParams_, 1, &lvc);
    SendMessage(hListParams_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);

    // Sub-tab: Headers List
    hListHeaders_ = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | WS_BORDER,
        0, 0, 100, 100, hWnd_, reinterpret_cast<HMENU>(2202), GetModuleHandle(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(hListHeaders_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    lvc.cx = 140; lvc.pszText = const_cast<LPWSTR>(L"Header"); ListView_InsertColumn(hListHeaders_, 0, &lvc);
    lvc.cx = 250; lvc.pszText = const_cast<LPWSTR>(L"Value"); ListView_InsertColumn(hListHeaders_, 1, &lvc);
    SendMessage(hListHeaders_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);

    // Sub-tab: Auth Controls
    hComboAuthType_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 120, 150, hWnd_, reinterpret_cast<HMENU>(2203), GetModuleHandle(nullptr), nullptr);
    static const wchar_t* auths[] = { L"No Auth", L"Bearer Token", L"Basic Auth", L"API Key", L"Inherit" };
    for (const auto* a : auths) SendMessage(hComboAuthType_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(a));
    SendMessage(hComboAuthType_, CB_SETCURSEL, 0, 0);
    SendMessage(hComboAuthType_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);
    hComboAuthLocation_=CreateWindowExW(0,L"COMBOBOX",L"API key location",WS_CHILD|WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,0,0,200,150,hWnd_,reinterpret_cast<HMENU>(2209),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(hComboAuthLocation_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"API key in header"));SendMessageW(hComboAuthLocation_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"API key in query"));SendMessageW(hComboAuthLocation_,CB_SETCURSEL,0,0);

    hEditAuthToken_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
        0, 0, 250, 24, hWnd_, reinterpret_cast<HMENU>(2204), GetModuleHandle(nullptr), nullptr);
    hEditAuthUser_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
        0, 0, 150, 24, hWnd_, reinterpret_cast<HMENU>(2205), GetModuleHandle(nullptr), nullptr);
    hEditAuthPass_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL,
        0, 0, 150, 24, hWnd_, reinterpret_cast<HMENU>(2206), GetModuleHandle(nullptr), nullptr);
    hEditAuthKey_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
        0, 0, 150, 24, hWnd_, reinterpret_cast<HMENU>(2207), GetModuleHandle(nullptr), nullptr);
    hEditAuthVal_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
        0, 0, 150, 24, hWnd_, reinterpret_cast<HMENU>(2208), GetModuleHandle(nullptr), nullptr);

    SendMessage(hEditAuthToken_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);
    SendMessage(hEditAuthUser_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);
    SendMessage(hEditAuthPass_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);
    SendMessage(hEditAuthKey_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);
    SendMessage(hEditAuthVal_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);

    // Sub-tab: Body Controls
    hComboBodyType_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 140, 150, hWnd_, reinterpret_cast<HMENU>(2210), GetModuleHandle(nullptr), nullptr);
    static const wchar_t* bodyTypes[] = { L"None", L"JSON", L"Raw Text", L"x-www-form-urlencoded", L"form-data" };
    for (const auto* b : bodyTypes) SendMessage(hComboBodyType_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(b));
    SendMessage(hComboBodyType_, CB_SETCURSEL, 0, 0);
    SendMessage(hComboBodyType_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);

    hEditBody_ = CodeEditor::Create(hWnd_, 2211, EditorLanguage::Json);
    SendMessage(hEditBody_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetCodeFont()), TRUE);

    // Sub-tab: Tests List
    hListTests_ = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | WS_BORDER,
        0, 0, 100, 100, hWnd_, reinterpret_cast<HMENU>(2220), GetModuleHandle(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(hListTests_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    lvc.cx = 120; lvc.pszText = const_cast<LPWSTR>(L"Type"); ListView_InsertColumn(hListTests_, 0, &lvc);
    lvc.cx = 250; lvc.pszText = const_cast<LPWSTR>(L"Assertion Name / Expected"); ListView_InsertColumn(hListTests_, 1, &lvc);
    SendMessage(hListTests_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);

    hBtnAddTest_ = CreateWindowExW(0, L"BUTTON", L"+ Add Test", WS_CHILD | BS_PUSHBUTTON,
        0, 0, 80, 24, hWnd_, reinterpret_cast<HMENU>(2221), GetModuleHandle(nullptr), nullptr);
    SendMessage(hBtnAddTest_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetSmallFont()), TRUE);

    // Response Viewer: Body Edit (Read-Only)
    hEditRespBody_ = CodeEditor::Create(hWnd_, 2301, EditorLanguage::PlainText, true);
    CodeEditor::SetText(hEditRespBody_, L"Enter a URL and send a request to view its response.");
    SendMessage(hEditRespBody_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetCodeFont()), TRUE);

    // Response Viewer: Headers List
    hListRespHeaders_ = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | WS_BORDER,
        0, 0, 100, 100, hWnd_, reinterpret_cast<HMENU>(2302), GetModuleHandle(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(hListRespHeaders_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    lvc.cx = 150; lvc.pszText = const_cast<LPWSTR>(L"Header"); ListView_InsertColumn(hListRespHeaders_, 0, &lvc);
    lvc.cx = 300; lvc.pszText = const_cast<LPWSTR>(L"Value"); ListView_InsertColumn(hListRespHeaders_, 1, &lvc);
    SendMessage(hListRespHeaders_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);

    // Response Viewer: Test Results List
    hListRespTests_ = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | WS_BORDER,
        0, 0, 100, 100, hWnd_, reinterpret_cast<HMENU>(2303), GetModuleHandle(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(hListRespTests_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    lvc.cx = 60; lvc.pszText = const_cast<LPWSTR>(L"Result"); ListView_InsertColumn(hListRespTests_, 0, &lvc);
    lvc.cx = 180; lvc.pszText = const_cast<LPWSTR>(L"Assertion"); ListView_InsertColumn(hListRespTests_, 1, &lvc);
    lvc.cx = 250; lvc.pszText = const_cast<LPWSTR>(L"Details"); ListView_InsertColumn(hListRespTests_, 2, &lvc);
    SendMessage(hListRespTests_, WM_SETFONT, reinterpret_cast<WPARAM>(Theme::GetMainFont()), TRUE);

    hEditName_ = CreateWindowExW(0, L"EDIT", L"Untitled Request", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 0,0,200,34,hWnd_,reinterpret_cast<HMENU>(2105),GetModuleHandleW(nullptr),nullptr);
    hBtnSave_ = CreateWindowExW(0, L"BUTTON", L"Save request", WS_CHILD | WS_VISIBLE, 0,0,120,34,hWnd_,reinterpret_cast<HMENU>(2110),GetModuleHandleW(nullptr),nullptr);
    hBtnEditConfig_ = CreateWindowExW(0, L"BUTTON", L"Edit parameters", WS_CHILD, 0,0,160,34,hWnd_,reinterpret_cast<HMENU>(2222),GetModuleHandleW(nullptr),nullptr);
    hEditVariables_ = CodeEditor::Create(hWnd_, 2230, EditorLanguage::Json);
    hEditOptions_ = CodeEditor::Create(hWnd_, 2231, EditorLanguage::Json);
    hEditPreScript_=CodeEditor::Create(hWnd_,2232,EditorLanguage::JavaScript);
    hEditPostScript_=CodeEditor::Create(hWnd_,2233,EditorLanguage::JavaScript);
    hEditScriptConsole_=CodeEditor::Create(hWnd_,2304,EditorLanguage::PlainText,true);
    CodeEditor::SetText(hEditVariables_, L"[]");
    CodeEditor::SetText(hEditOptions_, L"{\"timeout\": 20, \"follow_redirects\": true, \"verify_tls\": true}");
    auto button = [&](const wchar_t* text, int id) { return CreateWindowExW(0,L"BUTTON",text,WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,100,34,hWnd_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr); };
    const wchar_t* requestNames[] = {L"Params",L"Headers",L"Auth",L"Body",L"Tests",L"Variables",L"Options",L"Pre-request",L"Post-response"};
    for(int i=0;i<9;++i) hRequestTabs_[i]=button(requestNames[i],2400+i);
    const wchar_t* responseNames[] = {L"Body",L"Headers",L"Test results",L"Console"};
    for(int i=0;i<4;++i) hResponseTabs_[i]=button(responseNames[i],i==3?2414:2410+i);
    hBtnSaveResponse_=button(L"Save body",2413);EnableWindow(hBtnSaveResponse_,FALSE);
    hSidebarTabs_[0]=button(L"Collections",2420); hSidebarTabs_[1]=button(L"History",2421);
    hDocumentSelector_=CreateWindowExW(0,L"COMBOBOX",L"Request documents",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,0,0,300,200,hWnd_,reinterpret_cast<HMENU>(2430),GetModuleHandleW(nullptr),nullptr);
    hBtnNewDocument_=button(L"+ Request",2431);hBtnCloseDocument_=button(L"Close",2432);
    hBtnAttachFile_=button(L"Attach file",2212);
    RequestDocument initial;initial.identity=NewUiId("doc_");documents_.push_back(initial);
    LoadRequest(initial.request);RefreshDocumentSelector();
    ApplyTheme();
    RefreshData();
    Layout();
    return true;
}

void ApiClientView::Show(bool bShow) {
    ShowWindow(hWnd_, bShow ? SW_SHOW : SW_HIDE);
}

void ApiClientView::Resize(int x, int y, int width, int height) {
    width_ = width;
    height_ = height;
    SetWindowPos(hWnd_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    Layout();
    InvalidateRect(hWnd_, nullptr, TRUE);
}

void ApiClientView::RefreshData() {
    CaptureDocument();
    PopulateCollectionsTree();
    for(auto& doc:documents_)for(const auto& bound:treeRequests_)if(!doc.request.id.empty()&&doc.request.id==bound.request.id){
        doc.scopes=bound.scopes;doc.inheritedAuth=bound.inheritedAuth;doc.collectionId=bound.collectionId;doc.folderId=bound.folderId;doc.preScripts=bound.preScripts;doc.postScripts=bound.postScripts;break;
    }
    if(!documents_.empty()){const auto& doc=documents_[activeDocument_];selectedScopes_=doc.scopes;selectedAuth_=doc.inheritedAuth;selectedCollectionId_=doc.collectionId;selectedFolderId_=doc.folderId;selectedPreScripts_=doc.preScripts;selectedPostScripts_=doc.postScripts;}
    PopulateHistoryList();
    ApplyTheme();
}

void ApiClientView::CaptureDocument() {
    if(documents_.empty()||activeDocument_>=documents_.size())return;
    auto read=[](HWND control){const int length=GetWindowTextLengthW(control);std::wstring text(static_cast<size_t>(length)+1,L'\0');GetWindowTextW(control,text.data(),length+1);text.resize(length);return ToUtf8(text);};
    auto& doc=documents_[activeDocument_];doc.request=currentRequest_;doc.request.name=read(hEditName_);doc.request.url=read(hEditUrl_);doc.request.method=static_cast<HttpMethod>(SendMessageW(hComboMethod_,CB_GETCURSEL,0,0));
    doc.request.auth.type=static_cast<AuthType>(SendMessageW(hComboAuthType_,CB_GETCURSEL,0,0));doc.request.auth.token=read(hEditAuthToken_);doc.request.auth.username=read(hEditAuthUser_);doc.request.auth.password=read(hEditAuthPass_);doc.request.auth.key=read(hEditAuthKey_);doc.request.auth.value=read(hEditAuthVal_);
    doc.request.auth.in=SendMessageW(hComboAuthLocation_,CB_GETCURSEL,0,0)==1?"query":"header";
    doc.request.body.type=static_cast<BodyType>(SendMessageW(hComboBodyType_,CB_GETCURSEL,0,0));
    doc.bodyDraft=CodeEditor::GetText(hEditBody_);doc.variablesDraft=CodeEditor::GetText(hEditVariables_);doc.optionsDraft=CodeEditor::GetText(hEditOptions_);doc.preDraft=CodeEditor::GetText(hEditPreScript_);doc.postDraft=CodeEditor::GetText(hEditPostScript_);
    doc.request.preRequestScript=ToUtf8(doc.preDraft);doc.request.postResponseScript=ToUtf8(doc.postDraft);doc.response=lastResponse_;doc.scopes=selectedScopes_;doc.inheritedAuth=selectedAuth_;doc.collectionId=selectedCollectionId_;doc.folderId=selectedFolderId_;doc.preScripts=selectedPreScripts_;doc.postScripts=selectedPostScripts_;doc.initialized=true;
}
void ApiClientView::LoadFieldsPreservingDrafts(const ApiRequest& request){
    const auto body=CodeEditor::GetText(hEditBody_),variables=CodeEditor::GetText(hEditVariables_),options=CodeEditor::GetText(hEditOptions_),pre=CodeEditor::GetText(hEditPreScript_),post=CodeEditor::GetText(hEditPostScript_);LoadRequest(request);CodeEditor::SetText(hEditBody_,body);CodeEditor::SetText(hEditVariables_,variables);CodeEditor::SetText(hEditOptions_,options);CodeEditor::SetText(hEditPreScript_,pre);CodeEditor::SetText(hEditPostScript_,post);CaptureDocument();
}
void ApiClientView::RefreshDocumentSelector() {
    SendMessageW(hDocumentSelector_,CB_RESETCONTENT,0,0);
    for(const auto& doc:documents_){const auto name=L"["+ToWide(HttpMethodToString(doc.request.method))+L"] "+ToWide(doc.request.name);SendMessageW(hDocumentSelector_,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));}
    SendMessageW(hDocumentSelector_,CB_SETCURSEL,activeDocument_,0);EnableWindow(hBtnCloseDocument_,!documents_.empty());
}
void ApiClientView::ActivateDocument(size_t index) {
    if(index>=documents_.size())return;CaptureDocument();activeDocument_=index;activatingDocument_=true;
    auto& doc=documents_[index];LoadRequest(doc.request);selectedScopes_=doc.scopes;selectedAuth_=doc.inheritedAuth;selectedCollectionId_=doc.collectionId;selectedFolderId_=doc.folderId;selectedPreScripts_=doc.preScripts;selectedPostScripts_=doc.postScripts;
    if(doc.initialized){CodeEditor::SetText(hEditBody_,doc.bodyDraft);CodeEditor::SetText(hEditVariables_,doc.variablesDraft);CodeEditor::SetText(hEditOptions_,doc.optionsDraft);CodeEditor::SetText(hEditPreScript_,doc.preDraft);CodeEditor::SetText(hEditPostScript_,doc.postDraft);}
    lastResponse_=doc.response;DisplayResponse(lastResponse_);activatingDocument_=false;ApplyTheme();RefreshDocumentSelector();UpdateExecutionButtons();
}
void ApiClientView::OpenRequest(const ApiRequest& request) {
    CaptureDocument();if(!request.id.empty())for(size_t i=0;i<documents_.size();++i)if(documents_[i].request.id==request.id){ActivateDocument(i);return;}
    RequestDocument doc;doc.identity=NewUiId("doc_");doc.request=request;documents_.push_back(std::move(doc));ActivateDocument(documents_.size()-1);
}
void ApiClientView::NewDocument(){ApiRequest request;OpenRequest(request);}
void ApiClientView::CycleDocument(bool backwards){if(!documents_.empty())ActivateDocument((activeDocument_+documents_.size()+(backwards?-1:1))%documents_.size());}
void ApiClientView::CloseDocument(bool confirm) {
    if(documents_.empty())return;
    if(confirm&&MessageBoxW(hWnd_,L"Close this request document? Save the request first to keep unsaved changes.",L"Close request",MB_YESNO|MB_ICONQUESTION)!=IDYES)return;
    documents_.erase(documents_.begin()+activeDocument_);if(documents_.empty()){RequestDocument doc;doc.identity=NewUiId("doc_");documents_.push_back(doc);}activeDocument_=(std::min)(activeDocument_,documents_.size()-1);
    // The previous document was removed; do not capture its controls into the next one.
    activatingDocument_=true;auto& doc=documents_[activeDocument_];LoadRequest(doc.request);selectedScopes_=doc.scopes;selectedAuth_=doc.inheritedAuth;selectedCollectionId_=doc.collectionId;selectedFolderId_=doc.folderId;selectedPreScripts_=doc.preScripts;selectedPostScripts_=doc.postScripts;
    if(doc.initialized){CodeEditor::SetText(hEditBody_,doc.bodyDraft);CodeEditor::SetText(hEditVariables_,doc.variablesDraft);CodeEditor::SetText(hEditOptions_,doc.optionsDraft);CodeEditor::SetText(hEditPreScript_,doc.preDraft);CodeEditor::SetText(hEditPostScript_,doc.postDraft);}
    lastResponse_=doc.response;DisplayResponse(lastResponse_);activatingDocument_=false;RefreshDocumentSelector();ApplyTheme();UpdateExecutionButtons();
}

void ApiClientView::PopulateCollectionsTree() {
    TreeView_DeleteAllItems(hTreeCollections_); treeRequests_.clear();treeNodes_.clear();
    auto node=[&](HTREEITEM parent,const std::wstring& text,LPARAM data=0) {
        TVINSERTSTRUCTW item{};item.hParent=parent;item.hInsertAfter=TVI_LAST;item.item.mask=TVIF_TEXT|TVIF_PARAM;item.item.pszText=const_cast<LPWSTR>(text.c_str());item.item.lParam=data;
        return TreeView_InsertItem(hTreeCollections_,&item);
    };
    wchar_t filter[257]{};GetWindowTextW(hTreeFilter_,filter,257);std::wstring search(filter);std::transform(search.begin(),search.end(),search.begin(),[](wchar_t value){return static_cast<wchar_t>(towlower(value));});
    auto matches=[&](const std::string& value){auto text=ToWide(value);std::transform(text.begin(),text.end(),text.begin(),[](wchar_t character){return static_cast<wchar_t>(towlower(character));});return search.empty()||text.find(search)!=std::wstring::npos;};
    std::function<bool(const Folder&)> folderMatches=[&](const Folder& f){if(matches(f.name))return true;for(const auto& request:f.requests)if(matches(request.name+" "+request.url+" "+HttpMethodToString(request.method)))return true;for(const auto& child:f.folders)if(folderMatches(child))return true;return false;};
    auto requests=[&](HTREEITEM parent,const std::vector<ApiRequest>& list,const std::vector<Variable>& scopes,const AuthConfig& auth,const std::string& collectionId,const std::string& folderId,const std::vector<ScriptEntry>& pre,const std::vector<ScriptEntry>& post) {
        for(const auto& request:list){treeRequests_.push_back({request,scopes,auth,collectionId,folderId,pre,post});if(matches(request.name+" "+request.url+" "+HttpMethodToString(request.method)))node(parent,L"["+ToWide(HttpMethodToString(request.method))+L"] "+ToWide(request.name),static_cast<LPARAM>(treeRequests_.size()));}
    };
    std::function<void(HTREEITEM,const Folder&,std::vector<Variable>,AuthConfig,const std::string&,std::vector<ScriptEntry>,std::vector<ScriptEntry>)> folder;
    folder=[&](HTREEITEM parent,const Folder& f,std::vector<Variable> scopes,AuthConfig auth,const std::string& collectionId,std::vector<ScriptEntry> pre,std::vector<ScriptEntry> post){
        treeNodes_.push_back({f.id,collectionId,f.id,true});const auto item=parent&&folderMatches(f)?node(parent,ToWide(f.name),-static_cast<LPARAM>(treeNodes_.size())):nullptr;scopes.insert(scopes.end(),f.variables.begin(),f.variables.end());if(f.auth.type!=AuthType::Inherit)auth=f.auth;
        if(!f.preRequestScript.empty())pre.push_back({f.name,f.preRequestScript});if(!f.postResponseScript.empty())post.push_back({f.name,f.postResponseScript});
        requests(item,f.requests,scopes,auth,collectionId,f.id,pre,post);for(const auto& child:f.folders)folder(item,child,scopes,auth,collectionId,pre,post);if(item)TreeView_Expand(hTreeCollections_,item,TVE_EXPAND);
    };
    for(const auto& collection:storeMgr_->GetCollections()) {
        bool visible=matches(collection.name);for(const auto& request:collection.requests)visible=visible||matches(request.name+" "+request.url+" "+HttpMethodToString(request.method));for(const auto& f:collection.folders)visible=visible||folderMatches(f);
        treeNodes_.push_back({collection.id,collection.id,"",false});auto item=visible?node(TVI_ROOT,ToWide(collection.name),-static_cast<LPARAM>(treeNodes_.size())):nullptr;std::vector<ScriptEntry> pre,post;
        if(!collection.preRequestScript.empty())pre.push_back({collection.name,collection.preRequestScript});if(!collection.postResponseScript.empty())post.push_back({collection.name,collection.postResponseScript});
        requests(item,collection.requests,collection.variables,collection.auth,collection.id,"",pre,post);
        for(const auto& f:collection.folders)folder(item,f,collection.variables,collection.auth,collection.id,pre,post);
        if(item)TreeView_Expand(hTreeCollections_,item,TVE_EXPAND);
    }
}

void ApiClientView::PopulateHistoryList() {
    ListView_DeleteAllItems(hListHistory_);
    auto history = storeMgr_->GetHistory();

    for (int i = 0; i < static_cast<int>(history.size()); ++i) {
        const auto& h = history[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        std::wstring wMethod = ToWide(h.method);
        item.pszText = const_cast<LPWSTR>(wMethod.c_str());
        ListView_InsertItem(hListHistory_, &item);

        std::wstring wStatus = std::to_wstring(h.statusCode);
        ListView_SetItemText(hListHistory_, i, 1, const_cast<LPWSTR>(wStatus.c_str()));

        std::wstring wUrl = ToWide(h.url);
        ListView_SetItemText(hListHistory_, i, 2, const_cast<LPWSTR>(wUrl.c_str()));
    }
}

namespace {
Folder* FindFolder(std::vector<Folder>& folders,const std::string& id){for(auto& folder:folders){if(folder.id==id)return &folder;if(auto* found=FindFolder(folder.folders,id))return found;}return nullptr;}
bool RemoveFolder(std::vector<Folder>& folders,const std::string& id){for(auto it=folders.begin();it!=folders.end();++it){if(it->id==id){folders.erase(it);return true;}if(RemoveFolder(it->folders,id))return true;}return false;}
ApiRequest* FindRequest(Collection& collection,const std::string& id){
    std::function<ApiRequest*(std::vector<ApiRequest>&,std::vector<Folder>&)> find=[&](auto& requests,auto& folders)->ApiRequest*{for(auto& request:requests)if(request.id==id)return &request;for(auto& folder:folders)if(auto* found=find(folder.requests,folder.folders))return found;return nullptr;};return find(collection.requests,collection.folders);
}
bool RemoveRequest(Collection& collection,const std::string& id){std::function<bool(std::vector<ApiRequest>&,std::vector<Folder>&)> remove=[&](auto& requests,auto& folders){for(auto it=requests.begin();it!=requests.end();++it)if(it->id==id){requests.erase(it);return true;}for(auto& folder:folders)if(remove(folder.requests,folder.folders))return true;return false;};return remove(collection.requests,collection.folders);}
Collection FolderCollection(const Folder& folder){Collection value;value.id=folder.id;value.name=folder.name;value.description=folder.description;value.requests=folder.requests;value.folders=folder.folders;value.variables=folder.variables;value.auth=folder.auth;value.preRequestScript=folder.preRequestScript;value.postResponseScript=folder.postResponseScript;return value;}
Folder CollectionFolder(const Collection& value){Folder folder;folder.id=value.id;folder.name=value.name;folder.description=value.description;folder.requests=value.requests;folder.folders=value.folders;folder.variables=value.variables;folder.auth=value.auth;folder.preRequestScript=value.preRequestScript;folder.postResponseScript=value.postResponseScript;return folder;}
LPARAM SelectedTreeData(HWND tree){TVITEMW item{};item.mask=TVIF_PARAM;item.hItem=TreeView_GetSelection(tree);return item.hItem&&TreeView_GetItem(tree,&item)?item.lParam:0;}
std::string ReadImportText(const std::wstring& path){const std::filesystem::path filePath(path);if(!std::filesystem::is_regular_file(filePath)||std::filesystem::file_size(filePath)>16*1024*1024)throw std::runtime_error("Choose a regular UTF-8 import file of at most 16 MiB.");std::ifstream file(filePath,std::ios::binary);if(!file)throw std::runtime_error("The import file could not be opened.");std::string text((std::istreambuf_iterator<char>(file)),{});if(file.bad())throw std::runtime_error("The import file could not be read completely.");if(!text.empty()&&MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0)==0)throw std::runtime_error("The import file must use UTF-8 text.");return text;}
}

void ApiClientView::ImportCollectionFile(){
    const auto path=ChooseNativeFile(hWnd_,L"Import collection, Postman, or OpenAPI",L"API files\0*.json;*.yaml;*.yml\0All files\0*.*\0\0",false);if(path.empty())return;
    const auto text=ReadImportText(path);const auto imported=CollectionJson::Import(text);
    if(imported.empty())throw std::runtime_error("This file contains no collections.");auto collections=storeMgr_->GetCollections();collections.insert(collections.end(),imported.begin(),imported.end());storeMgr_->SaveCollections(collections);RefreshData();
}
void ApiClientView::TreeMenu(){
    POINT point{};GetCursorPos(&point);POINT client=point;ScreenToClient(hTreeCollections_,&client);TVHITTESTINFO hit{};hit.pt=client;if(auto item=TreeView_HitTest(hTreeCollections_,&hit))TreeView_SelectItem(hTreeCollections_,item);
    const auto data=SelectedTreeData(hTreeCollections_);HMENU menu=CreatePopupMenu();
    AppendMenuW(menu,MF_STRING,2600,L"New collection");AppendMenuW(menu,MF_STRING,2611,L"Import collection / Postman / OpenAPI...");
    if(data){AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,2601,L"New request here");AppendMenuW(menu,MF_STRING,2602,L"New folder here");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,2603,L"Rename...");
        AppendMenuW(menu,MF_STRING,2604,L"Description...");if(data<0){AppendMenuW(menu,MF_STRING,2605,L"Variables...");AppendMenuW(menu,MF_STRING,2606,L"Authentication...");AppendMenuW(menu,MF_STRING,2607,L"Pre-request script...");AppendMenuW(menu,MF_STRING,2608,L"Post-response script...");AppendMenuW(menu,MF_STRING,2612,L"Run this collection / folder");}
        AppendMenuW(menu,MF_STRING,2613,L"Edit complete JSON...");AppendMenuW(menu,MF_STRING,2610,L"Export JSON...");AppendMenuW(menu,MF_STRING,2614,L"Duplicate");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,2609,L"Delete...");
    }
    const auto command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_LEFTALIGN,point.x,point.y,0,hWnd_,nullptr);DestroyMenu(menu);if(command)EditTreeNode(command,data);
}
void ApiClientView::EditTreeNode(UINT command,LPARAM data){
    if(command==2611){ImportCollectionFile();return;}
    TreeNode target;std::string requestId;
    if(data<0&&-data<=static_cast<LPARAM>(treeNodes_.size()))target=treeNodes_[static_cast<size_t>(-data-1)];
    else if(data>0&&data<=static_cast<LPARAM>(treeRequests_.size())){const auto& bound=treeRequests_[static_cast<size_t>(data-1)];target={bound.folderId.empty()?bound.collectionId:bound.folderId,bound.collectionId,bound.folderId,!bound.folderId.empty()};requestId=bound.request.id;}
    if(command==2612){RunCollectionAsync(target.collectionId,target.folderId);return;}
    auto collections=storeMgr_->GetCollections();
    if(command==2600){Collection collection;collection.id=NewUiId("col_");collection.name="New collection";collections.push_back(collection);storeMgr_->SaveCollections(collections);RefreshData();return;}
    if(collections.empty()){Collection collection;collection.id=NewUiId("col_");collection.name="My collection";collections.push_back(collection);}
    if(target.collectionId.empty())target.collectionId=collections.front().id;
    auto locate=[&](std::vector<Collection>& values)->Collection*{for(auto& value:values)if(value.id==target.collectionId)return &value;return nullptr;};
    auto* collection=locate(collections);if(!collection)throw std::runtime_error("The selected collection no longer exists.");
    auto* folder=target.folder?FindFolder(collection->folders,target.folderId):nullptr;if(target.folder&&!folder)throw std::runtime_error("The selected folder no longer exists.");
    auto* request=requestId.empty()?nullptr:FindRequest(*collection,requestId);if(!requestId.empty()&&!request)throw std::runtime_error("The selected request no longer exists.");
    if(command==2601){ApiRequest value;value.id=NewUiId("req_");value.name="New request";value.auth.type=AuthType::Inherit;(folder?folder->requests:collection->requests).push_back(value);storeMgr_->SaveCollections(collections);RefreshData();for(const auto& bound:treeRequests_)if(bound.request.id==value.id){OpenRequest(bound.request);selectedScopes_=bound.scopes;selectedAuth_=bound.inheritedAuth;selectedCollectionId_=bound.collectionId;selectedFolderId_=bound.folderId;selectedPreScripts_=bound.preScripts;selectedPostScripts_=bound.postScripts;CaptureDocument();RefreshEditorCompletions();break;}return;}
    if(command==2602){Folder value;value.id=NewUiId("folder_");value.name="New folder";value.auth.type=AuthType::Inherit;(folder?folder->folders:collection->folders).push_back(value);storeMgr_->SaveCollections(collections);RefreshData();return;}
    auto node=request?RequestJson::ToJson(*request):CollectionJson::ToJson(folder?FolderCollection(*folder):*collection);
    if(command==2610){const auto path=ChooseNativeFile(hWnd_,L"Export JSON",L"JSON files\0*.json\0All files\0*.*\0\0",true,L"collection.json");if(!path.empty())WriteNativeFileAtomic(path,node.dump(2));return;}
    if(command==2614){if(request){auto value=RequestJson::FromJson(node,true);value.name+=" copy";(folder?folder->requests:collection->requests).push_back(value);}else{auto value=CollectionJson::FromJson(node,true);value.name+=" copy";if(folder)collection->folders.push_back(CollectionFolder(value));else collections.push_back(value);}storeMgr_->SaveCollections(collections);RefreshData();return;}
    if(command==2609){if(MessageBoxW(hWnd_,L"Delete the selected item and its saved contents? Open documents remain available until closed.",L"Delete saved item",MB_YESNO|MB_ICONWARNING)!=IDYES)return;collections=storeMgr_->GetCollections();collection=locate(collections);if(!collection)return;if(request)RemoveRequest(*collection,requestId);else if(target.folder)RemoveFolder(collection->folders,target.folderId);else collections.erase(std::remove_if(collections.begin(),collections.end(),[&](const Collection& value){return value.id==target.collectionId;}),collections.end());storeMgr_->SaveCollections(collections);RefreshData();return;}
    const char* field=command==2603?"name":command==2604?"description":command==2605?"variables":command==2606?"auth":command==2607?"preRequestScript":command==2608?"postResponseScript":nullptr;
    const bool structured=field&&(command==2605||command==2606);auto text=ToWide(field?(structured?node[field].dump(2):node.value(field,std::string{})):node.dump(2));
    auto validate=[&](const std::wstring& content)->std::wstring{try{auto candidate=node;if(field)candidate[field]=structured?nlohmann::json::parse(ToUtf8(content)):nlohmann::json(ToUtf8(content));else candidate=nlohmann::json::parse(ToUtf8(content));candidate["id"]=node.at("id");if(request)RequestJson::FromJson(candidate);else CollectionJson::FromJson(candidate);if(candidate.value("name","").empty())return L"Enter a name.";return {};}catch(const std::exception&){return L"Enter a valid item with the original JSON field types and unique item IDs.";}};
    const auto language=command==2607||command==2608?EditorLanguage::JavaScript:structured||!field?EditorLanguage::Json:EditorLanguage::PlainText;
    std::vector<Variable> completionScopes=collection->variables;
    if(folder){std::function<bool(const std::vector<Folder>&,std::vector<Variable>)> visit=[&](const std::vector<Folder>& folders,std::vector<Variable> inherited){for(const auto& child:folders){auto nested=inherited;nested.insert(nested.end(),child.variables.begin(),child.variables.end());if(child.id==folder->id){completionScopes=std::move(nested);return true;}if(visit(child.folders,std::move(nested)))return true;}return false;};visit(collection->folders,completionScopes);}
    const auto scriptKeys=language==EditorLanguage::JavaScript?CompletionKeys(request?*request:ApiRequest{},completionScopes,false):std::vector<std::wstring>{};
    if(!EditNativeText(hWnd_,command==2607?L"Inherited pre-request script":command==2608?L"Inherited post-response script":L"Edit saved item",text,language,validate,scriptKeys))return;
    if(field)node[field]=structured?nlohmann::json::parse(ToUtf8(text)):nlohmann::json(ToUtf8(text));else node=nlohmann::json::parse(ToUtf8(text));
    // Modal dialogs pump completions. Apply only the chosen field to the newest store.
    collections=storeMgr_->GetCollections();collection=locate(collections);if(!collection)throw std::runtime_error("The collection was removed while editing.");folder=target.folder?FindFolder(collection->folders,target.folderId):nullptr;request=requestId.empty()?nullptr:FindRequest(*collection,requestId);if((target.folder&&!folder)||(!requestId.empty()&&!request))throw std::runtime_error("The saved item was removed while editing.");
    auto latest=request?RequestJson::ToJson(*request):CollectionJson::ToJson(folder?FolderCollection(*folder):*collection);if(field)latest[field]=node[field];else latest=node;latest["id"]=request?requestId:target.folder?target.folderId:target.collectionId;
    if(request)*request=RequestJson::FromJson(latest);else if(folder)*folder=CollectionFolder(CollectionJson::FromJson(latest));else *collection=CollectionJson::FromJson(latest);storeMgr_->SaveCollections(collections);RefreshData();
}

void ApiClientView::Layout() {
    if (width_ <= 0 || height_ <= 0) return;
    auto s = [](int v) { return Theme::Scale(v); };
    const bool compact=width_<s(1040);
    compactSections_=width_<s(800)||height_<s(500);
    const bool tight=compactSections_;
    sidebarWidth_ = tight?(std::min)(s(240),width_/4):s(compact?240:320);
    auto place = [](HWND hwnd, int x, int y, int w, int h) {
        SetWindowPos(hwnd, nullptr, x, y, (std::max)(1,w), (std::max)(1,h), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    const int rowH=s(tight?24:34),gap=s(tight?4:8);
    rcSidebarTabCol_ = {0,0,tight?sidebarWidth_:sidebarWidth_/2,tight?rowH:s(38)};
    rcSidebarTabHist_ = tight?RECT{0,rowH+gap,sidebarWidth_,2*rowH+gap}:RECT{sidebarWidth_/2,0,sidebarWidth_,s(38)};
    place(hSidebarTabs_[0],rcSidebarTabCol_.left,rcSidebarTabCol_.top,rcSidebarTabCol_.right-rcSidebarTabCol_.left,rcSidebarTabCol_.bottom-rcSidebarTabCol_.top);
    place(hSidebarTabs_[1],rcSidebarTabHist_.left,rcSidebarTabHist_.top,rcSidebarTabHist_.right-rcSidebarTabHist_.left,rcSidebarTabHist_.bottom-rcSidebarTabHist_.top);
    const int bottom = height_-(tight?3*(rowH+gap)+gap:s(compact?84:44));
    const int treeY=tight?2*rowH+3*gap:s(46);
    place(hTreeFilter_,s(8),treeY,sidebarWidth_-s(16),rowH);const int filteredTreeY=treeY+rowH+gap;
    place(hTreeCollections_,s(8),filteredTreeY,sidebarWidth_-s(16),bottom-filteredTreeY-gap);
    place(hListHistory_,s(8),treeY,sidebarWidth_-s(16),bottom-treeY-gap);
    place(hBtnAddCol_,s(8),bottom,tight?sidebarWidth_-s(16):s(110),rowH);
    place(hBtnAddReq_,tight?s(8):s(124),bottom+(tight?rowH+gap:0),tight?sidebarWidth_-s(16):s(100),rowH);
    place(hBtnRunAll_,s(compact?8:230),bottom+(tight?2*(rowH+gap):s(compact?40:0)),tight?sidebarWidth_-s(16):s(82),rowH);
    place(hBtnClearHist_,s(8),bottom,tight?sidebarWidth_-s(16):s(100),rowH);
    for(auto button : {hBtnAddCol_,hBtnAddReq_,hBtnRunAll_,hTreeCollections_,hTreeFilter_}) ShowWindow(button,sidebarTab_==0?SW_SHOW:SW_HIDE);
    for(auto button : {hBtnClearHist_,hListHistory_}) ShowWindow(button,sidebarTab_==1?SW_SHOW:SW_HIDE);
    const int mainX=sidebarWidth_+s(16), mainW=(std::max)(1,width_-mainX-s(16));
    const int documentY=s(tight?4:10);
    place(hDocumentSelector_,mainX,documentY,mainW-s(196),s(240));SendMessageW(hDocumentSelector_,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),rowH-s(6));
    place(hBtnNewDocument_,mainX+mainW-s(184),documentY,s(112),rowH);place(hBtnCloseDocument_,mainX+mainW-s(64),documentY,s(64),rowH);
    const int nameY=documentY+rowH+gap,barY=nameY+rowH+gap;
    place(hEditName_,mainX,nameY,mainW-s(152),rowH); place(hBtnSave_,mainX+mainW-s(136),nameY,s(136),rowH);
    place(hComboMethod_,mainX,barY,s(104),s(240)); SendMessageW(hComboMethod_,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),rowH-s(6));
    const int urlW=mainW-s(302);
    place(hEditUrl_,mainX+s(114),barY,urlW,rowH);
    place(hBtnSend_,mainX+s(124)+urlW,barY,s(90),rowH); place(hBtnCode_,mainX+mainW-s(78),barY,s(78),rowH);
    const int tabY=barY+rowH+s(tight?4:12),tabHeight=rowH,tabWidth=s(88);
    RECT* requestRects[]={&rcReqTabParams_,&rcReqTabHeaders_,&rcReqTabAuth_,&rcReqTabBody_,&rcReqTabTests_};
    int tabX=mainX,tabRowY=tabY;
    const wchar_t* names[]={L"Params",L"Headers",L"Auth",L"Body",L"Tests",L"Variables",L"Options",L"Pre-request",L"Post-response"};
    for(int i=0;i<9;++i){ShowWindow(hRequestTabs_[i],!tight||i==requestTab_?SW_SHOW:SW_HIDE);SetWindowTextW(hRequestTabs_[i],tight?(std::wstring(names[i])+L" section \x25BE").c_str():names[i]);
        if(tight){place(hRequestTabs_[i],mainX,tabY,(std::min)(mainW,s(180)),tabHeight);if(i<5)*requestRects[i]={};}
        else {int tw=s(i==5?104:i==6?96:i>=7?136:88);if(tabX>mainX&&tabX+tw>mainX+mainW){tabX=mainX;tabRowY+=tabHeight+s(4);}place(hRequestTabs_[i],tabX,tabRowY,tw,tabHeight);if(i<5)*requestRects[i]={tabX,tabRowY,tabX+tw,tabRowY+tabHeight};tabX+=tw;}}
    const int contentY=tabRowY+tabHeight+gap;
    const int responseHeading=s(tight?24:30);
    const int requestExtra=requestTab_==3?rowH+gap:0;
    const int halfH=(std::max)(1,(height_-contentY-responseHeading-tabHeight-3*gap+requestExtra)/2);
    place(hListParams_,mainX,contentY,mainW,halfH-s(44)); place(hListHeaders_,mainX,contentY,mainW,halfH-s(44));
    place(hBtnEditConfig_,mainX,contentY+halfH-rowH,s(210),rowH);
    place(hComboAuthType_,mainX,contentY,s(180),s(240)); SendMessageW(hComboAuthType_,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),rowH-s(6));
    place(hComboAuthLocation_,mainX+s(188),contentY,(std::min)(s(220),mainW-s(188)),s(240));SendMessageW(hComboAuthLocation_,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),rowH-s(6));
    const int fieldsY=contentY+rowH+gap;
    place(hEditAuthToken_,mainX,fieldsY,mainW,rowH);
    place(hEditAuthUser_,mainX,fieldsY,mainW/2-s(8),rowH); place(hEditAuthPass_,mainX+mainW/2,fieldsY,mainW/2,rowH);
    place(hEditAuthKey_,mainX,fieldsY,mainW/2-s(8),rowH); place(hEditAuthVal_,mainX+mainW/2,fieldsY,mainW/2,rowH);
    place(hComboBodyType_,mainX,contentY,(std::min)(mainW,s(240)),s(240)); SendMessageW(hComboBodyType_,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),rowH-s(6));
    place(hBtnAttachFile_,mainX+s(248),contentY,s(130),rowH);
    place(hEditBody_,mainX,fieldsY,mainW,halfH-rowH-gap);
    place(hListTests_,mainX,contentY,mainW,halfH-s(44));
    place(hBtnAddTest_,mainX+s(224),contentY+halfH-rowH,s(130),rowH);
    place(hEditVariables_,mainX,contentY,mainW,halfH); place(hEditOptions_,mainX,contentY,mainW,halfH);
    place(hEditPreScript_,mainX,contentY,mainW,halfH);place(hEditPostScript_,mainX,contentY,mainW,halfH);
    const int responseY=contentY+halfH+gap, responseTabY=responseY+responseHeading;
    RECT* responseRects[]={&rcRespTabBody_,&rcRespTabHeaders_,&rcRespTabTests_};
    int responseX=mainX;
    compactResponses_=mainW<s(510);
    const wchar_t* responseNames[]={L"Body",L"Headers",L"Test results",L"Console"};
    for(int i=0;i<4;++i){ShowWindow(hResponseTabs_[i],!compactResponses_||i==responseTab_?SW_SHOW:SW_HIDE);const int tw=compactResponses_?(std::min)(mainW-s(102),s(180)):i==2?s(136):i==3?s(100):tabWidth;if(i<3)*responseRects[i]=compactResponses_?RECT{}:RECT{responseX,responseTabY,responseX+tw,responseTabY+tabHeight};place(hResponseTabs_[i],compactResponses_?mainX:responseX,responseTabY,tw,tabHeight);SetWindowTextW(hResponseTabs_[i],compactResponses_?(std::wstring(responseNames[i])+L" \x25BE").c_str():responseNames[i]);responseX+=tw;}
    place(hBtnSaveResponse_,mainX+mainW-s(90),responseTabY,s(90),tabHeight);
    const int responseContent=responseTabY+tabHeight+gap,responseH=height_-responseContent-gap;
    place(hEditRespBody_,mainX,responseContent,mainW,responseH); place(hListRespHeaders_,mainX,responseContent,mainW,responseH); place(hListRespTests_,mainX,responseContent,mainW,responseH);
    place(hEditScriptConsole_,mainX,responseContent,mainW,responseH);
    UpdateRequestTabsVisibility(); UpdateResponseTabsVisibility();
    for(int i=0;i<2;++i){RemovePropW(hSidebarTabs_[i],L"DataForge.Active");if(i==sidebarTab_)SetPropW(hSidebarTabs_[i],L"DataForge.Active",reinterpret_cast<HANDLE>(1));InvalidateRect(hSidebarTabs_[i],nullptr,FALSE);}
}

void ApiClientView::UpdateRequestTabsVisibility() {
    for(int i=0;i<9;++i){RemovePropW(hRequestTabs_[i],L"DataForge.Active");if(i==requestTab_)SetPropW(hRequestTabs_[i],L"DataForge.Active",reinterpret_cast<HANDLE>(1));InvalidateRect(hRequestTabs_[i],nullptr,FALSE);}
    ShowWindow(hListParams_, requestTab_ == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(hListHeaders_, requestTab_ == 1 ? SW_SHOW : SW_HIDE);

    bool isAuth = (requestTab_ == 2);
    ShowWindow(hComboAuthType_, isAuth ? SW_SHOW : SW_HIDE);
    int authSel = static_cast<int>(SendMessage(hComboAuthType_, CB_GETCURSEL, 0, 0));
    ShowWindow(hEditAuthToken_, (isAuth && authSel == 1) ? SW_SHOW : SW_HIDE);
    ShowWindow(hEditAuthUser_, (isAuth && authSel == 2) ? SW_SHOW : SW_HIDE);
    ShowWindow(hEditAuthPass_, (isAuth && authSel == 2) ? SW_SHOW : SW_HIDE);
    ShowWindow(hEditAuthKey_, (isAuth && authSel == 3) ? SW_SHOW : SW_HIDE);
    ShowWindow(hEditAuthVal_, (isAuth && authSel == 3) ? SW_SHOW : SW_HIDE);
    ShowWindow(hComboAuthLocation_,(isAuth&&authSel==3)?SW_SHOW:SW_HIDE);

    bool isBody = (requestTab_ == 3);
    ShowWindow(hComboBodyType_, isBody ? SW_SHOW : SW_HIDE);
    ShowWindow(hEditBody_, isBody ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnAttachFile_,isBody&&SendMessageW(hComboBodyType_,CB_GETCURSEL,0,0)==4?SW_SHOW:SW_HIDE);

    bool isTests = (requestTab_ == 4);
    ShowWindow(hListTests_, isTests ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnAddTest_, isTests ? SW_SHOW : SW_HIDE);
    ShowWindow(hEditVariables_, requestTab_ == 5 ? SW_SHOW : SW_HIDE);
    ShowWindow(hEditOptions_, requestTab_ == 6 ? SW_SHOW : SW_HIDE);
    ShowWindow(hEditPreScript_,requestTab_==7?SW_SHOW:SW_HIDE);ShowWindow(hEditPostScript_,requestTab_==8?SW_SHOW:SW_HIDE);
    ShowWindow(hBtnEditConfig_, requestTab_ == 0 || requestTab_ == 1 || isTests ? SW_SHOW : SW_HIDE);
    SetWindowTextW(hBtnEditConfig_, requestTab_ == 0 ? L"Edit parameters" : requestTab_ == 1 ? L"Edit headers" : L"Edit assertions");
}

void ApiClientView::UpdateResponseTabsVisibility() {
    for(int i=0;i<4;++i){RemovePropW(hResponseTabs_[i],L"DataForge.Active");if(i==responseTab_)SetPropW(hResponseTabs_[i],L"DataForge.Active",reinterpret_cast<HANDLE>(1));InvalidateRect(hResponseTabs_[i],nullptr,FALSE);}
    ShowWindow(hEditRespBody_, responseTab_ == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(hListRespHeaders_, responseTab_ == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(hListRespTests_, responseTab_ == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(hEditScriptConsole_,responseTab_==3?SW_SHOW:SW_HIDE);
}

void ApiClientView::LoadRequest(const ApiRequest& req) {
    if(req.id.empty() || req.id!=currentRequest_.id){selectedScopes_.clear();selectedAuth_={};selectedCollectionId_.clear();selectedFolderId_.clear();selectedPreScripts_.clear();selectedPostScripts_.clear();}
    currentRequest_ = req;
    SetWindowTextW(hEditName_, ToWide(req.name).c_str());
    nlohmann::json variables = nlohmann::json::array();
    for(const auto& v:req.variables) variables.push_back({{"key",v.key},{"value",v.scriptJsonValue.empty()?nlohmann::json(v.value):nlohmann::json::parse(v.scriptJsonValue)},{"enabled",v.enabled}});
    CodeEditor::SetText(hEditVariables_, ToWide(variables.dump(2)));
    CodeEditor::SetText(hEditOptions_, ToWide(nlohmann::json{{"timeout",req.options.timeoutSec},{"follow_redirects",req.options.followRedirects},{"verify_tls",req.options.verifyTls}}.dump(2)));
    CodeEditor::SetText(hEditPreScript_,ToWide(req.preRequestScript));CodeEditor::SetText(hEditPostScript_,ToWide(req.postResponseScript));

    int methodIdx = static_cast<int>(req.method);
    SendMessage(hComboMethod_, CB_SETCURSEL, methodIdx, 0);

    SetWindowTextW(hEditUrl_, ToWide(req.url).c_str());

    // Query and path parameters are both editable in the same section.
    ListView_DeleteAllItems(hListParams_);
    auto displayedParams=req.params;
    displayedParams.insert(displayedParams.end(),req.pathParams.begin(),req.pathParams.end());
    for (int i = 0; i < static_cast<int>(displayedParams.size()); ++i) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        std::wstring k = (i>=static_cast<int>(req.params.size())?L"[path] ":L"")+ToWide(displayedParams[i].key);
        item.pszText = const_cast<LPWSTR>(k.c_str());
        ListView_InsertItem(hListParams_, &item);
        std::wstring v = ToWide(displayedParams[i].value);
        ListView_SetItemText(hListParams_, i, 1, const_cast<LPWSTR>(v.c_str()));
    }

    // Headers
    ListView_DeleteAllItems(hListHeaders_);
    for (int i = 0; i < static_cast<int>(req.headers.size()); ++i) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        std::wstring k = ToWide(req.headers[i].key);
        item.pszText = const_cast<LPWSTR>(k.c_str());
        ListView_InsertItem(hListHeaders_, &item);
        std::wstring v = ToWide(req.headers[i].value);
        ListView_SetItemText(hListHeaders_, i, 1, const_cast<LPWSTR>(v.c_str()));
    }

    // Auth
    int authIdx = static_cast<int>(req.auth.type);
    SendMessage(hComboAuthType_, CB_SETCURSEL, authIdx, 0);
    SetWindowTextW(hEditAuthToken_, ToWide(req.auth.token).c_str());
    SetWindowTextW(hEditAuthUser_, ToWide(req.auth.username).c_str());
    SetWindowTextW(hEditAuthPass_, ToWide(req.auth.password).c_str());
    SetWindowTextW(hEditAuthKey_, ToWide(req.auth.key).c_str());
    SetWindowTextW(hEditAuthVal_, ToWide(req.auth.value).c_str());
    SendMessageW(hComboAuthLocation_,CB_SETCURSEL,req.auth.in=="query"?1:0,0);

    // Body
    int bodyIdx = static_cast<int>(req.body.type);
    SendMessage(hComboBodyType_, CB_SETCURSEL, bodyIdx, 0);
    SetWindowTextW(hEditBody_, ToWide(req.body.content).c_str());
    if(req.body.type == BodyType::UrlEncoded || req.body.type == BodyType::FormData) {
        nlohmann::json items=nlohmann::json::array();
        for(const auto& f:req.body.formItems) items.push_back({{"key",f.key},{"value",f.value},{"enabled",f.enabled},{"type",f.type},{"filename",f.filename},{"contentType",f.contentType},{"content",f.contentBase64}});
        CodeEditor::SetText(hEditBody_,ToWide(items.dump(2)));
    }
    CodeEditor::SetLanguage(hEditBody_, req.body.type == BodyType::Raw ? EditorLanguage::PlainText : EditorLanguage::Json);

    // Tests
    ListView_DeleteAllItems(hListTests_);
    for (int i = 0; i < static_cast<int>(req.tests.size()); ++i) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        std::wstring t = ToWide(req.tests[i].type);
        item.pszText = const_cast<LPWSTR>(t.c_str());
        ListView_InsertItem(hListTests_, &item);
        std::wstring desc = ToWide(req.tests[i].name.empty() ? req.tests[i].expected : req.tests[i].name);
        ListView_SetItemText(hListTests_, i, 1, const_cast<LPWSTR>(desc.c_str()));
    }

    UpdateRequestTabsVisibility();
    ApplyTheme();
    InvalidateRect(hWnd_, nullptr, TRUE);
    if(!activatingDocument_){CaptureDocument();RefreshDocumentSelector();}
}

ApiRequest ApiClientView::GetCurrentRequest() {
    ApiRequest req = currentRequest_;
    auto read = [](HWND hwnd) {int count=GetWindowTextLengthW(hwnd);std::wstring text(static_cast<size_t>(count)+1,L'\0');GetWindowTextW(hwnd,text.data(),count+1);text.resize(count);return ToUtf8(text);};
    req.name=read(hEditName_);
    req.preRequestScript=ToUtf8(CodeEditor::GetText(hEditPreScript_));req.postResponseScript=ToUtf8(CodeEditor::GetText(hEditPostScript_));

    int curMethod = static_cast<int>(SendMessage(hComboMethod_, CB_GETCURSEL, 0, 0));
    req.method = static_cast<HttpMethod>(curMethod);

    req.url = read(hEditUrl_);

    int authIdx = static_cast<int>(SendMessage(hComboAuthType_, CB_GETCURSEL, 0, 0));
    req.auth.type = static_cast<AuthType>(authIdx);

    req.auth.token=read(hEditAuthToken_);req.auth.username=read(hEditAuthUser_);req.auth.password=read(hEditAuthPass_);req.auth.key=read(hEditAuthKey_);req.auth.value=read(hEditAuthVal_);
    req.auth.in=SendMessageW(hComboAuthLocation_,CB_GETCURSEL,0,0)==1?"query":"header";

    int bodyIdx = static_cast<int>(SendMessage(hComboBodyType_, CB_GETCURSEL, 0, 0));
    req.body.type = static_cast<BodyType>(bodyIdx);

    req.body.content=ToUtf8(CodeEditor::GetText(hEditBody_));
    if(req.body.type==BodyType::Json && !req.body.content.empty() && req.body.content.find("{{")==std::string::npos) {
        if(nlohmann::json::parse(req.body.content,nullptr,false).is_discarded()) throw std::runtime_error("Request body must contain valid JSON.");
    }
    if(req.body.type==BodyType::UrlEncoded || req.body.type==BodyType::FormData) {
        auto form=nlohmann::json::parse(req.body.content.empty()?"[]":req.body.content);
        if(!form.is_array())throw std::runtime_error("Form body must be a JSON array of key/value objects.");
        req.body.formItems.clear();
        for(const auto& j:form){ FormItem f;f.key=j.value("key","");f.value=j.value("value","");f.enabled=j.value("enabled",true);f.type=j.value("type","text");f.filename=j.value("filename","");f.contentType=j.value("contentType","application/octet-stream");f.contentBase64=j.value("content",j.value("contentBase64",""));req.body.formItems.push_back(f); }
        req.body.content.clear();
    }
    auto variables=nlohmann::json::parse(ToUtf8(CodeEditor::GetText(hEditVariables_)));
    if(!variables.is_array())throw std::runtime_error("Variables must be a JSON array of key/value objects.");
    req.variables.clear();for(const auto& j:variables){KeyValuePair value;value.key=j.value("key","");const auto raw=j.value("value",nlohmann::json(""));value.value=raw.is_string()?raw.get<std::string>():raw.dump();value.scriptJsonValue=raw.is_string()?std::string{}:raw.dump();value.enabled=j.value("enabled",true);req.variables.push_back(std::move(value));}
    auto options=nlohmann::json::parse(ToUtf8(CodeEditor::GetText(hEditOptions_)));
    if(!options.is_object())throw std::runtime_error("Options must be a JSON object.");
    req.options.timeoutSec=options.value("timeout",options.value("timeoutSec",20.0));req.options.followRedirects=options.value("follow_redirects",options.value("followRedirects",true));req.options.verifyTls=options.value("verify_tls",options.value("verifyTls",true));
    if(req.options.timeoutSec<=0 || req.options.timeoutSec>300)throw std::runtime_error("Request timeout must be between 0 and 300 seconds.");

    return req;
}

void ApiClientView::ApplyTheme() {
    Theme::ApplyToWindow(hWnd_);
    if(hRunner_&&IsWindow(hRunner_)){Theme::ApplyToWindow(hRunner_);RECT client{};GetClientRect(hRunner_,&client);SendMessageW(hRunner_,WM_SIZE,0,MAKELPARAM(client.right,client.bottom));}
    RefreshEditorCompletions();
    Layout();
}
std::vector<std::wstring> ApiClientView::CompletionKeys(const ApiRequest& request,const std::vector<Variable>& scopes,bool useDraft) const {
    std::set<std::wstring> keys;
    const auto add=[&](const std::string& key){if(!key.empty())keys.insert(ToWide(key));};
    for(const auto& [key,value]:storeMgr_->GetMergedVariables("")){(void)value;add(key);}
    for(const auto& variable:scopes)if(variable.enabled)add(variable.key);
    std::vector<std::string> local;for(const auto& variable:request.variables)if(variable.enabled)local.push_back(variable.key);
    if(useDraft&&hEditVariables_){const auto draft=nlohmann::json::parse(ToUtf8(CodeEditor::GetText(hEditVariables_)),nullptr,false);if(draft.is_array()){local.clear();for(const auto& variable:draft)if(variable.is_object()&&variable.value("enabled",nlohmann::json(true))==true&&variable.contains("key")&&variable["key"].is_string())local.push_back(variable["key"].get<std::string>());}}
    for(const auto& key:local)add(key);
    if(!request.iterationDataJson.empty()){const auto data=nlohmann::json::parse(request.iterationDataJson,nullptr,false);if(data.is_object())for(auto value=data.begin();value!=data.end();++value)add(value.key());}
    return {keys.begin(),keys.end()};
}
void ApiClientView::RefreshEditorCompletions(){
    const auto keys=CompletionKeys(currentRequest_,selectedScopes_,true);std::vector<std::wstring> body;body.reserve(keys.size());for(const auto& key:keys)body.push_back(L"{{"+key+L"}}");
    CodeEditor::SetCompletions(hEditBody_,body);CodeEditor::SetCompletions(hEditPreScript_,keys);CodeEditor::SetCompletions(hEditPostScript_,keys);
}
std::map<std::string,std::string> ApiClientView::ResolveVariables(const ApiRequest& req,const std::vector<Variable>& scopes) {
    std::map<std::string,std::string> result;
    for(const auto& v:storeMgr_->GetGlobals())if(v.enabled)result[v.key]=v.value;
    for(const auto& v:scopes)if(v.enabled)result[v.key]=v.value;
    const auto env=storeMgr_->GetEnvironments();
    for(const auto& e:env.environments)if(e.id==env.activeId)for(const auto& v:e.variables)if(v.enabled)result[v.key]=v.value;
    for(const auto& v:req.variables)if(v.enabled)result[v.key]=v.value;
    return result;
}
ScriptContext ApiClientView::MakeScriptContext(const ApiRequest& request,const std::vector<Variable>& scopes,const std::vector<ScriptEntry>& pre,const std::vector<ScriptEntry>& post) const {
    ScriptContext context;context.globals=ScriptStore::VariablesToJson(storeMgr_->GetGlobals());context.collection=ScriptStore::VariablesToJson(scopes);context.local=ScriptStore::VariablesToJson(request.variables);context.preRequestScripts=pre;context.postResponseScripts=post;
    const auto environments=storeMgr_->GetEnvironments();for(const auto& environment:environments.environments)if(environment.id==environments.activeId){context.environment=ScriptStore::VariablesToJson(environment.variables);context.environmentEnabled=true;break;}
    return context;
}
static HistoryEntry MakeHistory(const ApiRequest& request,const ApiResponse& response,size_t sequence) {
    HistoryEntry history;
    history.id="hist_"+std::to_string(GetTickCount64())+"_"+std::to_string(sequence);
    history.method=response.method;history.url=response.url;history.statusCode=response.statusCode;
    history.statusText=response.statusText;history.latencyMs=response.latencyMs;history.sizeBytes=response.sizeBytes;
    history.executedAt=response.executedAt;history.requestSnapshot=request;return history;
}
void ApiClientView::RequestCancel(){for(auto& job:jobs_)job->task->cancelled.store(true);}
void ApiClientView::UpdateExecutionButtons(){
    bool active=false,runner=false;
    for(const auto& job:jobs_){runner=runner||job->task->runner;active=active||(!job->task->runner&&!documents_.empty()&&job->task->documentIdentity==documents_[activeDocument_].identity);}
    isExecuting_=!jobs_.empty();SetWindowTextW(hBtnSend_,active?L"Cancel":L"Send");SetWindowTextW(hBtnRunAll_,runner?L"Cancel run":L"Run all");EnableWindow(hBtnSend_,TRUE);EnableWindow(hBtnRunAll_,TRUE);
}
void ApiClientView::LaunchTask(const std::shared_ptr<Pending>& task,std::function<void()> work){
    if(jobs_.size()>=32)throw std::runtime_error("Wait for a request to finish before starting more background jobs.");
    auto slot=std::make_unique<WorkerJob>();slot->task=task;jobs_.push_back(std::move(slot));
    try {
        if(!SetTimer(hWnd_,72,50,nullptr))throw std::runtime_error("Windows could not monitor the background request.");
        jobs_.back()->worker=std::thread([task,work=std::move(work)]{try{work();}catch(...){task->error="The background request could not complete.";task->response.error=task->error;}task->done.store(true,std::memory_order_release);});
    }catch(...){jobs_.pop_back();if(jobs_.empty())KillTimer(hWnd_,72);UpdateExecutionButtons();throw;}
    UpdateExecutionButtons();
}
void ApiClientView::SendRequestAsync() {
    if(GetPropW(hParent_,L"DataForge.Closing"))return;
    for(auto& job:jobs_)if(!job->task->runner&&job->task->documentIdentity==documents_[activeDocument_].identity){job->task->cancelled.store(true);return;}
    ApiRequest request=GetCurrentRequest();const ApiRequest snapshot=request;
    const bool keepRuntime=documents_[activeDocument_].variablesDraft==CodeEditor::GetText(hEditVariables_);
    if(!keepRuntime)documents_[activeDocument_].runtimeVariables=nlohmann::json::object();currentRequest_=snapshot;CaptureDocument();
    if(request.url.find("[REDACTED]")!=std::string::npos || request.url.find("[redacted]")!=std::string::npos)
        throw std::runtime_error("This history entry has no saved request context. Re-enter the original URL and request values before sending.");
    if(request.auth.type==AuthType::Inherit)request.auth=selectedAuth_;
    auto context=MakeScriptContext(request,selectedScopes_,selectedPreScripts_,selectedPostScripts_);context.collectionEnabled=!selectedCollectionId_.empty();
    if(keepRuntime)context.local.update(documents_[activeDocument_].runtimeVariables);
    const auto collectionId=selectedCollectionId_,folderId=selectedFolderId_,environmentId=storeMgr_->GetEnvironments().activeId;
    const auto task=std::make_shared<Pending>();task->documentIdentity=documents_[activeDocument_].identity;
    auto* engine=httpEngine_;
    LaunchTask(task,[task,engine,request,snapshot,context,collectionId,folderId,environmentId]{
        try {
            auto execution=ScriptRuntime::Execute(*engine,request,context,&task->cancelled);task->response=std::move(execution.response);
            task->scopeChanges.push_back({std::move(execution.changes),std::move(execution.local),collectionId,folderId,environmentId});
            task->history.push_back(MakeHistory(snapshot,task->response,0));
        } catch(const std::exception& error){task->response.error=error.what();}catch(...) {task->response.error="The request failed unexpectedly. Check request settings and try again.";}
    });
}
void ApiClientView::RunCollectionAsync(const std::string& collectionId,const std::string& folderId) {
    if(GetPropW(hParent_,L"DataForge.Closing"))return;
    for(auto& job:jobs_)if(job->task->runner){job->task->cancelled.store(true);return;}
    struct Run {ApiRequest request,snapshot;ScriptContext context;std::string collectionId,folderId;std::vector<std::string> chain;};
    std::vector<Run> requests;
    std::map<std::string,nlohmann::json> ownedScopes;
    auto add=[&](ApiRequest req,const std::vector<Variable>& scopes,const AuthConfig& auth,const std::string& cid,const std::string& fid,const std::vector<std::string>& chain,const std::vector<ScriptEntry>& pre,const std::vector<ScriptEntry>& post){
        if(!folderId.empty()&&std::find(chain.begin(),chain.end(),folderId)==chain.end())return;const auto snapshot=req;if(req.auth.type==AuthType::Inherit)req.auth=auth;
        auto context=MakeScriptContext(req,scopes,pre,post);context.collectionEnabled=true;requests.push_back({req,snapshot,std::move(context),cid,fid,chain});
    };
    std::function<void(const Folder&,std::vector<Variable>,AuthConfig,const std::string&,std::vector<std::string>,std::vector<ScriptEntry>,std::vector<ScriptEntry>)> folder;
    folder=[&](const Folder& f,std::vector<Variable> scopes,AuthConfig auth,const std::string& cid,std::vector<std::string> chain,std::vector<ScriptEntry> pre,std::vector<ScriptEntry> post){
        ownedScopes[f.id]=ScriptStore::VariablesToJson(f.variables);chain.push_back(f.id);scopes.insert(scopes.end(),f.variables.begin(),f.variables.end());if(f.auth.type!=AuthType::Inherit)auth=f.auth;
        if(!f.preRequestScript.empty())pre.push_back({f.name,f.preRequestScript});if(!f.postResponseScript.empty())post.push_back({f.name,f.postResponseScript});
        for(const auto& req:f.requests)add(req,scopes,auth,cid,f.id,chain,pre,post);for(const auto& child:f.folders)folder(child,scopes,auth,cid,chain,pre,post);
    };
    for(const auto& col:storeMgr_->GetCollections()) {
        if(!collectionId.empty()&&collectionId!=col.id)continue;ownedScopes[col.id]=ScriptStore::VariablesToJson(col.variables);std::vector<ScriptEntry> pre,post;
        if(!col.preRequestScript.empty())pre.push_back({col.name,col.preRequestScript});if(!col.postResponseScript.empty())post.push_back({col.name,col.postResponseScript});
        for(const auto& req:col.requests)add(req,col.variables,col.auth,col.id,"",{col.id},pre,post);for(const auto& f:col.folders)folder(f,col.variables,col.auth,col.id,{col.id},pre,post);
    }
    if(requests.empty()){MessageBoxW(hWnd_,L"Add requests to a collection before running it.",L"Collection runner",MB_OK);return;}
    const auto task=std::make_shared<Pending>();task->runner=true;task->total=static_cast<int>(requests.size());
    auto* engine=httpEngine_;const auto environmentId=storeMgr_->GetEnvironments().activeId;
    ShowRunner(task);
    LaunchTask(task,[task,engine,requests,ownedScopes,environmentId]() mutable {
        try {
            auto globals=requests.front().context.globals,environment=requests.front().context.environment;int iteration=0;
            auto patch=[](nlohmann::json& scope,const nlohmann::json& delta){for(const auto& key:delta.value("unset",nlohmann::json::array()))scope.erase(key.get<std::string>());const auto added=delta.value("set",nlohmann::json::object());scope.update(added);};
            for(const auto& run:requests) {
                if(task->cancelled.load())break;
                auto context=run.context;context.globals=globals;context.environment=environment;context.iteration=iteration++;context.collection=nlohmann::json::object();for(const auto& owner:run.chain)context.collection.update(ownedScopes[owner]);
                ApiResponse response;
                try {auto result=ScriptRuntime::Execute(*engine,run.request,context,&task->cancelled);response=std::move(result.response);
                    patch(globals,result.changes.value("globals",nlohmann::json::object()));patch(environment,result.changes.value("environment",nlohmann::json::object()));
                    const auto delta=result.changes.value("collection",nlohmann::json::object()),added=delta.value("set",nlohmann::json::object());std::vector<std::string> keys;for(auto item=added.begin();item!=added.end();++item)keys.push_back(item.key());for(const auto& key:delta.value("unset",nlohmann::json::array()))keys.push_back(key.get<std::string>());
                    for(const auto& key:keys){std::string owner=run.chain.front();for(auto item=run.chain.rbegin();item!=run.chain.rend();++item)if(ownedScopes[*item].contains(key)){owner=*item;break;}if(added.contains(key))ownedScopes[owner][key]=added[key];else ownedScopes[owner].erase(key);}
                    task->scopeChanges.push_back({std::move(result.changes),std::move(result.local),run.collectionId,run.folderId,environmentId});
                }catch(const std::exception& error){response.error=error.what();response.method=HttpMethodToString(run.request.method);response.url=run.request.url;}
                bool passed=response.error.empty() && response.statusCode>=200&&response.statusCode<400;
                for(const auto& assertion:response.testResults)if(!assertion.passed)passed=false;
                if(passed)++task->passed;
                task->history.push_back(MakeHistory(run.snapshot,response,task->history.size()));
                {std::lock_guard<std::mutex> lock(task->rowsMutex);Pending::RunRow row;row.name=run.request.name;row.method=HttpMethodToString(run.request.method);row.url=response.url;row.error=response.error;row.status=response.statusCode;row.latency=response.latencyMs;row.passed=passed;row.tests=response.testResults;row.console=response.scriptConsole;task->rows.push_back(std::move(row));}
                task->completed.fetch_add(1);
            }
        } catch(...) {task->error="The collection could not complete. Check its requests and settings.";}
    });
}
void ApiClientView::ShowRunner(const std::shared_ptr<Pending>& task){
    if(hRunner_&&IsWindow(hRunner_))DestroyWindow(hRunner_);runnerTask_=task;
    WNDCLASSW window{};window.lpfnWndProc=RunnerProc;window.hInstance=GetModuleHandleW(nullptr);window.hCursor=LoadCursorW(nullptr,IDC_ARROW);window.lpszClassName=L"DataForgeNativeCollectionRunner";RegisterClassW(&window);
    MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(hWnd_,MONITOR_DEFAULTTONEAREST),&monitor);const int width=(std::min<int>)(Theme::Scale(820),monitor.rcWork.right-monitor.rcWork.left),height=(std::min<int>)(Theme::Scale(480),monitor.rcWork.bottom-monitor.rcWork.top);
    hRunner_=CreateWindowExW(WS_EX_CONTROLPARENT,window.lpszClassName,L"Collection runner",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,monitor.rcWork.left+(monitor.rcWork.right-monitor.rcWork.left-width)/2,monitor.rcWork.top+(monitor.rcWork.bottom-monitor.rcWork.top-height)/2,width,height,hParent_,nullptr,window.hInstance,this);
    if(!hRunner_)throw std::runtime_error("Windows could not open runner results.");
    hRunnerStatus_=CreateWindowExW(0,L"STATIC",L"Starting collection...",WS_CHILD|WS_VISIBLE,0,0,1,1,hRunner_,reinterpret_cast<HMENU>(1),window.hInstance,nullptr);
    hRunnerList_=CreateWindowExW(WS_EX_CLIENTEDGE,WC_LISTVIEWW,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|LVS_REPORT|LVS_SINGLESEL,0,0,1,1,hRunner_,reinterpret_cast<HMENU>(5),window.hInstance,nullptr);ListView_SetExtendedListViewStyle(hRunnerList_,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
    const wchar_t* names[]={L"Result",L"Request",L"Status",L"Time",L"Details"};const int widths[]={70,200,65,80,340};for(int i=0;i<5;++i){LVCOLUMNW column{};column.mask=LVCF_TEXT|LVCF_WIDTH;column.pszText=const_cast<wchar_t*>(names[i]);column.cx=Theme::Scale(widths[i]);ListView_InsertColumn(hRunnerList_,i,&column);}
    for(const auto& item:std::vector<std::pair<int,const wchar_t*>>{{2,L"Cancel run"},{3,L"Export results"},{4,L"Close"}})CreateWindowExW(0,L"BUTTON",item.second,WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,1,1,hRunner_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(item.first)),window.hInstance,nullptr);
    Theme::ApplyToWindow(hRunner_);RECT client{};GetClientRect(hRunner_,&client);SendMessageW(hRunner_,WM_SIZE,0,MAKELPARAM(client.right,client.bottom));ShowWindow(hRunner_,SW_SHOW);UpdateRunner();
}
void ApiClientView::UpdateRunner(){
    if(!hRunner_||!runnerTask_)return;const auto task=runnerTask_;const bool done=task->done.load(std::memory_order_acquire);std::lock_guard<std::mutex> lock(task->rowsMutex);int passed=0;for(const auto& row:task->rows)if(row.passed)++passed;
    std::wstring status=(done?(task->cancelled.load()?L"Cancelled: ":L"Finished: "):L"Running: ")+std::to_wstring(task->completed.load())+L" / "+std::to_wstring(task->total)+L" requests; "+std::to_wstring(passed)+L" passed. Double-click a result for assertions and console.";if(done&&!task->error.empty())status+=L" "+ToWide(task->error);SetWindowTextW(hRunnerStatus_,status.c_str());EnableWindow(GetDlgItem(hRunner_,2),!done);
    for(size_t i=static_cast<size_t>(ListView_GetItemCount(hRunnerList_));i<task->rows.size();++i){const auto& row=task->rows[i];std::wstring result=row.passed?L"PASS":L"FAIL",name=ToWide(row.method+" "+row.name),code=std::to_wstring(row.status),latency=std::to_wstring(static_cast<int>(row.latency))+L" ms",details=ToWide(row.error.empty()?row.url:row.error);LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=static_cast<int>(i);item.pszText=result.data();ListView_InsertItem(hRunnerList_,&item);ListView_SetItemText(hRunnerList_,item.iItem,1,name.data());ListView_SetItemText(hRunnerList_,item.iItem,2,code.data());ListView_SetItemText(hRunnerList_,item.iItem,3,latency.data());ListView_SetItemText(hRunnerList_,item.iItem,4,details.data());}
}
LRESULT CALLBACK ApiClientView::RunnerProc(HWND window,UINT message,WPARAM wp,LPARAM lp){
    auto* view=reinterpret_cast<ApiClientView*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(message==WM_NCCREATE){view=static_cast<ApiClientView*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(view));}if(!view)return DefWindowProcW(window,message,wp,lp);
    try{
    if(message==WM_GETMINMAXINFO){MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor);auto* limits=reinterpret_cast<MINMAXINFO*>(lp);const UINT dpi=GetDpiForWindow(window);limits->ptMinTrackSize={ (std::min<LONG>)(MulDiv(450,dpi?dpi:96,96),monitor.rcWork.right-monitor.rcWork.left),(std::min<LONG>)(MulDiv(320,dpi?dpi:96,96),monitor.rcWork.bottom-monitor.rcWork.top)};return 0;}
    if(message==WM_SIZE&&view->hRunnerList_){const int width=LOWORD(lp),height=HIWORD(lp),gap=Theme::Scale(12),row=Theme::Scale(36),heading=Theme::Scale(64),available=(std::max)(3,width-4*gap),buttonWidth=available/3;SetWindowPos(view->hRunnerStatus_,nullptr,gap,gap,(std::max)(1,width-2*gap),heading,SWP_NOZORDER);SetWindowPos(view->hRunnerList_,nullptr,gap,2*gap+heading,(std::max)(1,width-2*gap),(std::max)(1,height-4*gap-heading-row),SWP_NOZORDER);const bool compact=buttonWidth<Theme::Scale(135);const wchar_t* labels[]={compact?L"Cancel":L"Cancel run",compact?L"Export":L"Export results",L"Close"};for(int i=2;i<=4;++i){SetWindowTextW(GetDlgItem(window,i),labels[i-2]);SetWindowPos(GetDlgItem(window,i),nullptr,gap+(i-2)*(buttonWidth+gap),height-gap-row,buttonWidth,row,SWP_NOZORDER);}const int columns[]={70,200,65,80,340};for(int i=0;i<5;++i)ListView_SetColumnWidth(view->hRunnerList_,i,Theme::Scale(columns[i]));return 0;}
    if(message==WM_COMMAND){if(LOWORD(wp)==2&&view->runnerTask_)view->runnerTask_->cancelled.store(true);if(LOWORD(wp)==4)SendMessageW(window,WM_CLOSE,0,0);if(LOWORD(wp)==3&&view->runnerTask_){nlohmann::json values=nlohmann::json::array();{std::lock_guard<std::mutex> lock(view->runnerTask_->rowsMutex);for(const auto& row:view->runnerTask_->rows)values.push_back({{"name",row.name},{"method",row.method},{"url",row.url},{"status",row.status},{"latencyMs",row.latency},{"passed",row.passed},{"error",row.error}});}const auto path=ChooseNativeFile(window,L"Export runner results",L"JSON files\0*.json\0All files\0*.*\0\0",true,L"runner-results.json");if(!path.empty())WriteNativeFileAtomic(path,values.dump(2));}return 0;}
    if(message==WM_NOTIFY&&reinterpret_cast<NMHDR*>(lp)->code==NM_DBLCLK&&view->runnerTask_){const int index=ListView_GetNextItem(view->hRunnerList_,-1,LVNI_SELECTED);Pending::RunRow row;{std::lock_guard<std::mutex> lock(view->runnerTask_->rowsMutex);if(index<0||static_cast<size_t>(index)>=view->runnerTask_->rows.size())return 0;row=view->runnerTask_->rows[index];}nlohmann::json details={{"name",row.name},{"method",row.method},{"status",row.status},{"url",row.url},{"error",row.error},{"tests",nlohmann::json::array()},{"console",nlohmann::json::array()}};for(const auto& test:row.tests)details["tests"].push_back({{"name",test.name},{"passed",test.passed},{"message",test.message},{"expected",test.expected},{"actual",test.actual}});for(const auto& entry:row.console)details["console"].push_back({{"level",entry.level},{"phase",entry.phase},{"source",entry.source},{"message",entry.message}});auto text=ToWide(details.dump(2));EditNativeText(window,L"Runner result",text,EditorLanguage::Json);return 0;}
    if(message==WM_CLOSE){if(view->runnerTask_&&!view->runnerTask_->done.load())view->runnerTask_->cancelled.store(true);DestroyWindow(window);return 0;}
    if(message==WM_DESTROY){view->hRunner_=view->hRunnerList_=view->hRunnerStatus_=nullptr;return 0;}
    if(message==WM_PAINT){PAINTSTRUCT paint{};auto dc=BeginPaint(window,&paint);RECT rect{};GetClientRect(window,&rect);FillRect(dc,&rect,Theme::GetBgPrimaryBrush());EndPaint(window,&paint);return 0;}
    if(message==WM_ERASEBKGND)return 1;
    }catch(const std::exception& error){MessageBoxW(window,ToWide(error.what()).c_str(),L"Collection runner",MB_ICONERROR);return 0;}
    return DefWindowProcW(window,message,wp,lp);
}
void ApiClientView::PollTask() {
    UpdateRunner();
    for(size_t index=0;index<jobs_.size();) {
        auto& job=jobs_[index];if(!job->task->done.load(std::memory_order_acquire)){++index;continue;}
        if(job->worker.joinable())job->worker.join();auto task=job->task;jobs_.erase(jobs_.begin()+index);
        try{for(const auto& history:task->history)storeMgr_->AddHistory(history);}catch(...){if(!GetPropW(hParent_,L"DataForge.Closing"))MessageBoxW(hWnd_,L"History could not be saved. Check disk space and workspace permissions.",L"History not saved",MB_ICONERROR);}
        bool variablesChanged=false;try{for(const auto& change:task->scopeChanges)variablesChanged=ScriptStore::ApplyChanges(*storeMgr_,change.changes,change.collectionId,change.folderId,change.environmentId)||variablesChanged;}catch(...){task->response.error+=" Script variable changes could not be saved.";}
        if(task->runner){UpdateRunner();}
        else for(size_t docIndex=0;docIndex<documents_.size();++docIndex)if(documents_[docIndex].identity==task->documentIdentity){documents_[docIndex].response=std::move(task->response);if(!task->scopeChanges.empty())documents_[docIndex].runtimeVariables=task->scopeChanges.back().local;if(docIndex==activeDocument_){lastResponse_=documents_[docIndex].response;DisplayResponse(lastResponse_);}break;}
        PopulateHistoryList();
        if(variablesChanged)RefreshData();
    }
    if(jobs_.empty())KillTimer(hWnd_,72);UpdateExecutionButtons();
}
void ApiClientView::SaveCurrentRequest() {
    ApiRequest request=GetCurrentRequest();
    if(request.name.empty())throw std::runtime_error("Enter a name for the request before saving.");
    auto collections=storeMgr_->GetCollections();
    bool found=false;
    auto save=[&](std::vector<ApiRequest>& requests){for(auto& existing:requests)if(!request.id.empty()&&existing.id==request.id){existing=request;found=true;}};
    std::function<void(Folder&)> saveFolder=[&](Folder& f){save(f.requests);for(auto& child:f.folders)saveFolder(child);};
    for(auto& c:collections){save(c.requests);for(auto& f:c.folders)saveFolder(f);}
    if(!found) {
        std::string parentCollection,parentFolder;
        if(!collections.empty()){
            struct Parent {std::string collection,folder;};std::vector<Parent> parents;std::vector<std::wstring> labels;int initial=0;
            std::function<void(const Folder&,const std::string&,const std::wstring&)> addFolder=[&](const Folder& folder,const std::string& cid,const std::wstring& path){const auto childPath=path+L" / "+ToWide(folder.name);parents.push_back({cid,folder.id});labels.push_back(childPath);if(cid==selectedCollectionId_&&folder.id==selectedFolderId_)initial=static_cast<int>(parents.size()-1);for(const auto& child:folder.folders)addFolder(child,cid,childPath);};
            for(const auto& collection:collections){parents.push_back({collection.id,""});labels.push_back(ToWide(collection.name));if(collection.id==selectedCollectionId_&&selectedFolderId_.empty())initial=static_cast<int>(parents.size()-1);const auto path=labels.back();for(const auto& folder:collection.folders)addFolder(folder,collection.id,path);}labels.push_back(L"+ Create a new collection");
            const int selected=ChooseNativeItem(hWnd_,L"Save request in collection / folder",labels,initial);if(selected<0)return;
            if(static_cast<size_t>(selected)<parents.size()){parentCollection=parents[selected].collection;parentFolder=parents[selected].folder;collections=storeMgr_->GetCollections();}
            else {auto name=std::wstring(L"My collection");if(!EditNativeText(hWnd_,L"Name the new collection",name,EditorLanguage::PlainText,[](const std::wstring& value){return value.empty()?L"Enter a collection name.":L"";}))return;collections=storeMgr_->GetCollections();Collection collection;collection.id=NewUiId("col_");collection.name=ToUtf8(name);parentCollection=collection.id;collections.push_back(collection);}
        }else {Collection collection;collection.id=NewUiId("col_");collection.name="My collection";parentCollection=collection.id;collections.push_back(collection);}
        auto* destination=static_cast<Collection*>(nullptr);for(auto& collection:collections)if(collection.id==parentCollection)destination=&collection;if(!destination)throw std::runtime_error("The save destination was removed.");auto* folder=parentFolder.empty()?nullptr:FindFolder(destination->folders,parentFolder);if(!parentFolder.empty()&&!folder)throw std::runtime_error("The save folder was removed.");
        request.id=NewUiId("req_");(folder?folder->requests:destination->requests).push_back(request);selectedCollectionId_=parentCollection;selectedFolderId_=parentFolder;
    }
    storeMgr_->SaveCollections(collections);currentRequest_=request;CaptureDocument();RefreshDocumentSelector();RefreshData();
}
void ApiClientView::EditConfiguration() {
    using nlohmann::json;
    json values=json::array();
    if(requestTab_==4)for(const auto& t:currentRequest_.tests)values.push_back({{"type",t.type},{"expected",t.expected},{"field",t.field},{"header",t.header},{"name",t.name},{"enabled",t.enabled}});
    else {
        for(const auto& p:requestTab_==0?currentRequest_.params:currentRequest_.headers)values.push_back({{"key",p.key},{"value",p.value},{"enabled",p.enabled},{"description",p.description},{"location",requestTab_==0?"query":"header"}});
        if(requestTab_==0)for(const auto& p:currentRequest_.pathParams)values.push_back({{"key",p.key},{"value",p.value},{"enabled",p.enabled},{"description",p.description},{"location","path"}});
    }
    auto text=ToWide(values.dump(2));
    const auto validation=[&](const std::wstring& value)->std::wstring{
        auto j=json::parse(ToUtf8(value),nullptr,false);if(!j.is_array())return L"Enter a JSON array of objects.";
        for(const auto& item:j){if(!item.is_object())return L"Each entry must be an object.";
            if(item.contains("enabled")&&!item["enabled"].is_boolean())return L"Enabled must be true or false.";
            for(const auto* key:requestTab_==4?std::vector<const char*>{"type","expected","field","header","name"}:std::vector<const char*>{"key","value","description","location"})
                if(item.contains(key)&&!item[key].is_string())return L"Keys, values, and assertion fields must be strings.";
            if(requestTab_==0&&item.contains("location")&&item["location"]!="query"&&item["location"]!="path")return L"Parameter location must be query or path.";
        }return {};
    };
    if(EditNativeText(hWnd_,requestTab_==4?L"Assertions: type, expected, field, header, name":requestTab_==0?L"Parameters: key, value, enabled, location (query/path)":L"Headers: key, value, enabled",text,EditorLanguage::Json,validation)) {
        CaptureDocument();auto req=documents_[activeDocument_].request;auto j=json::parse(ToUtf8(text));
        if(requestTab_==4){req.tests.clear();for(const auto& t:j)req.tests.push_back({t.value("type","status_2xx"),t.value("expected",""),t.value("field",""),t.value("header",""),t.value("name",""),t.value("enabled",true)});}
        else {auto& params=requestTab_==0?req.params:req.headers;params.clear();if(requestTab_==0)req.pathParams.clear();
            for(const auto& p:j){auto& destination=requestTab_==0&&p.value("location","query")=="path"?req.pathParams:params;destination.push_back({p.value("key",""),p.value("value",""),p.value("enabled",true),p.value("description","")});}}
        LoadFieldsPreservingDrafts(req);
    }
}

void ApiClientView::ReplayHistory(size_t index) {
    const auto history=storeMgr_->GetHistory();if(index>=history.size())return;
    PopulateCollectionsTree();
    const auto& snapshot=history[index].requestSnapshot;
    for(const auto& bound:treeRequests_)if(!snapshot.id.empty()&&bound.request.id==snapshot.id){
        OpenRequest(bound.request);selectedScopes_=bound.scopes;selectedAuth_=bound.inheritedAuth;selectedCollectionId_=bound.collectionId;selectedFolderId_=bound.folderId;selectedPreScripts_=bound.preScripts;selectedPostScripts_=bound.postScripts;ApplyTheme();return;
    }
    OpenRequest(snapshot);
    CodeEditor::SetText(hEditRespBody_,L"History values are redacted. Re-enter the original URL, authentication, parameters and body before sending, or select a saved request.");
}

void ApiClientView::DisplayResponse(const ApiResponse& response) {
    std::string body=response.error.empty()?response.body:response.error;
    if(body.empty()&&response.statusCode==0)body="Enter a URL and send a request to view its response.";
    const bool binary=body.find('\0')!=std::string::npos;
    if(binary)body="Binary response ("+std::to_string(response.body.size())+" bytes). Use Save body to export the complete response.";
    else if(response.isJson&&body.size()<=1024*1024){auto value=nlohmann::json::parse(body,nullptr,false);if(!value.is_discarded())body=value.dump(2);}
    constexpr size_t previewLimit=4*1024*1024;
    if(body.size()>previewLimit)body=body.substr(0,previewLimit)+"\n\n[Preview limited to 4 MiB. Save body exports the complete response.]";
    CodeEditor::SetLanguage(hEditRespBody_,response.isJson&&!binary?EditorLanguage::Json:EditorLanguage::PlainText);CodeEditor::SetText(hEditRespBody_,ToWide(body));
    EnableWindow(hBtnSaveResponse_,response.statusCode>0||!response.body.empty());
    ListView_DeleteAllItems(hListRespHeaders_);
    for(int i=0;i<static_cast<int>(response.headers.size());++i){LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=i;auto key=ToWide(response.headers[i].key),value=ToWide(response.headers[i].value);item.pszText=key.data();ListView_InsertItem(hListRespHeaders_,&item);ListView_SetItemText(hListRespHeaders_,i,1,value.data());}
    ListView_DeleteAllItems(hListRespTests_);
    for(int i=0;i<static_cast<int>(response.testResults.size());++i){const auto& test=response.testResults[i];LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=i;std::wstring status=test.passed?L"PASS":L"FAIL",name=ToWide(test.name),message=ToWide(test.message);item.pszText=status.data();ListView_InsertItem(hListRespTests_,&item);ListView_SetItemText(hListRespTests_,i,1,name.data());ListView_SetItemText(hListRespTests_,i,2,message.data());}
    std::wstring console;for(const auto& entry:response.scriptConsole)console+=L"["+ToWide(entry.level)+L"] "+ToWide(entry.phase)+L" / "+ToWide(entry.source)+L": "+ToWide(entry.message)+L"\r\n";CodeEditor::SetText(hEditScriptConsole_,console);
    InvalidateRect(hWnd_,nullptr,FALSE);
}

void ApiClientView::Draw(HDC hdc) {
    const auto& tc = Theme::Get();

    // Fill background
    RECT clientRc{ 0, 0, width_, height_ };
    FillRect(hdc, &clientRc, Theme::GetBgPrimaryBrush());

    // Sidebar background
    RECT sbRc{ 0, 0, sidebarWidth_, height_ };
    FillRect(hdc, &sbRc, Theme::GetBgSecondaryBrush());

    // Sidebar separator line
    HPEN hSepPen = CreatePen(PS_SOLID, 1, tc.borderColor);
    HGDIOBJ oldPen = SelectObject(hdc, hSepPen);
    MoveToEx(hdc, sidebarWidth_, 0, nullptr);
    LineTo(hdc, sidebarWidth_, height_);

    // Sidebar tabs
    auto drawTab = [&](const RECT& rc, const wchar_t* title, bool active) {
        COLORREF bg = active ? tc.bgPrimary : tc.bgSecondary;
        COLORREF fg = active ? tc.textPrimary : tc.textMuted;
        HBRUSH br = CreateSolidBrush(bg);
        FillRect(hdc, &rc, br);
        DeleteObject(br);

        if (active) {
            HPEN p = CreatePen(PS_SOLID, 2, tc.accent);
            HGDIOBJ op = SelectObject(hdc, p);
            MoveToEx(hdc, rc.left, rc.bottom - 1, nullptr);
            LineTo(hdc, rc.right, rc.bottom - 1);
            SelectObject(hdc, op);
            DeleteObject(p);
        }

        SelectObject(hdc, active ? Theme::GetBoldFont() : Theme::GetMainFont());
        SetTextColor(hdc, fg);
        SetBkMode(hdc, TRANSPARENT);
        RECT tr = rc;
        DrawTextW(hdc, title, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };

    drawTab(rcSidebarTabCol_, L"Collections", sidebarTab_ == 0);
    drawTab(rcSidebarTabHist_, L"History", sidebarTab_ == 1);

    // Request sub-tabs
    drawTab(rcReqTabParams_, L"Params", requestTab_ == 0);
    drawTab(rcReqTabHeaders_, L"Headers", requestTab_ == 1);
    drawTab(rcReqTabAuth_, L"Auth", requestTab_ == 2);
    drawTab(rcReqTabBody_, L"Body", requestTab_ == 3);
    drawTab(rcReqTabTests_, L"Tests", requestTab_ == 4);

    // Response header summary
    int mainX = sidebarWidth_ + Theme::Scale(16);
    RECT responseTabBounds{};GetWindowRect(hResponseTabs_[responseTab_],&responseTabBounds);MapWindowPoints(nullptr,hWnd_,reinterpret_cast<POINT*>(&responseTabBounds),2);
    int respY = responseTabBounds.top - Theme::Scale(compactSections_?24:30);
    RECT respTitleRc{ mainX, respY, width_-Theme::Scale(16), respY + Theme::Scale(24) };
    SelectObject(hdc, Theme::GetBoldFont());
    SetTextColor(hdc, tc.textPrimary);
    SetBkMode(hdc, TRANSPARENT);
    std::wstring summary=L"Response";
    if (lastResponse_.statusCode > 0 || !lastResponse_.error.empty()) {
        const std::wstring statusStr = (lastResponse_.statusCode > 0)
            ? std::to_wstring(lastResponse_.statusCode) + L" " + ToWide(lastResponse_.statusText)
            : L"Error";
        summary+=L"   "+statusStr+L"   "+std::to_wstring(static_cast<int>(lastResponse_.latencyMs))+L" ms   "+std::to_wstring(lastResponse_.sizeBytes)+L" B";
    }
    DrawTextW(hdc,summary.c_str(),-1,&respTitleRc,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);

    // Response sub-tabs
    drawTab(rcRespTabBody_, L"Body", responseTab_ == 0);
    drawTab(rcRespTabHeaders_, L"Headers", responseTab_ == 1);
    drawTab(rcRespTabTests_, L"Test Results", responseTab_ == 2);

    SelectObject(hdc, oldPen);
    DeleteObject(hSepPen);
}

LRESULT CALLBACK ApiClientView::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ApiClientView* pThis = reinterpret_cast<ApiClientView*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    try {
    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<ApiClientView*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    case WM_PRINTCLIENT: if(pThis)pThis->Draw(reinterpret_cast<HDC>(wParam));return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ oldBm = SelectObject(memDC, memBm);

        if (pThis) pThis->Draw(memDC);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBm);
        DeleteObject(memBm);
        DeleteDC(memDC);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (!pThis) break;
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        if (PtInRect(&pThis->rcSidebarTabCol_, pt)) {
            pThis->sidebarTab_ = 0;
            pThis->Layout();
            InvalidateRect(hWnd, nullptr, TRUE);
        } else if (PtInRect(&pThis->rcSidebarTabHist_, pt)) {
            pThis->sidebarTab_ = 1;
            pThis->Layout();
            InvalidateRect(hWnd, nullptr, TRUE);
        } else if (PtInRect(&pThis->rcReqTabParams_, pt)) {
            pThis->requestTab_ = 0; pThis->UpdateRequestTabsVisibility(); InvalidateRect(hWnd, nullptr, TRUE);
        } else if (PtInRect(&pThis->rcReqTabHeaders_, pt)) {
            pThis->requestTab_ = 1; pThis->UpdateRequestTabsVisibility(); InvalidateRect(hWnd, nullptr, TRUE);
        } else if (PtInRect(&pThis->rcReqTabAuth_, pt)) {
            pThis->requestTab_ = 2; pThis->UpdateRequestTabsVisibility(); InvalidateRect(hWnd, nullptr, TRUE);
        } else if (PtInRect(&pThis->rcReqTabBody_, pt)) {
            pThis->requestTab_ = 3; pThis->UpdateRequestTabsVisibility(); InvalidateRect(hWnd, nullptr, TRUE);
        } else if (PtInRect(&pThis->rcReqTabTests_, pt)) {
            pThis->requestTab_ = 4; pThis->UpdateRequestTabsVisibility(); InvalidateRect(hWnd, nullptr, TRUE);
        } else if (PtInRect(&pThis->rcRespTabBody_, pt)) {
            pThis->responseTab_ = 0; pThis->UpdateResponseTabsVisibility(); InvalidateRect(hWnd, nullptr, TRUE);
        } else if (PtInRect(&pThis->rcRespTabHeaders_, pt)) {
            pThis->responseTab_ = 1; pThis->UpdateResponseTabsVisibility(); InvalidateRect(hWnd, nullptr, TRUE);
        } else if (PtInRect(&pThis->rcRespTabTests_, pt)) {
            pThis->responseTab_ = 2; pThis->UpdateResponseTabsVisibility(); InvalidateRect(hWnd, nullptr, TRUE);
        }
        return 0;
    }
    case WM_CONTEXTMENU:if(pThis&&reinterpret_cast<HWND>(wParam)==pThis->hTreeCollections_){pThis->TreeMenu();return 0;}break;
    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        WORD code = HIWORD(wParam);
        if(!pThis)return 0;
        if(id==2003&&code==EN_CHANGE){pThis->PopulateCollectionsTree();return 0;}
        if(id==2230&&code==EN_CHANGE){pThis->RefreshEditorCompletions();return 0;}
        if(id>=2600&&id<=2614){pThis->EditTreeNode(id,SelectedTreeData(pThis->hTreeCollections_));return 0;}
        if(id>=2400 && id<2409 && code==BN_CLICKED){
            if(pThis->compactSections_ && lParam){HMENU menu=CreatePopupMenu();const wchar_t* names[]={L"Params",L"Headers",L"Auth",L"Body",L"Tests",L"Variables",L"Options",L"Pre-request",L"Post-response"};
                for(int i=0;i<9;++i)AppendMenuW(menu,MF_STRING|(i==pThis->requestTab_?MF_CHECKED:0),2500+i,names[i]);
                RECT rect{};GetWindowRect(reinterpret_cast<HWND>(lParam),&rect);const UINT selection=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_LEFTALIGN,rect.left,rect.bottom,0,hWnd,nullptr);DestroyMenu(menu);
                if(selection>=2500&&selection<2509)pThis->requestTab_=selection-2500;
            }else pThis->requestTab_=id-2400;
            pThis->Layout();InvalidateRect(hWnd,nullptr,TRUE);return 0;}
        if(((id>=2410&&id<2413)||id==2414)&&code==BN_CLICKED){
            if(pThis->compactResponses_&&lParam){HMENU menu=CreatePopupMenu();const wchar_t* names[]={L"Body",L"Headers",L"Test results",L"Console"};for(int i=0;i<4;++i)AppendMenuW(menu,MF_STRING|(i==pThis->responseTab_?MF_CHECKED:0),2510+i,names[i]);RECT rect{};GetWindowRect(reinterpret_cast<HWND>(lParam),&rect);const auto selection=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_LEFTALIGN,rect.left,rect.bottom,0,hWnd,nullptr);DestroyMenu(menu);if(selection>=2510&&selection<2514)pThis->responseTab_=selection-2510;}
            else pThis->responseTab_=id==2414?3:id-2410;pThis->Layout();InvalidateRect(hWnd,nullptr,TRUE);return 0;}
        if(id==2413&&code==BN_CLICKED){auto path=ChooseNativeFile(hWnd,L"Save complete response body",L"All files\0*.*\0\0",true,L"response.txt");if(!path.empty())WriteNativeFileAtomic(path,pThis->lastResponse_.body);return 0;}
        if(id>=2420 && id<2422 && code==BN_CLICKED){pThis->sidebarTab_=id-2420;pThis->Layout();InvalidateRect(hWnd,nullptr,TRUE);return 0;}
        if(id==2430&&code==CBN_SELCHANGE){const auto selected=SendMessageW(pThis->hDocumentSelector_,CB_GETCURSEL,0,0);if(selected>=0)pThis->ActivateDocument(static_cast<size_t>(selected));return 0;}
        if(id==2431&&code==BN_CLICKED){pThis->NewDocument();return 0;}
        if(id==2432&&code==BN_CLICKED){pThis->CloseDocument();return 0;}
        if(id==2110 && code==BN_CLICKED){pThis->SaveCurrentRequest();return 0;}
        if(id==2222 && code==BN_CLICKED){pThis->EditConfiguration();return 0;}
        if(id==2221 && code==BN_CLICKED){pThis->CaptureDocument();auto req=pThis->documents_[pThis->activeDocument_].request;req.tests.push_back({"status_2xx","","","","Success status",true});pThis->LoadFieldsPreservingDrafts(req);return 0;}
        if(id==2210 && code==CBN_SELCHANGE){const int type=static_cast<int>(SendMessageW(pThis->hComboBodyType_,CB_GETCURSEL,0,0));CodeEditor::SetLanguage(pThis->hEditBody_,type==2?EditorLanguage::PlainText:EditorLanguage::Json);if((type==3||type==4)&&CodeEditor::GetText(pThis->hEditBody_).empty())CodeEditor::SetText(pThis->hEditBody_,L"[]");pThis->UpdateRequestTabsVisibility();return 0;}
        if(id==2212&&code==BN_CLICKED){auto path=ChooseNativeFile(hWnd,L"Attach a multipart file",L"All files\0*.*\0\0",false);if(path.empty())return 0;
            const auto size=std::filesystem::file_size(path);if(size>8*1024*1024)throw std::runtime_error("Multipart file selection is limited to 8 MiB per file.");
            std::ifstream input(std::filesystem::path(path),std::ios::binary);if(!input)throw std::runtime_error("The selected file could not be opened.");std::string bytes((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());if(input.bad())throw std::runtime_error("The selected file could not be read completely.");
            DWORD length=0;CryptBinaryToStringA(reinterpret_cast<const BYTE*>(bytes.data()),static_cast<DWORD>(bytes.size()),CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,nullptr,&length);std::string encoded(length,'\0');if(!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(bytes.data()),static_cast<DWORD>(bytes.size()),CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,encoded.data(),&length))throw std::runtime_error("The file could not be encoded.");encoded.resize(length&&encoded[length-1]=='\0'?length-1:length);
            auto form=nlohmann::json::parse(ToUtf8(CodeEditor::GetText(pThis->hEditBody_)),nullptr,false);if(!form.is_array())throw std::runtime_error("Enter a form array before attaching a file.");form.push_back({{"key","file"},{"value",""},{"enabled",true},{"type","file"},{"filename",std::filesystem::path(path).filename().u8string()},{"contentType","application/octet-stream"},{"content",encoded}});const auto text=ToWide(form.dump(2));if(text.size()>16*1024*1024)throw std::runtime_error("The complete multipart form exceeds the 16 MiB editor limit. Remove an attachment before adding another.");CodeEditor::SetText(pThis->hEditBody_,text);return 0;}

        if (id == 2103) { // Send Button
            if (pThis) pThis->SendRequestAsync();
        } else if (id == 2104) { // Code Button
            if(pThis->OnOpenCodeGen){auto request=pThis->GetCurrentRequest();if(request.auth.type==AuthType::Inherit)request.auth=pThis->selectedAuth_;request=VariableResolver::PrepareRequest(request,pThis->ResolveVariables(request,pThis->selectedScopes_));request.auth.type=AuthType::None;request.params.clear();request.pathParams.clear();request.variables.clear();pThis->OnOpenCodeGen(request);}
        } else if (id == 2010) { // Add Collection
            pThis->EditTreeNode(2600,0);
        } else if (id == 2011) { // Add Request
            pThis->EditTreeNode(2601,SelectedTreeData(pThis->hTreeCollections_));
        } else if (id == 2012) { // Run All
            if (pThis) pThis->RunCollectionAsync();
        } else if (id == 2013) { // Clear History
            if (pThis) {
                pThis->storeMgr_->ClearHistory();
                pThis->PopulateHistoryList();
            }
        } else if (id == 2203 && code == CBN_SELCHANGE) { // Auth type changed
            if (pThis) pThis->UpdateRequestTabsVisibility();
        }
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR* nm = reinterpret_cast<NMHDR*>(lParam);
        if (!pThis) return 0;
        if(nm->idFrom==2001&&nm->code==NM_RCLICK){pThis->TreeMenu();return 0;}
        if(nm->code==NM_DBLCLK&&(nm->idFrom==2201||nm->idFrom==2202||nm->idFrom==2220)){pThis->requestTab_=nm->idFrom==2201?0:nm->idFrom==2202?1:4;pThis->EditConfiguration();return 0;}
        if (nm->idFrom == 2001 && nm->code == TVN_SELCHANGEDW) {
            auto* item=reinterpret_cast<NMTREEVIEWW*>(lParam);
            const auto index=item->itemNew.lParam;
            if(index>0 && index<=static_cast<LPARAM>(pThis->treeRequests_.size())) {
                const auto bound=pThis->treeRequests_[static_cast<size_t>(index-1)];
                pThis->OpenRequest(bound.request);
                pThis->selectedScopes_=bound.scopes;pThis->selectedAuth_=bound.inheritedAuth;
                pThis->selectedCollectionId_=bound.collectionId;pThis->selectedFolderId_=bound.folderId;pThis->selectedPreScripts_=bound.preScripts;pThis->selectedPostScripts_=bound.postScripts;
                pThis->ApplyTheme();
            }
        } else if (nm->idFrom == 2002 && nm->code == NM_CLICK) { // History item selected
            int sel = ListView_GetNextItem(pThis->hListHistory_, -1, LVNI_SELECTED);
            if (sel >= 0) {
                pThis->ReplayHistory(static_cast<size_t>(sel));
            }
        }
        return 0;
    }
    case WM_APP_REQ_COMPLETED: {
        std::unique_ptr<ApiResponse> pResp(reinterpret_cast<ApiResponse*>(wParam));
        if (pThis && pResp) {
            pThis->lastResponse_ = *pResp;
            pThis->isExecuting_ = false;
            EnableWindow(pThis->hBtnSend_, TRUE);
            SetWindowTextW(pThis->hBtnSend_, L"Send");

            pThis->DisplayResponse(*pResp);

            pThis->PopulateHistoryList();
            InvalidateRect(hWnd, nullptr, TRUE);
        }
        return 0;
    }
    case WM_APP_RUNNER_COMPLETED: {
        int total = static_cast<int>(wParam);
        int passed = static_cast<int>(lParam);
        if (pThis) {
            pThis->isExecuting_ = false;
            EnableWindow(pThis->hBtnRunAll_, TRUE);
            std::wstring resultText = L"Collection runner completed: " + std::to_wstring(passed) + L"/" + std::to_wstring(total) + L" requests passed assertions.";
            MessageBoxW(hWnd, resultText.c_str(), L"Collection Runner", MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    }
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        const auto& tc = Theme::Get();
        SetTextColor(hdc, tc.textPrimary);
        SetBkColor(hdc, tc.bgSecondary);
        return reinterpret_cast<LRESULT>(Theme::GetBgSecondaryBrush());
    }
    case WM_TIMER: if(pThis && wParam==72){pThis->PollTask();return 0;}break;
    }
    }catch(const std::exception& error){
        std::wstring message=L"The operation could not complete. Check the request body, variables, options, and workspace permissions.";
        if(dynamic_cast<const nlohmann::json::exception*>(&error)==nullptr)message=ToWide(error.what());
        MessageBoxW(hWnd,message.c_str(),L"DataForge Studio",MB_OK|MB_ICONERROR);return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace native_app
