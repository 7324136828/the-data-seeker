#include "MainWindow.h"
#include "CodeEditor.h"
#include "TempWorkspace.h"
#include "NativeMockServer.h"
#include "ErLayout.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <vector>
#include <stdexcept>
#include <iterator>
#include <richedit.h>

namespace native_app {
namespace {
void PaintTree(HWND window,HDC dc) {
    SendMessageW(window,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_NONCLIENT);
    wchar_t className[64]{};GetClassNameW(window,className,64);
    if(_wcsicmp(className,L"ComboBox")==0)return; // The native combo paints its collapsed children itself.
    std::vector<HWND> children;
    for(HWND child=GetWindow(window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))children.push_back(child);
    for(auto it=children.rbegin();it!=children.rend();++it) {
        HWND child=*it;if(!(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE))continue;
        RECT rect{};GetWindowRect(child,&rect);POINT origin{rect.left,rect.top};MapWindowPoints(nullptr,window,&origin,1);
        const int saved=SaveDC(dc);OffsetViewportOrgEx(dc,origin.x,origin.y,nullptr);IntersectClipRect(dc,0,0,rect.right-rect.left,rect.bottom-rect.top);
        PaintTree(child,dc);RestoreDC(dc,saved);
    }
}
bool SaveView(HWND window,const std::filesystem::path& path) {
    RECT rect{};GetClientRect(window,&rect);if(rect.right<=0||rect.bottom<=0)return false;
    HDC screen=GetDC(window),dc=CreateCompatibleDC(screen);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=rect.right;info.bmiHeader.biHeight=-rect.bottom;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if(!bitmap){DeleteDC(dc);ReleaseDC(window,screen);return false;}
    const auto old=SelectObject(dc,bitmap);FillRect(dc,&rect,Theme::GetBgPrimaryBrush());PaintTree(window,dc);
    const DWORD size=static_cast<DWORD>(rect.right*rect.bottom*4);
    BITMAPFILEHEADER header{};header.bfType=0x4D42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);header.bfSize=header.bfOffBits+size;
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(&header),sizeof(header));file.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(BITMAPINFOHEADER));file.write(reinterpret_cast<const char*>(pixels),size);file.flush();const bool good=file.good();
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(window,screen);return good;
}
bool VisibleChildrenFit(HWND parent,std::wstring& failure) {
    RECT client{};GetClientRect(parent,&client);bool fits=true;
    for(HWND child=GetWindow(parent,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
        if(!(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE))continue;
        RECT rect{};GetWindowRect(child,&rect);MapWindowPoints(nullptr,parent,reinterpret_cast<POINT*>(&rect),2);
        if(rect.left<0||rect.top<0||rect.right>client.right+1||rect.bottom>client.bottom+1){
            wchar_t name[100]{};GetWindowTextW(child,name,100);failure+=L"Clipped control "+std::to_wstring(GetDlgCtrlID(child))+L" ("+name+L"); ";fits=false;
        }
    }
    return fits;
}
}
bool MainWindow::RunSelfTests(std::wstring& failure) {
    bool okay=true;
    const auto check=[&](bool value,const wchar_t* name){if(!value){failure+=std::wstring(name)+L"; ";okay=false;}return value;};
    try {
        TempWorkspace workspace("native-ui-");
        MainWindow main;main.dataDir_=workspace.GetPath().u8string();
        check(main.Create(GetModuleHandleW(nullptr),SW_HIDE,1366,920),L"whole native window initializes");
        if(!main.hWnd_)return false;
        check(main.storeMgr_->GetCollections().empty()&&main.storeMgr_->GetHistory().empty()&&main.dbEngine_->ListDatabases().empty(),L"fresh workspace is empty");
        auto pump=[&](bool waitForClose=false) {
            const auto deadline=GetTickCount64()+10000;
            while(GetTickCount64()<deadline) {
                MSG message{};
                while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                    if(message.message==WM_QUIT)continue;
                    if(!main.ProcessMessage(message)){TranslateMessage(&message);DispatchMessageW(&message);}
                }
                const bool busy=(main.apiClientView_&&main.apiClientView_->IsBusy())||(main.dbStudioView_&&main.dbStudioView_->IsBusy());
                if(waitForClose?!IsWindow(main.hWnd_):!busy)return true;
                MsgWaitForMultipleObjects(0,nullptr,FALSE,10,QS_ALLINPUT);
            }
            return false;
        };
        check(pump(),L"initial native jobs finish");
        wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,static_cast<DWORD>(std::size(executable)));
        const auto artifacts=std::filesystem::path(executable).parent_path()/"ui-artifacts";std::filesystem::create_directories(artifacts);
        ApiRequest request;request.id="ui_saved";request.name="Unicode request - \xC3\xA9";request.url="https://example.invalid/{{resource}}";request.method=HttpMethod::POST;
        request.body.type=BodyType::Json;request.body.content="{\"message\": \"hello\", \"active\": true, \"count\": 42}";
        request.variables.push_back({"resource","items",true,""});
        main.apiClientView_->LoadRequest(request);main.apiClientView_->SaveCurrentRequest();
        const auto collections=main.storeMgr_->GetCollections();check(collections.size()==1&&collections.front().requests.size()==1&&collections.front().requests.front().name==request.name,L"native request save persists Unicode");
        const auto saved=collections.front().requests.front();HistoryEntry entry;entry.id="ui_history";entry.method="POST";entry.url=saved.url;entry.requestSnapshot=saved;main.storeMgr_->AddHistory(entry);main.apiClientView_->RefreshData();
        HWND api=GetDlgItem(main.hWnd_,1002),history=GetDlgItem(api,2002);ListView_SetItemState(history,0,LVIS_SELECTED,LVIS_SELECTED);NMHDR selected{history,2002,NM_CLICK};SendMessageW(api,WM_NOTIFY,2002,reinterpret_cast<LPARAM>(&selected));
        check(main.apiClientView_->GetCurrentRequest().url==saved.url&&main.apiClientView_->GetCurrentRequest().body.content==saved.body.content,L"history reloads protected saved request context");
        const auto completeScriptKey=[&](HWND editor,const std::wstring& prefix,const std::wstring& expected){
            CodeEditor::SetText(editor,prefix+L"\");");CHARRANGE caret{static_cast<LONG>(prefix.size()),static_cast<LONG>(prefix.size())};SendMessageW(editor,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&caret));
            BYTE original[256]{},pressed[256]{};const BOOL keyboard=GetKeyboardState(original);if(keyboard){std::copy(std::begin(original),std::end(original),std::begin(pressed));pressed[VK_CONTROL]|=0x80;SetKeyboardState(pressed);SendMessageW(editor,WM_KEYDOWN,VK_SPACE,0);SetKeyboardState(original);}
            SendMessageW(editor,WM_KEYDOWN,VK_RETURN,0);SendMessageW(editor,WM_CHAR,L'\r',0);return keyboard&&CodeEditor::GetText(editor)==expected;
        };
        check(completeScriptKey(GetDlgItem(api,2232),L"pm.variables.get(\"reso",L"pm.variables.get(\"resource\");"),L"request script completion uses local variable names");
        CodeEditor::SetText(GetDlgItem(api,2230),LR"([{"key":"draftHint","value":"fresh","enabled":true}])");SendMessageW(api,WM_COMMAND,MAKEWPARAM(2230,EN_CHANGE),reinterpret_cast<LPARAM>(GetDlgItem(api,2230)));
        check(completeScriptKey(GetDlgItem(api,2233),L"pm.variables.get(\"draftH",L"pm.variables.get(\"draftHint\");"),L"post-response completion updates from unsaved variable drafts");main.apiClientView_->LoadRequest(saved);
        HWND bodyType=GetDlgItem(api,2210);SendMessageW(bodyType,CB_SETCURSEL,4,0);SendMessageW(api,WM_COMMAND,MAKEWPARAM(2210,CBN_SELCHANGE),reinterpret_cast<LPARAM>(bodyType));
        check(CodeEditor::GetText(GetDlgItem(api,2211))==L"{\"message\": \"hello\", \"active\": true, \"count\": 42}",L"changing body mode preserves unsaved content");main.apiClientView_->LoadRequest(saved);
        SendMessageW(api,WM_COMMAND,2403,0);
        check(VisibleChildrenFit(main.headerBar_.GetHwnd(),failure)&&VisibleChildrenFit(api,failure),L"API default layout stays in bounds");
        check(SaveView(main.hWnd_,artifacts/"api-dark.bmp"),L"dark API view renders");
        Theme::SetTheme(AppTheme::Light);main.ApplyTheme();check(SaveView(main.hWnd_,artifacts/"api-light.bmp"),L"light API view renders");
        Theme::SetTextScale(150);main.ApplyTheme();check(VisibleChildrenFit(main.headerBar_.GetHwnd(),failure),L"enlarged header stays in bounds");
        for(int tab=0;tab<9;++tab){SendMessageW(api,WM_COMMAND,2400+tab,0);check(VisibleChildrenFit(api,failure),L"enlarged API section stays in bounds");}
        for(int auth=0;auth<5;++auth){SendMessageW(api,WM_COMMAND,2402,0);HWND selector=GetDlgItem(api,2203);SendMessageW(selector,CB_SETCURSEL,auth,0);SendMessageW(api,WM_COMMAND,MAKEWPARAM(2203,CBN_SELCHANGE),reinterpret_cast<LPARAM>(selector));check(VisibleChildrenFit(api,failure),L"enlarged authentication controls stay in bounds");}
        for(int body=0;body<5;++body){SendMessageW(api,WM_COMMAND,2403,0);SendMessageW(bodyType,CB_SETCURSEL,body,0);SendMessageW(api,WM_COMMAND,MAKEWPARAM(2210,CBN_SELCHANGE),reinterpret_cast<LPARAM>(bodyType));check(VisibleChildrenFit(api,failure),L"enlarged body controls stay in bounds");}
        for(int response:{2410,2411,2412,2414}){SendMessageW(api,WM_COMMAND,response,0);check(VisibleChildrenFit(api,failure),L"enlarged response controls stay in bounds");}SendMessageW(api,WM_COMMAND,2410,0);main.apiClientView_->LoadRequest(saved);SendMessageW(api,WM_COMMAND,2403,0);
        check(SaveView(main.hWnd_,artifacts/"api-150.bmp"),L"enlarged API view renders");Theme::SetTextScale(100);Theme::SetTheme(AppTheme::Dark);main.ApplyTheme();
        const auto before=main.apiClientView_->DocumentCount();main.apiClientView_->NewDocument();check(main.apiClientView_->DocumentCount()==before+1,L"new request document is independent");
        SetWindowTextW(GetDlgItem(api,2105),L"Unfinished draft");CodeEditor::SetText(GetDlgItem(api,2211),L"{ unfinished");CodeEditor::SetText(GetDlgItem(api,2230),L"[ unfinished");CodeEditor::SetText(GetDlgItem(api,2231),L"{ unfinished");CodeEditor::SetText(GetDlgItem(api,2232),L"const unfinished =");main.apiClientView_->CycleDocument(true);main.apiClientView_->CycleDocument();check(CodeEditor::GetText(GetDlgItem(api,2211))==L"{ unfinished"&&CodeEditor::GetText(GetDlgItem(api,2230))==L"[ unfinished"&&CodeEditor::GetText(GetDlgItem(api,2231))==L"{ unfinished"&&CodeEditor::GetText(GetDlgItem(api,2232))==L"const unfinished =",L"document switching preserves invalid drafts without parsing");main.apiClientView_->CloseDocument(false);
        const auto database=main.dbEngine_->CreateDatabase("UI review","ecommerce");NativeMockServer mock(main.dbEngine_.get());const auto port=mock.Start(0);const auto base="http://127.0.0.1:"+std::to_string(port)+"/api/mock/data/";
        ApiRequest products;products.id="ui_products";products.name="Products script document";products.url=base+"products?db="+database.id;products.postResponseScript="pm.test('Products response', () => pm.expect(pm.response.json().table).to.equal('products')); pm.globals.set('uiObserved', true); console.log('products document');";main.apiClientView_->OpenRequest(products);main.apiClientView_->Execute();
        ApiRequest customers;customers.id="ui_customers";customers.name="Customers script document";customers.url=base+"customers?db="+database.id;customers.postResponseScript="pm.test('Customers response', () => pm.expect(pm.response.json().table).to.equal('customers')); console.log('customers document');";main.apiClientView_->OpenRequest(customers);main.apiClientView_->Execute();check(pump(),L"parallel document HTTP/script jobs finish");
        check(CodeEditor::GetText(GetDlgItem(api,2301)).find(L"customers")!=std::wstring::npos&&ListView_GetItemCount(GetDlgItem(api,2303))==1,L"active document owns its response and script assertions");main.apiClientView_->CycleDocument(true);check(CodeEditor::GetText(GetDlgItem(api,2301)).find(L"products")!=std::wstring::npos&&CodeEditor::GetText(GetDlgItem(api,2304)).find(L"products document")!=std::wstring::npos,L"background document retains its response and console");
        bool typedGlobal=false;for(const auto& value:main.storeMgr_->GetGlobals())if(value.key=="uiObserved"&&value.scriptJsonValue=="true")typedGlobal=true;check(typedGlobal,L"script completion saves a typed protected global");
        Collection runner;runner.id="ui_runner";runner.name="UI scripted runner";runner.variables.push_back({"counter","0",true,"0"});runner.preRequestScript="pm.collectionVariables.set('counter', Number(pm.collectionVariables.get('counter')) + 1);";products.id="ui_run_1";products.postResponseScript="pm.test('Inherited counter first', () => pm.expect(pm.collectionVariables.get('counter')).to.equal(1));";customers.id="ui_run_2";customers.postResponseScript="pm.test('Inherited counter next', () => pm.expect(pm.collectionVariables.get('counter')).to.equal(2));";runner.requests={products,customers};auto runnerCollections=main.storeMgr_->GetCollections();runnerCollections.push_back(runner);main.storeMgr_->SaveCollections(runnerCollections);main.apiClientView_->RefreshData();
        HWND tree=GetDlgItem(api,2001);for(auto item=TreeView_GetRoot(tree);item;item=TreeView_GetNextSibling(tree,item)){wchar_t name[100]{};TVITEMW info{};info.mask=TVIF_TEXT;info.hItem=item;info.pszText=name;info.cchTextMax=100;TreeView_GetItem(tree,&info);if(std::wstring(name)==L"UI scripted runner"){TreeView_SelectItem(tree,item);break;}}SendMessageW(api,WM_COMMAND,2612,0);HWND runnerWindow=FindWindowW(L"DataForgeNativeCollectionRunner",nullptr);if(runnerWindow)ShowWindow(runnerWindow,SW_HIDE);check(pump(),L"native collection runner completes");
        if(runnerWindow){HWND results=GetDlgItem(runnerWindow,5);check(ListView_GetItemCount(results)==2,L"runner keeps both request results");for(int row=0;row<2;++row){wchar_t status[16]{};ListView_GetItemText(results,row,0,status,16);check(std::wstring(status)==L"PASS",L"runner propagates inherited script variable changes");}check(VisibleChildrenFit(runnerWindow,failure),L"runner controls stay in bounds");check(SaveView(runnerWindow,artifacts/"runner-dark.bmp"),L"runner results render");Theme::SetTextScale(150);main.ApplyTheme();SetWindowPos(runnerWindow,nullptr,0,0,700,450,SWP_NOMOVE|SWP_NOZORDER);check(VisibleChildrenFit(runnerWindow,failure),L"enlarged narrow runner controls stay in bounds");check(SaveView(runnerWindow,artifacts/"runner-150.bmp"),L"enlarged narrow runner renders");Theme::SetTextScale(100);main.ApplyTheme();DestroyWindow(runnerWindow);}else check(false,L"native runner result window exists");
        SetWindowTextW(GetDlgItem(api,2003),L"no-request-matches-this");check(TreeView_GetCount(tree)==0,L"collection search filters displayed items");SetWindowTextW(GetDlgItem(api,2003),L"");check(TreeView_GetCount(tree)>=5,L"clearing search restores collection tree");
        mock.Stop();main.apiClientView_->OpenRequest(saved);SendMessageW(api,WM_COMMAND,2403,0);check(SaveView(main.hWnd_,artifacts/"api-documents-dark.bmp"),L"multiple native API documents render");
        main.dbStudioView_->RefreshData();check(pump(),L"sample schema is loaded asynchronously");
        main.headerBar_.SetMode(WorkbenchMode::DatabaseStudio);main.dbStudioView_->LoadSql("SELECT id, name, price FROM products ORDER BY price DESC LIMIT 10;");main.dbStudioView_->Execute();check(pump(),L"whole native SQL workflow finishes");
        check(VisibleChildrenFit(main.dbStudioView_->GetHwnd(),failure),L"database default layout stays in bounds");check(SaveView(main.hWnd_,artifacts/"sql-dark.bmp"),L"native SQL view renders");
        Theme::SetTextScale(150);main.ApplyTheme();check(VisibleChildrenFit(main.dbStudioView_->GetHwnd(),failure),L"enlarged database layout stays in bounds");check(SaveView(main.hWnd_,artifacts/"sql-150.bmp"),L"enlarged SQL view renders");Theme::SetTextScale(100);main.ApplyTheme();
        HWND databaseView = main.dbStudioView_->GetHwnd();
        SendMessageW(databaseView, WM_COMMAND, 3403, 0); SendMessageW(databaseView, WM_COMMAND, 3405, 0);
        check(VisibleChildrenFit(databaseView, failure), L"automatically arranged ER view controls stay in bounds");
        check(SaveView(main.hWnd_, artifacts/"er-dark.bmp"), L"column-level ER connectors render in dark mode");
        // A larger hidden review window exposes the entire sample at the host DPI.
        RECT originalWindow{}; GetWindowRect(main.hWnd_, &originalWindow);
        SetWindowPos(main.hWnd_, nullptr, 0, 0, Theme::Scale(1280), Theme::Scale(950), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        check(VisibleChildrenFit(databaseView, failure), L"complete ER overview controls stay in bounds");
        check(SaveView(main.hWnd_, artifacts/"er-overview-dark.bmp"), L"complete sample ER overview renders");
        const auto sampleSchema = main.dbEngine_->GetSchema(database.id);
        std::vector<ErLayoutNode> layoutNodes; std::vector<ErLayoutEdge> layoutEdges;
        for (const auto& table : sampleSchema.tables) {
            layoutNodes.push_back({table.name, 235, 38 + static_cast<int>(table.columns.size()) * 24});
            for (const auto& key : table.foreignKeys) layoutEdges.push_back({table.name, key.targetTable});
        }
        const auto arranged = ComputeErLayout(layoutNodes, layoutEdges); const auto product = arranged.find("products");
        if (product != arranged.end()) {
            RECT tab{}, bar{}; GetWindowRect(GetDlgItem(databaseView, 3400), &tab); GetWindowRect(GetDlgItem(databaseView, 3405), &bar);
            MapWindowPoints(nullptr, databaseView, reinterpret_cast<POINT*>(&tab), 2); MapWindowPoints(nullptr, databaseView, reinterpret_cast<POINT*>(&bar), 2);
            const int x = tab.left + Theme::Scale(product->second.x + 24), y = bar.bottom + Theme::Scale(6 + product->second.y + 44);
            SendMessageW(databaseView, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y)); SendMessageW(databaseView, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
            check(SaveView(main.hWnd_, artifacts/"er-selected-dark.bmp"), L"selected ER table highlights its relationships");
            SendMessageW(databaseView, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(tab.left + Theme::Scale(5), bar.bottom + Theme::Scale(11)));
        } else check(false, L"ER overview contains the sample products table");
        SetWindowPos(main.hWnd_, nullptr, 0, 0, originalWindow.right - originalWindow.left, originalWindow.bottom - originalWindow.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        Theme::SetTheme(AppTheme::Light); main.ApplyTheme();
        check(SaveView(main.hWnd_, artifacts/"er-light.bmp"), L"column-level ER connectors render in light mode");
        Theme::SetTextScale(150); main.ApplyTheme();
        check(VisibleChildrenFit(databaseView, failure), L"ER controls stay in bounds at 150% text");
        check(SaveView(main.hWnd_, artifacts/"er-150.bmp"), L"enlarged ER connectors render with native scrolling");
        Theme::SetTextScale(100); Theme::SetTheme(AppTheme::Dark); main.ApplyTheme();
        SendMessageW(databaseView, WM_COMMAND, 3400, 0);
        HWND editor=GetDlgItem(main.dbStudioView_->GetHwnd(),3101);MSG enter{};enter.hwnd=editor;enter.message=WM_KEYDOWN;enter.wParam=VK_RETURN;
        check((SendMessageW(editor,WM_GETDLGCODE,VK_RETURN,reinterpret_cast<LPARAM>(&enter))&DLGC_WANTMESSAGE)!=0,L"SQL editor receives Enter through dialog navigation");
        main.dbStudioView_->LoadSql("WITH RECURSIVE counter(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM counter WHERE n<100000000) SELECT sum(n) FROM counter;");main.dbStudioView_->Execute();SendMessageW(main.hWnd_,WM_CLOSE,0,0);
        check(pump(true)&&!IsWindow(main.hWnd_),L"closing cancels work without blocking the message loop");
    }catch(const std::exception&){failure+=L"Whole-window native integration failed; ";okay=false;}
    return okay;
}
}
