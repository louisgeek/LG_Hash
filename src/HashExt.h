#pragma once

#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>

extern HINSTANCE g_hModule;
extern LONG g_locks;

class CHashExt final : public IShellExtInit, public IShellPropSheetExt {
public:
    CHashExt() { InterlockedIncrement(&g_locks); }
    ~CHashExt() { InterlockedDecrement(&g_locks); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE Initialize(PCIDLIST_ABSOLUTE pidlFolder, IDataObject* pDataObj, HKEY hProgID) override;
    HRESULT STDMETHODCALLTYPE AddPages(LPFNADDPROPSHEETPAGE pfnAddPage, LPARAM lParam) override;
    HRESULT STDMETHODCALLTYPE ReplacePage(EXPPS uPageID, LPFNSVADDPROPSHEETPAGE pfnReplaceWith, LPARAM lParam) override;

    const std::vector<std::wstring>& Paths() const { return m_paths; }

private:
    LONG m_ref = 1;
    std::vector<std::wstring> m_paths;
};