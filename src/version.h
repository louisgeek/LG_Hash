#pragma once

// LG_Hash 版本号的唯一来源。改版本号只需要改下面四个数字。
//
// 派生关系：
//   .rc（资源编译器）   通过 build\version_gen.h 得到逗号形式和字符串形式
//                       该文件由 tools\gen-version-header.ps1 在构建前生成
//   .cpp（C++ 编译器）  直接用下面的宏 + 本文件的字符串化宏
//
// 为什么 rc 需要单独生成一个头：
//   rc.exe 的预处理器不支持 # 字符串化运算符（`#define STR(x) #x` 会报
//   RC2104: undefined keyword or key name）。C++ 支持，rc 不支持，所以
//   字符串形式不能由 rc 自己从数字推导，必须在构建前落成字面量。
//
// 这个头文件同时被 .rc 之外的 C++ 代码包含，所以这里不能用 C++ 语法，
// 只能用预处理宏。

#define LG_HASH_VERSION_MAJOR 0
#define LG_HASH_VERSION_MINOR 0
#define LG_HASH_VERSION_PATCH 3
#define LG_HASH_VERSION_BUILD 0

// ---- 仅供 C++ 使用（rc 不包含本文件）----

// 先展开再转字符串，否则会得到宏名字面量
#define LG_HASH_STR_(x) #x
#define LG_HASH_STR(x) LG_HASH_STR_(x)

#define LG_HASH_WIDEN_(x) L##x
#define LG_HASH_WIDEN(x) LG_HASH_WIDEN_(x)

// 面向用户的短版本 L"0.0.3"（不含 build 位）
#define LG_HASH_VERSION_SHORT \
    LG_HASH_WIDEN(LG_HASH_STR(LG_HASH_VERSION_MAJOR) "." \
                  LG_HASH_STR(LG_HASH_VERSION_MINOR) "." \
                  LG_HASH_STR(LG_HASH_VERSION_PATCH))

// 完整版本 L"0.0.3.0"
#define LG_HASH_VERSION_WIDE \
    LG_HASH_WIDEN(LG_HASH_STR(LG_HASH_VERSION_MAJOR) "." \
                  LG_HASH_STR(LG_HASH_VERSION_MINOR) "." \
                  LG_HASH_STR(LG_HASH_VERSION_PATCH) "." \
                  LG_HASH_STR(LG_HASH_VERSION_BUILD))
