#pragma once

#include <stddef.h>
#include <stdint.h>
#include <coroutine>
#include <functional>

typedef const char16_t* string_t;

#if defined(__EMSCRIPTEN__)
#define API __attribute__((used))
#elif defined(__wasm__)
#define API __attribute__((visibility("default")))
#endif

__attribute__((import_module("env"), import_name("Pow"))) int Pow(int, int);
__attribute__((import_module("env"), import_name("Console"))) void Console(string_t);
__attribute__((import_module("env"), import_name("GetCurrentTime"))) float GetCurrentTime();
__attribute__((import_module("env"), import_name("Wait"))) void Wait(float);

__attribute__((import_module("adventure"), import_name("ADVUpdate"))) void ADVUpdate(void);
__attribute__((import_module("adventure"), import_name("GetADVState"))) int GetADVState(void);
__attribute__((import_module("adventure"), import_name("LoadTextTable"))) int LoadTextTable(string_t);
__attribute__((import_module("adventure"), import_name("SetTextMessage"))) void SetTextMessage(int);
