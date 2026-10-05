#include "ApiClientView.h"
#include <windowsx.h>
#include <sstream>
#include <thread>
#include <json.hpp>
#include "CodeEditor.h"
#include "NativeDialogs.h"
#include <stdexcept>

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

ApiClientView::ApiClientView(StoreManager* storeMgr, HttpEngine* httpEngine)
    : storeMgr_(storeMgr), httpEngine_(httpEngine) {}

ApiClientView::~ApiClientView() {
    RequestCancel();
    if (worker_.joinable()) worker_.join();
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
    CodeEditor::SetText(hEditVariables_, L"[]");
    CodeEditor::SetText(hEditOptions_, L"{\"timeoutSec\": 20, \"followRedirects\": true, \"verifyTls\": true}");
    auto button = [&](const wchar_t* text, int id) { return CreateWindowExW(0,L"BUTTON",text,WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,100,34,hWnd_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr); };
    const wchar_t* requestNames[] = {L"Params",L"Headers",L"Auth",L"Body",L"Tests",L"Variables",L"Options"};
    for(int i=0;i<7;++i) hRequestTabs_[i]=button(requestNames[i],2400+i);
    const wchar_t* responseNames[] = {L"Body",L"Headers",L"Test results"};
    for(int i=0;i<3;++i) hResponseTabs_[i]=button(responseNames[i],2410+i);
    hSidebarTabs_[0]=button(L"Collections",2420); hSidebarTabs_[1]=button(L"History",2421);
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
    PopulateCollectionsTree();
    PopulateHistoryList();
    ApplyTheme();
}

void ApiClientView::PopulateCollectionsTree() {
    TreeView_DeleteAllItems(hTreeCollections_); treeRequests_.clear();
    auto node=[&](HTREEITEM parent,const std::wstring& text,LPARAM data=0) {
        TVINSERTSTRUCTW item{};item.hParent=parent;item.hInsertAfter=TVI_LAST;item.item.mask=TVIF_TEXT|TVIF_PARAM;item.item.pszText=const_cast<LPWSTR>(text.c_str());item.item.lParam=data;
        return TreeView_InsertItem(hTreeCollections_,&item);
    };
    auto requests=[&](HTREEITEM parent,const std::vector<ApiRequest>& list,const std::vector<Variable>& scopes,const AuthConfig& auth) {
        for(const auto& request:list){treeRequests_.push_back({request,scopes,auth});node(parent,L"["+ToWide(HttpMethodToString(request.method))+L"] "+ToWide(request.name),static_cast<LPARAM>(treeRequests_.size()));}
    };
    std::function<void(HTREEITEM,const Folder&,std::vector<Variable>,AuthConfig)> folder;
    folder=[&](HTREEITEM parent,const Folder& f,std::vector<Variable> scopes,AuthConfig auth){
        const auto item=node(parent,ToWide(f.name));scopes.insert(scopes.end(),f.variables.begin(),f.variables.end());if(f.auth.type!=AuthType::Inherit)auth=f.auth;
        requests(item,f.requests,scopes,auth);for(const auto& child:f.folders)folder(item,child,scopes,auth);TreeView_Expand(hTreeCollections_,item,TVE_EXPAND);
    };
    for(const auto& collection:storeMgr_->GetCollections()) {
        auto item=node(TVI_ROOT,ToWide(collection.name));requests(item,collection.requests,collection.variables,collection.auth);
        for(const auto& f:collection.folders)folder(item,f,collection.variables,collection.auth);
        TreeView_Expand(hTreeCollections_,item,TVE_EXPAND);
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

void ApiClientView::Layout() {
    if (width_ <= 0 || height_ <= 0) return;
    auto s = [](int v) { return Theme::Scale(v); };
    sidebarWidth_ = s(320);
    auto place = [](HWND hwnd, int x, int y, int w, int h) {
        SetWindowPos(hwnd, nullptr, x, y, (std::max)(1,w), (std::max)(1,h), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    rcSidebarTabCol_ = {0,0,sidebarWidth_/2,s(38)};
    rcSidebarTabHist_ = {sidebarWidth_/2,0,sidebarWidth_,s(38)};
    place(hSidebarTabs_[0], 0,0,sidebarWidth_/2,s(38));
    place(hSidebarTabs_[1], sidebarWidth_/2,0,sidebarWidth_/2,s(38));
    const int bottom = height_ - s(44);
    place(hTreeCollections_,s(8),s(46),sidebarWidth_-s(16),bottom-s(54));
    place(hListHistory_,s(8),s(46),sidebarWidth_-s(16),bottom-s(54));
    place(hBtnAddCol_,s(8),bottom,s(110),s(34)); place(hBtnAddReq_,s(124),bottom,s(100),s(34));
    place(hBtnRunAll_,s(230),bottom,s(82),s(34)); place(hBtnClearHist_,s(8),bottom,s(100),s(34));
    for(auto button : {hBtnAddCol_,hBtnAddReq_,hBtnRunAll_,hTreeCollections_}) ShowWindow(button,sidebarTab_==0?SW_SHOW:SW_HIDE);
    for(auto button : {hBtnClearHist_,hListHistory_}) ShowWindow(button,sidebarTab_==1?SW_SHOW:SW_HIDE);
    const int mainX=sidebarWidth_+s(16), mainW=(std::max)(s(640),width_-mainX-s(16));
    place(hEditName_,mainX,s(10),mainW-s(152),s(34)); place(hBtnSave_,mainX+mainW-s(136),s(10),s(136),s(34));
    place(hComboMethod_,mainX,s(54),s(104),s(240)); SendMessageW(hComboMethod_,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),s(26));
    const int urlW=mainW-s(302);
    place(hEditUrl_,mainX+s(114),s(54),urlW,s(34));
    place(hBtnSend_,mainX+s(124)+urlW,s(54),s(90),s(34)); place(hBtnCode_,mainX+mainW-s(78),s(54),s(78),s(34));
    const int tabY=s(100),tabHeight=s(34),tabWidth=s(88);
    RECT* requestRects[]={&rcReqTabParams_,&rcReqTabHeaders_,&rcReqTabAuth_,&rcReqTabBody_,&rcReqTabTests_};
    int tabX=mainX;
    for(int i=0;i<7;++i){ int tw=s(i==5?104:i==6?96:88); place(hRequestTabs_[i],tabX,tabY,tw,tabHeight); if(i<5)*requestRects[i]={tabX,tabY,tabX+tw,tabY+tabHeight}; tabX+=tw; }
    const int contentY=tabY+tabHeight+s(8);
    const int halfH=(std::max)(s(100),(height_-contentY-s(102))/2);
    place(hListParams_,mainX,contentY,mainW,halfH-s(44)); place(hListHeaders_,mainX,contentY,mainW,halfH-s(44));
    place(hBtnEditConfig_,mainX,contentY+halfH-s(34),s(210),s(34));
    place(hComboAuthType_,mainX,contentY,s(180),s(240)); SendMessageW(hComboAuthType_,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),s(26));
    place(hEditAuthToken_,mainX,contentY+s(54),mainW,s(34));
    place(hEditAuthUser_,mainX,contentY+s(54),mainW/2-s(8),s(34)); place(hEditAuthPass_,mainX+mainW/2,contentY+s(54),mainW/2,s(34));
    place(hEditAuthKey_,mainX,contentY+s(54),mainW/2-s(8),s(34)); place(hEditAuthVal_,mainX+mainW/2,contentY+s(54),mainW/2,s(34));
    place(hComboBodyType_,mainX,contentY,s(240),s(240)); SendMessageW(hComboBodyType_,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),s(26));
    place(hEditBody_,mainX,contentY+s(44),mainW,halfH-s(44));
    place(hListTests_,mainX,contentY,mainW,halfH-s(44));
    place(hBtnAddTest_,mainX+s(224),contentY+halfH-s(34),s(130),s(34));
    place(hEditVariables_,mainX,contentY,mainW,halfH); place(hEditOptions_,mainX,contentY,mainW,halfH);
    const int responseY=contentY+halfH+s(12), responseTabY=responseY+s(30);
    RECT* responseRects[]={&rcRespTabBody_,&rcRespTabHeaders_,&rcRespTabTests_};
    int responseX=mainX;
    for(int i=0;i<3;++i){const int tw=i==2?s(136):tabWidth; *responseRects[i]={responseX,responseTabY,responseX+tw,responseTabY+tabHeight}; place(hResponseTabs_[i],responseX,responseTabY,tw,tabHeight); responseX+=tw;}
    const int responseContent=responseTabY+tabHeight+s(8),responseH=height_-responseContent-s(12);
    place(hEditRespBody_,mainX,responseContent,mainW,responseH); place(hListRespHeaders_,mainX,responseContent,mainW,responseH); place(hListRespTests_,mainX,responseContent,mainW,responseH);
    UpdateRequestTabsVisibility(); UpdateResponseTabsVisibility();
    for(int i=0;i<2;++i){RemovePropW(hSidebarTabs_[i],L"DataForge.Active");if(i==sidebarTab_)SetPropW(hSidebarTabs_[i],L"DataForge.Active",reinterpret_cast<HANDLE>(1));InvalidateRect(hSidebarTabs_[i],nullptr,FALSE);}
}

void ApiClientView::UpdateRequestTabsVisibility() {
    for(int i=0;i<7;++i){RemovePropW(hRequestTabs_[i],L"DataForge.Active");if(i==requestTab_)SetPropW(hRequestTabs_[i],L"DataForge.Active",reinterpret_cast<HANDLE>(1));InvalidateRect(hRequestTabs_[i],nullptr,FALSE);}
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

    bool isBody = (requestTab_ == 3);
    ShowWindow(hComboBodyType_, isBody ? SW_SHOW : SW_HIDE);
    ShowWindow(hEditBody_, isBody ? SW_SHOW : SW_HIDE);

    bool isTests = (requestTab_ == 4);
    ShowWindow(hListTests_, isTests ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnAddTest_, isTests ? SW_SHOW : SW_HIDE);
    ShowWindow(hEditVariables_, requestTab_ == 5 ? SW_SHOW : SW_HIDE);
    ShowWindow(hEditOptions_, requestTab_ == 6 ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnEditConfig_, requestTab_ == 0 || requestTab_ == 1 || isTests ? SW_SHOW : SW_HIDE);
    SetWindowTextW(hBtnEditConfig_, requestTab_ == 0 ? L"Edit parameters" : requestTab_ == 1 ? L"Edit headers" : L"Edit assertions");
}

void ApiClientView::UpdateResponseTabsVisibility() {
    for(int i=0;i<3;++i){RemovePropW(hResponseTabs_[i],L"DataForge.Active");if(i==responseTab_)SetPropW(hResponseTabs_[i],L"DataForge.Active",reinterpret_cast<HANDLE>(1));InvalidateRect(hResponseTabs_[i],nullptr,FALSE);}
    ShowWindow(hEditRespBody_, responseTab_ == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(hListRespHeaders_, responseTab_ == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(hListRespTests_, responseTab_ == 2 ? SW_SHOW : SW_HIDE);
}

void ApiClientView::LoadRequest(const ApiRequest& req) {
    if(req.id.empty() || req.id!=currentRequest_.id){selectedScopes_.clear();selectedAuth_={};}
    currentRequest_ = req;
    SetWindowTextW(hEditName_, ToWide(req.name).c_str());
    nlohmann::json variables = nlohmann::json::array();
    for(const auto& v:req.variables) variables.push_back({{"key",v.key},{"value",v.value},{"enabled",v.enabled}});
    CodeEditor::SetText(hEditVariables_, ToWide(variables.dump(2)));
    CodeEditor::SetText(hEditOptions_, ToWide(nlohmann::json{{"timeoutSec",req.options.timeoutSec},{"followRedirects",req.options.followRedirects},{"verifyTls",req.options.verifyTls}}.dump(2)));

    int methodIdx = static_cast<int>(req.method);
    SendMessage(hComboMethod_, CB_SETCURSEL, methodIdx, 0);

    SetWindowTextW(hEditUrl_, ToWide(req.url).c_str());

    // Params
    ListView_DeleteAllItems(hListParams_);
    for (int i = 0; i < static_cast<int>(req.params.size()); ++i) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        std::wstring k = ToWide(req.params[i].key);
        item.pszText = const_cast<LPWSTR>(k.c_str());
        ListView_InsertItem(hListParams_, &item);
        std::wstring v = ToWide(req.params[i].value);
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

    // Body
    int bodyIdx = static_cast<int>(req.body.type);
    SendMessage(hComboBodyType_, CB_SETCURSEL, bodyIdx, 0);
    SetWindowTextW(hEditBody_, ToWide(req.body.content).c_str());
    if(req.body.type == BodyType::UrlEncoded || req.body.type == BodyType::FormData) {
        nlohmann::json items=nlohmann::json::array();
        for(const auto& f:req.body.formItems) items.push_back({{"key",f.key},{"value",f.value},{"enabled",f.enabled},{"type",f.type},{"filename",f.filename},{"contentType",f.contentType},{"contentBase64",f.contentBase64}});
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
}

ApiRequest ApiClientView::GetCurrentRequest() {
    ApiRequest req = currentRequest_;
    auto read = [](HWND hwnd) {int count=GetWindowTextLengthW(hwnd);std::wstring text(static_cast<size_t>(count)+1,L'\0');GetWindowTextW(hwnd,text.data(),count+1);text.resize(count);return ToUtf8(text);};
    req.name=read(hEditName_);

    int curMethod = static_cast<int>(SendMessage(hComboMethod_, CB_GETCURSEL, 0, 0));
    req.method = static_cast<HttpMethod>(curMethod);

    req.url = read(hEditUrl_);

    int authIdx = static_cast<int>(SendMessage(hComboAuthType_, CB_GETCURSEL, 0, 0));
    req.auth.type = static_cast<AuthType>(authIdx);

    req.auth.token=read(hEditAuthToken_);req.auth.username=read(hEditAuthUser_);req.auth.password=read(hEditAuthPass_);req.auth.key=read(hEditAuthKey_);req.auth.value=read(hEditAuthVal_);

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
        for(const auto& j:form){ FormItem f;f.key=j.value("key","");f.value=j.value("value","");f.enabled=j.value("enabled",true);f.type=j.value("type","text");f.filename=j.value("filename","");f.contentType=j.value("contentType","application/octet-stream");f.contentBase64=j.value("contentBase64","");req.body.formItems.push_back(f); }
        req.body.content.clear();
    }
    auto variables=nlohmann::json::parse(ToUtf8(CodeEditor::GetText(hEditVariables_)));
    if(!variables.is_array())throw std::runtime_error("Variables must be a JSON array of key/value objects.");
    req.variables.clear();for(const auto& j:variables)req.variables.push_back({j.value("key",""),j.value("value",""),j.value("enabled",true),""});
    auto options=nlohmann::json::parse(ToUtf8(CodeEditor::GetText(hEditOptions_)));
    if(!options.is_object())throw std::runtime_error("Options must be a JSON object.");
    req.options.timeoutSec=options.value("timeoutSec",20.0);req.options.followRedirects=options.value("followRedirects",true);req.options.verifyTls=options.value("verifyTls",true);
    if(req.options.timeoutSec<=0 || req.options.timeoutSec>300)throw std::runtime_error("Request timeout must be between 0 and 300 seconds.");

    return req;
}

void ApiClientView::ApplyTheme() {
    Theme::ApplyToWindow(hWnd_);
    std::vector<std::wstring> words;
    for(const auto& [key,value]:storeMgr_->GetMergedVariables("")) { (void)value; words.push_back(L"{{"+ToWide(key)+L"}}"); }
    for(const auto& v:selectedScopes_)if(v.enabled)words.push_back(L"{{"+ToWide(v.key)+L"}}");
    for(const auto& v:currentRequest_.variables)if(v.enabled)words.push_back(L"{{"+ToWide(v.key)+L"}}");
    CodeEditor::SetCompletions(hEditBody_,words);
    Layout();
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
static HistoryEntry MakeHistory(const ApiRequest& request,const ApiResponse& response,size_t sequence) {
    HistoryEntry history;
    history.id="hist_"+std::to_string(GetTickCount64())+"_"+std::to_string(sequence);
    history.method=response.method;history.url=response.url;history.statusCode=response.statusCode;
    history.statusText=response.statusText;history.latencyMs=response.latencyMs;history.sizeBytes=response.sizeBytes;
    history.executedAt=response.executedAt;history.requestSnapshot=request;return history;
}
void ApiClientView::RequestCancel() { if(pending_)pending_->cancelled.store(true); }
void ApiClientView::SendRequestAsync() {
    if(isExecuting_) { RequestCancel(); return; }
    ApiRequest request=GetCurrentRequest();const ApiRequest snapshot=request;
    if(request.auth.type==AuthType::Inherit)request.auth=selectedAuth_;
    auto variables=ResolveVariables(request,selectedScopes_);
    if(worker_.joinable())worker_.join();
    pending_=std::make_shared<Pending>();const auto task=pending_;
    auto* engine=httpEngine_;
    isExecuting_=true;SetWindowTextW(hBtnSend_,L"Cancel");EnableWindow(hBtnRunAll_,FALSE);
    SetTimer(hWnd_,72,50,nullptr);
    worker_=std::thread([task,engine,request,snapshot,variables]{
        try {
            task->response=engine->Execute(request,variables,false,&task->cancelled);
            task->history.push_back(MakeHistory(snapshot,task->response,0));
        } catch(...) {task->response.error="The request failed unexpectedly. Check request settings and try again.";}
        task->done.store(true,std::memory_order_release);
    });
}
void ApiClientView::RunCollectionAsync() {
    if(isExecuting_) { RequestCancel(); return; }
    struct Run {ApiRequest request;ApiRequest snapshot;std::map<std::string,std::string> variables;};
    std::vector<Run> requests;
    std::function<void(const Folder&,std::vector<Variable>,AuthConfig)> folder;
    auto add=[&](ApiRequest req,const std::vector<Variable>& scopes,const AuthConfig& auth) {
        const auto snapshot=req;
        if(req.auth.type==AuthType::Inherit)req.auth=auth;
        requests.push_back({req,snapshot,ResolveVariables(req,scopes)});
    };
    folder=[&](const Folder& f,std::vector<Variable> scopes,AuthConfig auth) {
        scopes.insert(scopes.end(),f.variables.begin(),f.variables.end());if(f.auth.type!=AuthType::Inherit)auth=f.auth;
        for(const auto& req:f.requests)add(req,scopes,auth);
        for(const auto& child:f.folders)folder(child,scopes,auth);
    };
    for(const auto& col:storeMgr_->GetCollections()) {
        for(const auto& req:col.requests)add(req,col.variables,col.auth);
        for(const auto& f:col.folders)folder(f,col.variables,col.auth);
    }
    if(requests.empty()){MessageBoxW(hWnd_,L"Add requests to a collection before running it.",L"Collection runner",MB_OK);return;}
    if(worker_.joinable())worker_.join();
    pending_=std::make_shared<Pending>();const auto task=pending_;task->runner=true;task->total=static_cast<int>(requests.size());
    auto* engine=httpEngine_;isExecuting_=true;EnableWindow(hBtnSend_,FALSE);SetWindowTextW(hBtnRunAll_,L"Cancel");
    SetTimer(hWnd_,72,50,nullptr);
    worker_=std::thread([task,engine,requests]{
        try {
            for(const auto& run:requests) {
                if(task->cancelled.load())break;
                const auto response=engine->Execute(run.request,run.variables,false,&task->cancelled);
                bool passed=response.error.empty() && response.statusCode>0;
                for(const auto& assertion:response.testResults)if(!assertion.passed)passed=false;
                if(passed)++task->passed;
                task->history.push_back(MakeHistory(run.snapshot,response,task->history.size()));
                task->completed.fetch_add(1);
            }
        } catch(...) {task->error="The collection could not complete. Check its requests and settings.";}
        task->done.store(true,std::memory_order_release);
    });
}
void ApiClientView::PollTask() {
    if(!pending_ || !pending_->done.load(std::memory_order_acquire)) return;
    if(worker_.joinable())worker_.join();
    auto task=pending_;pending_.reset();KillTimer(hWnd_,72);
    isExecuting_=false;EnableWindow(hBtnSend_,TRUE);EnableWindow(hBtnRunAll_,TRUE);SetWindowTextW(hBtnSend_,L"Send");SetWindowTextW(hBtnRunAll_,L"Run all");
    try {for(const auto& history:task->history)storeMgr_->AddHistory(history);}
    catch(...) {MessageBoxW(hWnd_,L"The operation finished, but history could not be saved. Check available disk space and workspace permissions.",L"History not saved",MB_ICONERROR);}
    if(task->runner) {
        std::wstring summary=(task->cancelled.load()?L"Runner cancelled. ":L"Runner finished. ");
        summary+=std::to_wstring(task->completed.load())+L"/"+std::to_wstring(task->total)+L" requests executed; "+std::to_wstring(task->passed)+L" passed.";
        if(!task->error.empty())summary+=L" "+ToWide(task->error);
        PopulateHistoryList();if(!GetPropW(hParent_,L"DataForge.Closing"))MessageBoxW(hWnd_,summary.c_str(),L"Collection runner",MB_OK);
    } else {
        auto* response=new ApiResponse(std::move(task->response));
        SendMessageW(hWnd_,WM_APP_REQ_COMPLETED,reinterpret_cast<WPARAM>(response),0);
    }
}
void ApiClientView::SaveCurrentRequest() {
    ApiRequest request=GetCurrentRequest();
    if(request.name.empty())throw std::runtime_error("Enter a name for the request before saving.");
    auto collections=storeMgr_->GetCollections();
    bool found=false;
    auto save=[&](std::vector<ApiRequest>& requests){for(auto& existing:requests)if(!request.id.empty()&&existing.id==request.id){existing=request;found=true;}};
    std::function<void(Folder&)> folder=[&](Folder& f){save(f.requests);for(auto& child:f.folders)folder(child);};
    for(auto& c:collections){save(c.requests);for(auto& f:c.folders)folder(f);}
    if(!found) {
        if(collections.empty()){Collection c;c.id="col_"+std::to_string(GetTickCount64());c.name="My collection";collections.push_back(c);}
        request.id="req_"+std::to_string(GetTickCount64());collections.front().requests.push_back(request);
    }
    storeMgr_->SaveCollections(collections);currentRequest_=request;PopulateCollectionsTree();
}
void ApiClientView::EditConfiguration() {
    using nlohmann::json;
    json values=json::array();
    if(requestTab_==4)for(const auto& t:currentRequest_.tests)values.push_back({{"type",t.type},{"expected",t.expected},{"field",t.field},{"header",t.header},{"name",t.name},{"enabled",t.enabled}});
    else for(const auto& p:requestTab_==0?currentRequest_.params:currentRequest_.headers)values.push_back({{"key",p.key},{"value",p.value},{"enabled",p.enabled},{"description",p.description}});
    auto text=ToWide(values.dump(2));
    const auto validation=[&](const std::wstring& value)->std::wstring{
        auto j=json::parse(ToUtf8(value),nullptr,false);if(!j.is_array())return L"Enter a JSON array of objects.";
        for(const auto& item:j){if(!item.is_object())return L"Each entry must be an object.";
            if(item.contains("enabled")&&!item["enabled"].is_boolean())return L"Enabled must be true or false.";
            for(const auto* key:requestTab_==4?std::vector<const char*>{"type","expected","field","header","name"}:std::vector<const char*>{"key","value","description"})
                if(item.contains(key)&&!item[key].is_string())return L"Keys, values, and assertion fields must be strings.";
        }return {};
    };
    if(EditNativeText(hWnd_,requestTab_==4?L"Assertions: type, expected, field, header, name":L"Parameters / headers: key, value, enabled",text,EditorLanguage::Json,validation)) {
        auto req=GetCurrentRequest();auto j=json::parse(ToUtf8(text));
        if(requestTab_==4){req.tests.clear();for(const auto& t:j)req.tests.push_back({t.value("type","status_2xx"),t.value("expected",""),t.value("field",""),t.value("header",""),t.value("name",""),t.value("enabled",true)});}
        else {auto& params=requestTab_==0?req.params:req.headers;params.clear();for(const auto& p:j)params.push_back({p.value("key",""),p.value("value",""),p.value("enabled",true),p.value("description","")});}
        LoadRequest(req);
    }
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
    int respY = rcRespTabBody_.top - Theme::Scale(30);
    RECT respTitleRc{ mainX, respY, mainX + Theme::Scale(200), respY + Theme::Scale(24) };
    SelectObject(hdc, Theme::GetBoldFont());
    SetTextColor(hdc, tc.textPrimary);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, L"Response", -1, &respTitleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (lastResponse_.statusCode > 0 || !lastResponse_.error.empty()) {
        std::wstring statusStr = (lastResponse_.statusCode > 0)
            ? std::to_wstring(lastResponse_.statusCode) + L" " + ToWide(lastResponse_.statusText)
            : L"Error";
        COLORREF sc = Theme::GetStatusColor(lastResponse_.statusCode);
        SetTextColor(hdc, sc);
        RECT statusRc{ mainX + Theme::Scale(100), respY, mainX + Theme::Scale(250), respY + Theme::Scale(24) };
        DrawTextW(hdc, statusStr.c_str(), -1, &statusRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SetTextColor(hdc, tc.textSecondary);
        std::wstring latStr = std::to_wstring(static_cast<int>(lastResponse_.latencyMs)) + L" ms";
        RECT latRc{ mainX + Theme::Scale(270), respY, mainX + Theme::Scale(360), respY + Theme::Scale(24) };
        DrawTextW(hdc, latStr.c_str(), -1, &latRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        std::wstring szStr = std::to_wstring(lastResponse_.sizeBytes) + L" B";
        RECT szRc{ mainX + Theme::Scale(380), respY, mainX + Theme::Scale(500), respY + Theme::Scale(24) };
        DrawTextW(hdc, szStr.c_str(), -1, &szRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

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
    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        WORD code = HIWORD(wParam);
        if(!pThis)return 0;
        if(id>=2400 && id<2407 && code==BN_CLICKED){pThis->requestTab_=id-2400;pThis->UpdateRequestTabsVisibility();InvalidateRect(hWnd,nullptr,TRUE);return 0;}
        if(id>=2410 && id<2413 && code==BN_CLICKED){pThis->responseTab_=id-2410;pThis->UpdateResponseTabsVisibility();InvalidateRect(hWnd,nullptr,TRUE);return 0;}
        if(id>=2420 && id<2422 && code==BN_CLICKED){pThis->sidebarTab_=id-2420;pThis->Layout();InvalidateRect(hWnd,nullptr,TRUE);return 0;}
        if(id==2110 && code==BN_CLICKED){pThis->SaveCurrentRequest();return 0;}
        if(id==2222 && code==BN_CLICKED){pThis->EditConfiguration();return 0;}
        if(id==2221 && code==BN_CLICKED){auto req=pThis->GetCurrentRequest();req.tests.push_back({"status_2xx","","","","Success status",true});pThis->LoadRequest(req);return 0;}
        if(id==2210 && code==CBN_SELCHANGE){const int type=static_cast<int>(SendMessageW(pThis->hComboBodyType_,CB_GETCURSEL,0,0));CodeEditor::SetLanguage(pThis->hEditBody_,type==2?EditorLanguage::PlainText:EditorLanguage::Json);if(type==3 || type==4)CodeEditor::SetText(pThis->hEditBody_,L"[]");return 0;}

        if (id == 2103) { // Send Button
            if (pThis) pThis->SendRequestAsync();
        } else if (id == 2104) { // Code Button
            if (pThis && pThis->OnOpenCodeGen) pThis->OnOpenCodeGen(pThis->GetCurrentRequest());
        } else if (id == 2010) { // Add Collection
            if (pThis) {
                auto cols = pThis->storeMgr_->GetCollections();
                Collection c;
                c.id = "col_" + std::to_string(GetTickCount64());
                c.name = "New Collection";
                cols.push_back(c);
                pThis->storeMgr_->SaveCollections(cols);
                pThis->PopulateCollectionsTree();
            }
        } else if (id == 2011) { // Add Request
            if (pThis) {
                auto cols = pThis->storeMgr_->GetCollections();
                if (!cols.empty()) {
                    ApiRequest r;
                    r.id = "req_" + std::to_string(GetTickCount64());
                    r.name = "New Request";
                    r.method = HttpMethod::GET;
                    r.url = "";
                    cols[0].requests.push_back(r);
                    pThis->storeMgr_->SaveCollections(cols);
                    pThis->PopulateCollectionsTree();
                }
            }
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
        if (nm->idFrom == 2001 && nm->code == TVN_SELCHANGEDW) {
            auto* item=reinterpret_cast<NMTREEVIEWW*>(lParam);
            const auto index=item->itemNew.lParam;
            if(index>0 && index<=static_cast<LPARAM>(pThis->treeRequests_.size())) {
                const auto bound=pThis->treeRequests_[static_cast<size_t>(index-1)];
                pThis->LoadRequest(bound.request);
                pThis->selectedScopes_=bound.scopes;pThis->selectedAuth_=bound.inheritedAuth;
            }
        } else if (nm->idFrom == 2002 && nm->code == NM_CLICK) { // History item selected
            int sel = ListView_GetNextItem(pThis->hListHistory_, -1, LVNI_SELECTED);
            if (sel >= 0) {
                auto hist = pThis->storeMgr_->GetHistory();
                if (sel < static_cast<int>(hist.size())) {
                    pThis->LoadRequest(hist[sel].requestSnapshot);
                }
            }
        }
        return 0;
    }
    case WM_APP_REQ_COMPLETED: {
        auto* pResp = reinterpret_cast<ApiResponse*>(wParam);
        if (pThis && pResp) {
            pThis->lastResponse_ = *pResp;
            pThis->isExecuting_ = false;
            EnableWindow(pThis->hBtnSend_, TRUE);
            SetWindowTextW(pThis->hBtnSend_, L"Send");

            // Update Response Viewer Body
            std::string body=pResp->error.empty()?pResp->body:pResp->error;
            if(pResp->isJson){auto json=nlohmann::json::parse(body,nullptr,false);if(!json.is_discarded())body=json.dump(2);}
            CodeEditor::SetLanguage(pThis->hEditRespBody_,pResp->isJson?EditorLanguage::Json:EditorLanguage::PlainText);
            CodeEditor::SetText(pThis->hEditRespBody_,ToWide(body));

            // Update Response Headers
            ListView_DeleteAllItems(pThis->hListRespHeaders_);
            for (int i = 0; i < static_cast<int>(pResp->headers.size()); ++i) {
                LVITEMW itm{};
                itm.mask = LVIF_TEXT;
                itm.iItem = i;
                std::wstring k = ToWide(pResp->headers[i].key);
                itm.pszText = const_cast<LPWSTR>(k.c_str());
                ListView_InsertItem(pThis->hListRespHeaders_, &itm);
                std::wstring v = ToWide(pResp->headers[i].value);
                ListView_SetItemText(pThis->hListRespHeaders_, i, 1, const_cast<LPWSTR>(v.c_str()));
            }

            // Update Test Results
            ListView_DeleteAllItems(pThis->hListRespTests_);
            for (int i = 0; i < static_cast<int>(pResp->testResults.size()); ++i) {
                const auto& tr = pResp->testResults[i];
                LVITEMW itm{};
                itm.mask = LVIF_TEXT;
                itm.iItem = i;
                std::wstring resStr = tr.passed ? L"PASS" : L"FAIL";
                itm.pszText = const_cast<LPWSTR>(resStr.c_str());
                ListView_InsertItem(pThis->hListRespTests_, &itm);
                std::wstring n = ToWide(tr.name);
                ListView_SetItemText(pThis->hListRespTests_, i, 1, const_cast<LPWSTR>(n.c_str()));
                std::wstring d = ToWide(tr.message);
                ListView_SetItemText(pThis->hListRespTests_, i, 2, const_cast<LPWSTR>(d.c_str()));
            }

            pThis->PopulateHistoryList();
            InvalidateRect(hWnd, nullptr, TRUE);
            delete pResp;
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
