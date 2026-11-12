#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <new>
#include <string>
#include "HashExt.h"
#include "Lang.h"

#pragma comment(linker, "/export:DllGetClassObject,PRIVATE")
#pragma comment(linker, "/export:DllCanUnloadNow,PRIVATE")
#pragma comment(linker, "/export:DllRegisterServer,PRIVATE")
#pragma comment(linker, "/export:DllUnregisterServer,PRIVATE")

HINSTANCE g_hModule = nullptr;
LONG g_locks = 0;

static const wchar_t kClsid[] = L"{06FE4DCB-7AD4-4594-83CD-E7753BDA89FC}";
static const wchar_t kFriendlyName[] = L"File Hash Property Page";

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hinst;
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}

class CClassFactory final : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }

    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto ext = new (std::nothrow) CHashExt();
        if (!ext) return E_OUTOFMEMORY;
        HRESULT hr = ext->QueryInterface(riid, ppv);
        ext->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_locks);
        else InterlockedDecrement(&g_locks);
        return S_OK;
    }

private:
    LONG m_ref = 1;
};

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    CLSID clsid;
    if (FAILED(CLSIDFromString(kClsid, &clsid)) || rclsid != clsid)
        return CLASS_E_CLASSNOTAVAILABLE;
    auto factory = new (std::nothrow) CClassFactory();
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    return g_locks == 0 ? S_OK : S_FALSE;
}

static HRESULT RegSet(HKEY root, const std::wstring& subkey, const wchar_t* name, const std::wstring& value) {
    HKEY key;
    LSTATUS s = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                KEY_SET_VALUE, nullptr, &key, nullptr);
    if (s != ERROR_SUCCESS) return HRESULT_FROM_WIN32(s);
    s = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(s);
}

STDAPI DllRegisterServer() {
    wchar_t path[1024];
    DWORD n = GetModuleFileNameW(g_hModule, path, 1024);
    if (n == 0 || n >= 1024) return HRESULT_FROM_WIN32(GetLastError());

    const std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + kClsid;
    const std::wstring handlerKey =
        std::wstring(L"Software\\Classes\\*\\shellex\\PropertySheetHandlers\\") + kClsid;

    HRESULT hr = RegSet(HKEY_CURRENT_USER, clsidKey, nullptr, kFriendlyName);
    if (FAILED(hr)) return hr;
    hr = RegSet(HKEY_CURRENT_USER, clsidKey + L"\\InprocServer32", nullptr, path);
    if (FAILED(hr)) return hr;
    hr = RegSet(HKEY_CURRENT_USER, clsidKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
    if (FAILED(hr)) return hr;
    hr = RegSet(HKEY_CURRENT_USER, handlerKey, nullptr, kFriendlyName);
    if (FAILED(hr)) return hr;

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    const std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + kClsid;
    const std::wstring handlerKey =
        std::wstring(L"Software\\Classes\\*\\shellex\\PropertySheetHandlers\\") + kClsid;
    RegDeleteTreeW(HKEY_CURRENT_USER, clsidKey.c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, handlerKey.c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}