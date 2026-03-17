#pragma once
#include <iostream>
#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <ctime>
#include <fstream>
#include <codecvt>

bool ConvertDefaultPrnToPng(std::wstring& outputPngPath, std::wstring& errorMsg);
