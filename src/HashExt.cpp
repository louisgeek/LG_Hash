#include "HashExt.h"
#include "LG_Hash.h"
#include "HashEngine.h"   // LongPath
#include "Lang.h"
#include "resource.h"

#include <new>
#include <vector>

// 单次多选最多算多少个文件：防止一次拖进上千个文件把属性页拖死
static constexpr UINT kMaxFiles = 64;

HRESULT CHashExt::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IShellExtInit) {
        *ppv = static_cast<IShellExtInit*>(this);
    } else if (riid == IID_IShellPropSheetExt) {
        *ppv = static_cast<IShellPropSheetExt*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

ULONG CHashExt::AddRef() {
    return InterlockedIncrement(&m_ref);
}

ULONG CHashExt::Release() {
    LONG r = InterlockedDecrement(&m_ref);
    if (r == 0) delete this;
    return r;
}

// 从 DragQueryFile 的 HDROP 里收集路径。返回收集到的文件数；
// 文件夹（属性页只处理文件）会被跳过。
static UINT CollectDrop(HDROP drop, std::vector<std::wstring>* out) {
    UINT got = 0;
    const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n && out->size() < kMaxFiles; ++i) {
        // 用两段式取长度，避免固定 32 KB 栈缓冲
        const UINT need = DragQueryFileW(drop, i, nullptr, 0);
        if (need == 0) continue;
        std::wstring buf(static_cast<size_t>(need) + 1, L'\0');
        if (DragQueryFileW(drop, i, buf.data(), need + 1) == 0) continue;
        buf.resize(wcslen(buf.c_str()));
        const DWORD attr = GetFileAttributesW(LongPath(buf).c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
        out->push_back(std::move(buf));
        ++got;
    }
    return got;
}

HRESULT CHashExt::Initialize(PCIDLIST_ABSOLUTE, IDataObject* pDataObj, HKEY) {
    m_paths.clear();
    if (!pDataObj) return E_FAIL;

    // 异常不得跨越 COM 边界
    try {
        FORMATETC fe = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM stg{};
        if (SUCCEEDED(pDataObj->GetData(&fe, &stg))) {
            HDROP drop = static_cast<HDROP>(stg.hGlobal);
            if (drop) CollectDrop(drop, &m_paths);
            ReleaseStgMedium(&stg);
        } else {
            // 某些宿主只提供 Shell IDList，没有 CF_HDROP
            fe.cfFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_SHELLIDLIST));
            if (SUCCEEDED(pDataObj->GetData(&fe, &stg))) {
                auto cida = static_cast<LPIDA>(GlobalLock(stg.hGlobal));
                if (cida) {
                    // 只取落在映射区内的子项；与 CF_HDROP 分支保持一致，
                    // 支持超过 MAX_PATH 的路径：SHGetPathFromIDListEx 不返回所需
                    // 长度，失败时逐步加大缓冲重试。
                    for (UINT i = 1; i <= cida->cidl && m_paths.size() < kMaxFiles; ++i) {
                        const SIZE_T base = cida->aoffset[i];
                        auto pidl = reinterpret_cast<PCITEMID_CHILD>(
                            reinterpret_cast<const BYTE*>(cida) + base);
                        const DWORD caps[] = { MAX_PATH, 1024u, 32768u };
                        for (DWORD cap : caps) {
                            std::wstring buf(static_cast<size_t>(cap), L'\0');
                            if (SHGetPathFromIDListEx(pidl, buf.data(), cap, GPFIDL_DEFAULT)) {
                                buf.resize(wcslen(buf.c_str()));
                                const DWORD attr = GetFileAttributesW(LongPath(buf).c_str());
                                if (attr == INVALID_FILE_ATTRIBUTES ||
                                    !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                                    m_paths.push_back(std::move(buf));
                                }
                                break;
                            }
                        }
                    }
                    GlobalUnlock(stg.hGlobal);
                }
                ReleaseStgMedium(&stg);
            }
        }
    } catch (...) {
        m_paths.clear();
        return E_FAIL;
    }

    return m_paths.empty() ? E_FAIL : S_OK;
}

HRESULT CHashExt::AddPages(LPFNADDPROPSHEETPAGE pfnAddPage, LPARAM lParam) {
    if (!pfnAddPage) return E_POINTER;
    if (m_paths.empty()) return E_FAIL;

    auto ctx = new (std::nothrow) PageContext{ m_paths, nullptr };
    if (!ctx) return E_OUTOFMEMORY;

    PROPSHEETPAGEW psp{};
    psp.dwSize = sizeof(psp);
    psp.dwFlags = PSP_USETITLE | PSP_USECALLBACK | PSP_USEREFPARENT;
    psp.hInstance = g_hModule;
    psp.pszTemplate = MAKEINTRESOURCEW(IDD_LG_HASH);
    psp.pszTitle = Lang::Get(Lang::Id::PageTitle);
    psp.pfnDlgProc = LG_HashProc;
    psp.pfnCallback = LG_HashCallback;
    psp.lParam = reinterpret_cast<LPARAM>(ctx);
    // 属性表管理器负责加减 DLL 引用计数，页面未打开也不会提前卸载
    psp.pcRefParent = reinterpret_cast<UINT*>(&g_locks);

    HPROPSHEETPAGE hpage = CreatePropertySheetPageW(&psp);
    if (!hpage) {
        delete ctx;
        return E_FAIL;
    }
    if (!pfnAddPage(hpage, lParam)) {
        DestroyPropertySheetPage(hpage);
        return E_FAIL;
    }
    return S_OK;
}

HRESULT CHashExt::ReplacePage(EXPPS, LPFNSVADDPROPSHEETPAGE, LPARAM) {
    return E_NOTIMPL;
}
