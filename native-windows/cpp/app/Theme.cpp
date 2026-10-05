#include "Theme.h"
#include "CodeEditor.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>
#include <array>
#include <map>
#include <cmath>
#pragma comment(lib,"uxtheme.lib")
namespace native_app {
AppTheme Theme::currentTheme_=AppTheme::Dark;
ThemeColors Theme::colors_{};
HFONT Theme::hFontMain_=nullptr;
HFONT Theme::hFontBold_=nullptr;
HFONT Theme::hFontTitle_=nullptr;
HFONT Theme::hFontCode_=nullptr;
HFONT Theme::hFontSmall_=nullptr;
HBRUSH Theme::hBrBgPrimary_=nullptr;
HBRUSH Theme::hBrBgSecondary_=nullptr;
HBRUSH Theme::hBrBgTertiary_=nullptr;
namespace {
UINT dpi=96;
int textScale=100;
std::map<unsigned long long,std::array<HFONT,5>> fonts;
constexpr UINT_PTR ControlSubclass=0xDF10;
}
int Theme::Scale(int pixels){return MulDiv(pixels,static_cast<int>(dpi)*textScale,9600);}
int Theme::GetTextScale(){return textScale;}
void Theme::UpdateFonts(){
    const auto key=static_cast<unsigned long long>(dpi)*1000+textScale;
    auto found=fonts.find(key);
    if(found==fonts.end()){
        auto make=[](int size,int weight,const wchar_t* face,DWORD pitch){return CreateFontW(-Scale(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,pitch,face);};
        found=fonts.emplace(key,std::array<HFONT,5>{make(16,FW_NORMAL,L"Segoe UI",DEFAULT_PITCH),make(16,FW_SEMIBOLD,L"Segoe UI",DEFAULT_PITCH),make(20,FW_BOLD,L"Segoe UI",DEFAULT_PITCH),make(16,FW_NORMAL,L"Consolas",FIXED_PITCH|FF_MODERN),make(14,FW_NORMAL,L"Segoe UI",DEFAULT_PITCH)}).first;
    }
    const auto& set=found->second;hFontMain_=set[0];hFontBold_=set[1];hFontTitle_=set[2];hFontCode_=set[3];hFontSmall_=set[4];
}
void Theme::SetDpi(UINT value){if(value&&value!=dpi){dpi=value;UpdateFonts();}}
void Theme::SetTextScale(int value){value=(std::max)(100,(std::min)(150,value));if(value!=textScale){textScale=value;UpdateFonts();}}
void Theme::UpdateBrushes(){
    if(hBrBgPrimary_)DeleteObject(hBrBgPrimary_);if(hBrBgSecondary_)DeleteObject(hBrBgSecondary_);if(hBrBgTertiary_)DeleteObject(hBrBgTertiary_);
    hBrBgPrimary_=CreateSolidBrush(colors_.bgPrimary);hBrBgSecondary_=CreateSolidBrush(colors_.bgSecondary);hBrBgTertiary_=CreateSolidBrush(colors_.bgTertiary);
}
void Theme::SetTheme(AppTheme theme){
    currentTheme_=theme;const bool dark=theme==AppTheme::Dark;
    colors_.bgPrimary=dark?RGB(15,23,42):RGB(248,250,252);colors_.bgSecondary=dark?RGB(30,41,59):RGB(255,255,255);
    colors_.bgTertiary=dark?RGB(51,65,85):RGB(241,245,249);colors_.bgHover=dark?RGB(39,53,73):RGB(226,232,240);
    colors_.borderColor=dark?RGB(71,85,105):RGB(203,213,225);colors_.textPrimary=dark?RGB(248,250,252):RGB(15,23,42);
    colors_.textSecondary=dark?RGB(203,213,225):RGB(71,85,105);colors_.textMuted=dark?RGB(176,191,211):RGB(71,85,105);
    colors_.accent=RGB(37,99,235);colors_.accentHover=RGB(29,78,216);
    colors_.textOnAccent=RGB(255,255,255);
    colors_.methodGet=dark?RGB(110,231,183):RGB(4,120,87);colors_.methodPost=dark?RGB(253,230,138):RGB(146,64,14);
    colors_.methodPut=dark?RGB(147,197,253):RGB(29,78,216);colors_.methodDelete=dark?RGB(252,165,165):RGB(185,28,28);
    colors_.methodPatch=dark?RGB(196,181,253):RGB(109,40,217);colors_.methodOther=colors_.textSecondary;
    colors_.status2xx=colors_.methodGet;colors_.status3xx=colors_.methodPut;colors_.status4xx=colors_.methodPost;colors_.status5xx=colors_.methodDelete;
    HIGHCONTRASTW high{sizeof(high)};
    if(SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(high),&high,0)&&(high.dwFlags&HCF_HIGHCONTRASTON)){
        colors_.bgPrimary=colors_.bgSecondary=colors_.bgTertiary=colors_.bgHover=GetSysColor(COLOR_WINDOW);
        colors_.textPrimary=colors_.textSecondary=colors_.textMuted=GetSysColor(COLOR_WINDOWTEXT);colors_.accent=colors_.accentHover=GetSysColor(COLOR_HIGHLIGHT);colors_.borderColor=colors_.textPrimary;
        colors_.textOnAccent=GetSysColor(COLOR_HIGHLIGHTTEXT);
        colors_.methodGet=colors_.methodPost=colors_.methodPut=colors_.methodDelete=colors_.methodPatch=colors_.methodOther=colors_.textPrimary;
        colors_.status2xx=colors_.status3xx=colors_.status4xx=colors_.status5xx=colors_.textPrimary;
    }
    UpdateBrushes();if(!hFontMain_)UpdateFonts();
}
const ThemeColors& Theme::Get(){if(!hBrBgPrimary_)SetTheme(currentTheme_);return colors_;}
AppTheme Theme::GetCurrentTheme(){return currentTheme_;}
HFONT Theme::GetMainFont(){Get();return hFontMain_;}
HFONT Theme::GetBoldFont(){Get();return hFontBold_;}
HFONT Theme::GetTitleFont(){Get();return hFontTitle_;}
HFONT Theme::GetCodeFont(){Get();return hFontCode_;}
HFONT Theme::GetSmallFont(){Get();return hFontSmall_;}
HBRUSH Theme::GetBgPrimaryBrush(){Get();return hBrBgPrimary_;}
HBRUSH Theme::GetBgSecondaryBrush(){Get();return hBrBgSecondary_;}
HBRUSH Theme::GetBgTertiaryBrush(){Get();return hBrBgTertiary_;}
COLORREF Theme::GetMethodColor(const std::string& method){Get();std::string upper=method;std::transform(upper.begin(),upper.end(),upper.begin(),[](unsigned char c){return static_cast<char>(toupper(c));});if(upper=="GET")return colors_.methodGet;if(upper=="POST")return colors_.methodPost;if(upper=="PUT")return colors_.methodPut;if(upper=="DELETE")return colors_.methodDelete;if(upper=="PATCH")return colors_.methodPatch;return colors_.methodOther;}
COLORREF Theme::GetStatusColor(int code){Get();if(code>=200&&code<300)return colors_.status2xx;if(code>=300&&code<400)return colors_.status3xx;if(code>=400&&code<500)return colors_.status4xx;return colors_.status5xx;}
namespace {
LRESULT CALLBACK ControlProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR){
    if(msg==WM_NCDESTROY){RemoveWindowSubclass(hwnd,ControlProc,ControlSubclass);return DefSubclassProc(hwnd,msg,wp,lp);}
    if(msg==WM_CTLCOLORSTATIC||msg==WM_CTLCOLOREDIT||msg==WM_CTLCOLORLISTBOX||msg==WM_CTLCOLORBTN){const auto& c=Theme::Get();HDC dc=reinterpret_cast<HDC>(wp);SetTextColor(dc,c.textPrimary);SetBkColor(dc,c.bgSecondary);return reinterpret_cast<LRESULT>(Theme::GetBgSecondaryBrush());}
    wchar_t name[64]{};GetClassNameW(hwnd,name,64);
    const bool button=_wcsicmp(name,L"Button")==0&&(GetWindowLongPtrW(hwnd,GWL_STYLE)&BS_TYPEMASK)<=BS_DEFPUSHBUTTON;
    const bool header=_wcsicmp(name,WC_HEADERW)==0;
    const bool combo=_wcsicmp(name,L"ComboBox")==0&&(GetWindowLongPtrW(hwnd,GWL_STYLE)&0x3)==CBS_DROPDOWNLIST;
    if(button&&msg==WM_ERASEBKGND)return 1;
    if((button||header||combo)&&(msg==WM_PAINT||msg==WM_PRINTCLIENT)){
        PAINTSTRUCT ps{};const bool paint=msg==WM_PAINT;HDC dc=paint?BeginPaint(hwnd,&ps):reinterpret_cast<HDC>(wp);const int saved=SaveDC(dc);RECT rect{};GetClientRect(hwnd,&rect);const auto& c=Theme::Get();SetBkMode(dc,TRANSPARENT);
        if(button){
            const bool active=GetPropW(hwnd,L"DataForge.Active")!=nullptr;const bool pressed=(SendMessageW(hwnd,BM_GETSTATE,0,0)&BST_PUSHED)!=0;
            HBRUSH brush=CreateSolidBrush(active?c.accent:pressed?c.bgHover:c.bgTertiary);FillRect(dc,&rect,brush);DeleteObject(brush);
            HPEN pen=CreatePen(PS_SOLID,(std::max)(1,Theme::Scale(1)),GetFocus()==hwnd?c.accent:c.borderColor);
            HGDIOBJ oldPen=SelectObject(dc,pen);SelectObject(dc,GetStockObject(NULL_BRUSH));RoundRect(dc,rect.left,rect.top,rect.right,rect.bottom,Theme::Scale(6),Theme::Scale(6));SelectObject(dc,oldPen);DeleteObject(pen);
            SelectObject(dc,Theme::GetMainFont());SetTextColor(dc,active?c.textOnAccent:IsWindowEnabled(hwnd)?c.textPrimary:c.textMuted);
            wchar_t text[256]{};GetWindowTextW(hwnd,text,256);InflateRect(&rect,-Theme::Scale(4),0);DrawTextW(dc,text,-1,&rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
            if(GetFocus()==hwnd){InflateRect(&rect,-Theme::Scale(2),-Theme::Scale(3));DrawFocusRect(dc,&rect);}
        }else if(combo){
            FillRect(dc,&rect,Theme::GetBgTertiaryBrush());SelectObject(dc,Theme::GetMainFont());SetTextColor(dc,IsWindowEnabled(hwnd)?c.textPrimary:c.textMuted);
            const auto selection=SendMessageW(hwnd,CB_GETCURSEL,0,0);std::wstring text;
            if(selection!=CB_ERR){const auto length=SendMessageW(hwnd,CB_GETLBTEXTLEN,selection,0);if(length>=0&&length<32768){text.resize(static_cast<size_t>(length)+1);SendMessageW(hwnd,CB_GETLBTEXT,selection,reinterpret_cast<LPARAM>(text.data()));text.resize(static_cast<size_t>(length));}}
            RECT label=rect;label.left+=Theme::Scale(8);label.right-=Theme::Scale(26);DrawTextW(dc,text.c_str(),-1,&label,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
            RECT arrow=rect;arrow.left=arrow.right-Theme::Scale(24);DrawTextW(dc,L"\x25BE",1,&arrow,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            HPEN pen=CreatePen(PS_SOLID,1,GetFocus()==hwnd?c.accent:c.borderColor);const auto old=SelectObject(dc,pen);SelectObject(dc,GetStockObject(NULL_BRUSH));Rectangle(dc,rect.left,rect.top,rect.right,rect.bottom);SelectObject(dc,old);DeleteObject(pen);
            if(GetFocus()==hwnd){InflateRect(&label,-2,-3);DrawFocusRect(dc,&label);}
        }else{
            FillRect(dc,&rect,Theme::GetBgTertiaryBrush());SelectObject(dc,Theme::GetBoldFont());SetTextColor(dc,c.textPrimary);
            for(int i=0;i<Header_GetItemCount(hwnd);++i){RECT cell{};Header_GetItemRect(hwnd,i,&cell);wchar_t text[256]{};HDITEMW item{};item.mask=HDI_TEXT;item.pszText=text;item.cchTextMax=256;Header_GetItem(hwnd,i,&item);InflateRect(&cell,-Theme::Scale(8),0);DrawTextW(dc,text,-1,&cell,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);}
        }
        RestoreDC(dc,saved);if(paint)EndPaint(hwnd,&ps);return 0;
    }
    const auto result=DefSubclassProc(hwnd,msg,wp,lp);
    if(_wcsicmp(name,L"Edit")==0&&(msg==WM_PAINT||msg==WM_PRINTCLIENT)&&GetWindowTextLengthW(hwnd)==0&&GetFocus()!=hwnd){wchar_t cue[256]{};if(SendMessageW(hwnd,EM_GETCUEBANNER,reinterpret_cast<WPARAM>(cue),256)&&cue[0]){const bool paint=msg==WM_PAINT;HDC dc=paint?GetDC(hwnd):reinterpret_cast<HDC>(wp);if(dc){const int saved=SaveDC(dc);RECT rect{};GetClientRect(hwnd,&rect);FillRect(dc,&rect,Theme::GetBgSecondaryBrush());rect.left+=Theme::Scale(6);rect.right-=Theme::Scale(6);SelectObject(dc,Theme::GetMainFont());SetTextColor(dc,Theme::Get().textMuted);SetBkMode(dc,TRANSPARENT);DrawTextW(dc,cue,-1,&rect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);RestoreDC(dc,saved);if(paint)ReleaseDC(hwnd,dc);}}}
    if((button||combo)&&(msg==WM_SETFOCUS||msg==WM_KILLFOCUS||msg==WM_ENABLE||msg==BM_SETSTATE||msg==WM_SETTEXT||msg==CB_SETCURSEL||msg==WM_KEYDOWN||msg==WM_LBUTTONUP))InvalidateRect(hwnd,nullptr,FALSE);return result;
}
void ApplyControl(HWND hwnd){
    wchar_t name[64]{};GetClassNameW(hwnd,name,64);SetWindowSubclass(hwnd,ControlProc,ControlSubclass,0);
    if(CodeEditor::IsEditor(hwnd)){CodeEditor::ApplyTheme(hwnd);return;}
    SendMessageW(hwnd,WM_SETFONT,reinterpret_cast<WPARAM>(Theme::GetMainFont()),TRUE);const auto& c=Theme::Get();
    if(_wcsicmp(name,WC_LISTVIEWW)==0){SetWindowTheme(hwnd,L"",L"");ListView_SetBkColor(hwnd,c.bgPrimary);ListView_SetTextBkColor(hwnd,c.bgPrimary);ListView_SetTextColor(hwnd,c.textPrimary);}
    if(_wcsicmp(name,WC_TREEVIEWW)==0){SetWindowTheme(hwnd,L"",L"");TreeView_SetBkColor(hwnd,c.bgSecondary);TreeView_SetTextColor(hwnd,c.textPrimary);TreeView_SetLineColor(hwnd,c.borderColor);TreeView_SetItemHeight(hwnd,Theme::Scale(28));}
    if(GetParent(hwnd)&&(_wcsicmp(name,L"Button")==0||_wcsicmp(name,L"Edit")==0||_wcsicmp(name,L"ComboBox")==0||_wcsicmp(name,WC_LISTVIEWW)==0||_wcsicmp(name,WC_TREEVIEWW)==0))SetWindowLongPtrW(hwnd,GWL_STYLE,GetWindowLongPtrW(hwnd,GWL_STYLE)|WS_TABSTOP);
    InvalidateRect(hwnd,nullptr,TRUE);
}
BOOL CALLBACK ApplyChild(HWND hwnd,LPARAM){ApplyControl(hwnd);return TRUE;}
}
void Theme::ApplyToWindow(HWND hwnd){if(hwnd&&IsWindow(hwnd)){ApplyControl(hwnd);EnumChildWindows(hwnd,ApplyChild,0);RedrawWindow(hwnd,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);}}
bool Theme::VerifyContrast(){
    auto luminance=[](COLORREF color){auto linear=[](int v){const double c=v/255.0;return c<=0.04045?c/12.92:std::pow((c+0.055)/1.055,2.4);};return 0.2126*linear(GetRValue(color))+0.7152*linear(GetGValue(color))+0.0722*linear(GetBValue(color));};
    auto ratio=[&](COLORREF a,COLORREF b){double x=luminance(a),y=luminance(b);if(x<y)std::swap(x,y);return(x+0.05)/(y+0.05);};const auto saved=currentTheme_;bool okay=true;
    for(auto mode:{AppTheme::Dark,AppTheme::Light}){SetTheme(mode);for(auto fg:{colors_.textPrimary,colors_.textSecondary,colors_.textMuted,colors_.methodGet,colors_.methodPost,colors_.methodPut,colors_.methodDelete,colors_.methodPatch})for(auto bg:{colors_.bgPrimary,colors_.bgSecondary})okay=okay&&ratio(fg,bg)>=4.5;okay=okay&&ratio(colors_.textOnAccent,colors_.accent)>=4.5;}SetTheme(saved);return okay;
}
void Theme::Shutdown(){for(const auto& entry:fonts)for(HFONT font:entry.second)if(font)DeleteObject(font);fonts.clear();hFontMain_=hFontBold_=hFontTitle_=hFontCode_=hFontSmall_=nullptr;if(hBrBgPrimary_)DeleteObject(hBrBgPrimary_);if(hBrBgSecondary_)DeleteObject(hBrBgSecondary_);if(hBrBgTertiary_)DeleteObject(hBrBgTertiary_);hBrBgPrimary_=hBrBgSecondary_=hBrBgTertiary_=nullptr;}
}
