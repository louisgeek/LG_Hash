#pragma once

#include <windows.h>
#include <prsht.h>
#include <string>
#include <vector>

struct Job;

struct PageContext {
    std::vector<std::wstring> paths;   // 选中的文件；多选时按序串行计算
    Job* job = nullptr;

    bool Multi() const { return paths.size() > 1; }
};

INT_PTR CALLBACK LG_HashProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
UINT CALLBACK LG_HashCallback(HWND hwnd, UINT msg, LPPROPSHEETPAGEW ppsp);
