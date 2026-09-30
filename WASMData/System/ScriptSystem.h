#pragma once

#include <stddef.h>
#include <stdint.h>

typedef const unsigned short* string_t;

#define API __attribute__((visibility("default")))
__attribute__((import_module("env"), import_name("Test1"))) void Test1(int);

__attribute__((import_module("env"), import_name("Pow"))) int Pow(int, int);
__attribute__((import_module("env"), import_name("Console"))) void Console(string_t);
__attribute__((import_module("env"), import_name("LoadTextTable"))) int LoadTextTable(string_t);
__attribute__((import_module("env"), import_name("GetCurrentTime"))) int64_t GetCurrentTime();
