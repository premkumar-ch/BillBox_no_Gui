
#include "esc.h"
#include <algorithm>




namespace fs = std::filesystem;

static inline bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static inline std::wstring Q(const std::wstring& s) {
    return L"\"" + s + L"\"";
}

// RAII guard to restore CWD
struct CwdGuard {
    fs::path oldPath;
    CwdGuard() {
        std::error_code ec;
        oldPath = fs::current_path(ec);
    }
    ~CwdGuard() {
        if (!oldPath.empty()) {
            std::error_code ec;
            fs::current_path(oldPath, ec);
        }
    }
};

static bool RunHidden(const std::wstring& cmd, const std::wstring& workDir, DWORD& exitCode, std::wstring& err) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};

    // CreateProcessW modifies the command string, so we need a writable buffer
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    LPCWSTR cwd = workDir.empty() ? nullptr : workDir.c_str();

    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, cwd, &si, &pi)) {
        err = L"CreateProcessW failed for: " + cmd;
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static std::wstring FindEscImagesPhp() {
    // 1) next to the EXE
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    fs::path exeDir = fs::path(exePath).parent_path();
    fs::path p1 = exeDir / L"escimages.php";
    if (FileExists(p1.wstring())) return p1.wstring();

    // 2) current working directory
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    if (!ec) {
        fs::path p2 = cwd / L"escimages.php";
        if (FileExists(p2.wstring())) return p2.wstring();
    }

    // 3) project path
    fs::path p3 = L"C:\\BillBox\\common\\escpos-tools\\escimages.php";
    if (FileExists(p3.wstring())) return p3.wstring();

    // 4) common BillBox path
    fs::path p4 = L"C:\\BillBox\\common\\escimages.php";
    if (FileExists(p4.wstring())) return p4.wstring();

    return L"";
}

static std::wstring FindExecutable(const std::wstring& name, const std::wstring& fallbackPath) {
    // Check if fallback exists
    if (FileExists(fallbackPath)) return fallbackPath;

    // Try to find in PATH (simplified - just return name and let system find it)
    return name;
}

std::string wstring_to_string(const std::wstring& wstr) {
    if (wstr.empty()) return {};

    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(),
        nullptr, 0, nullptr, nullptr);

    std::string result(size, 0);

    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(),
        result.data(), size, nullptr, nullptr);

    return result;
}
bool cmp(const fs::path& a,const fs::path& b) {
    const std::wstring& sa = a.stem().wstring();
    const std::wstring& sb = b.stem().wstring();

    int na = 0, nb = 0;
    int ra = swscanf(sa.c_str(), L"print-%d", &na);
    int rb = swscanf(sb.c_str(), L"print-%d", &nb);

    if (ra != 1) na = INT_MAX;  // push invalid ones to end
    if (rb != 1) nb = INT_MAX;

    return na < nb;
}
bool ConvertDefaultPrnToPng(std::wstring& outputPngPath, std::wstring& errorMsg) {
    const std::wstring kPrn = L"C:\\BillBox\\Prints\\print.prn";
    const std::wstring kOutputDir = L"C:\\BillBox\\Bills\\";

    // Find executables with fallbacks
    const std::wstring kPhp = FindExecutable(
        L"php.exe",
        L"C:\\BillBox\\common\\php-8.4.12-Win32-vs17-x64\\php.exe"
    );
    const std::wstring kMagick = FindExecutable(
        L"magick.exe",
        L"C:\\Program Files\\ImageMagick-7.1.2-Q16-HDRI\\magick.exe"
    );

    try {
        if (!FileExists(kPrn)) {
            errorMsg = L"Input PRN not found: " + kPrn;
            return false;
        }

        std::wstring script = FindEscImagesPhp();
        if (script.empty()) {
            errorMsg = L"escimages.php not found near EXE/CWD/project/common.";
            return false;
        }

        // Create output directory
        std::error_code mk;
        fs::create_directories(kOutputDir, mk);
        if (mk) {
            errorMsg = L"Failed to create output directory: " + kOutputDir;
            return false;
        }

        // Get absolute paths
        fs::path prnAbs = fs::absolute(kPrn);
        fs::path workDir = prnAbs.parent_path();

        // Use RAII to ensure CWD is restored
        CwdGuard cwdGuard;
        std::error_code cwdEc;
        fs::current_path(workDir, cwdEc);
        if (cwdEc) {
            errorMsg = L"Failed to change to work directory: " + workDir.wstring();
            return false;
        }

        // 1) Run php escimages.php --file "<prn>"
        {
            std::wstring cmd = Q(kPhp) + L" " + Q(script) + L" --file " + Q(prnAbs.wstring());
            DWORD code = 0;
            if (!RunHidden(cmd, workDir.wstring(), code, errorMsg)) {
                return false;
            }
            if (code != 0) {
                errorMsg = L"escimages.php failed (exit " + std::to_wstring(code) + L")";
                return false;
            }
        }

        // 2) Collect PNG pages using filesystem iterator
        std::vector<fs::path> pages;
        std::wstring stemPattern = prnAbs.stem().wstring() + L"-";
        
        for (const auto& entry : fs::directory_iterator(workDir, cwdEc)) {
            if (cwdEc) continue;

            if (entry.is_regular_file() && entry.path().extension() == L".png") {
                std::wstring filename = entry.path().filename().wstring();
                // Check for stem-*.png pattern
                if (filename.find(stemPattern) == 0) {
                    pages.push_back(entry.path().filename());
                }
            }
        }

        // Fallback to print-*.png if no stem matches found
        if (pages.empty()) {
            for (const auto& entry : fs::directory_iterator(workDir, cwdEc)) {
                if (cwdEc) continue;

                if (entry.is_regular_file() && entry.path().extension() == L".png") {
                    std::wstring filename = entry.path().filename().wstring();
                    if (filename.find(L"print-") == 0) {
                        pages.push_back(entry.path().filename());
                    }
                }
            }
        }

        if (pages.empty()) {
            errorMsg = L"No PNG pages generated by escimages.php";
            return false;
        }

        // Sort pages alphabetically
        /*
        std::sort(pages.begin(), pages.end(),
            [](const fs::path& a, const fs::path& b) {
                return a.wstring() < b.wstring();
            });
            */
        std::sort(pages.begin(), pages.end(), cmp);
        // 3) Create output path
        fs::path outAbs = fs::path(kOutputDir) / L"res.png";

        // Remove existing file
        std::error_code delEc;
        fs::remove(outAbs, delEc);

        // 4) Run magick <pages...> -append "<outAbs>"
        {
            std::wstring cmd = Q(kMagick);
            for (const auto& p : pages) {
                cmd += L" " + Q((workDir / p).wstring());
            }
            cmd += L" -append " + Q(outAbs.wstring());

            DWORD code = 0;
            if (!RunHidden(cmd, workDir.wstring(), code, errorMsg)) {
                return false;
            }
            if (code != 0) {
                errorMsg = L"ImageMagick failed (exit " + std::to_wstring(code) + L")";
                return false;
            }
            if (!FileExists(outAbs.wstring())) {
                errorMsg = L"Output PNG not created: " + outAbs.wstring();
                return false;
            }
        }

        // 5) Cleanup intermediate files
        for (const auto& p : pages) {
            std::error_code del;
            fs::remove(workDir / p, del);
        }

        outputPngPath = outAbs.wstring();
        return true;

    }
    catch (const std::exception& e) {
        errorMsg = L"Exception: " + std::wstring(e.what(), e.what() + strlen(e.what()));
        return false;
    }
    catch (...) {
        errorMsg = L"Unknown exception occurred";
        return false;
    }
}
