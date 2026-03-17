    #include <windows.h>
    #include <thread>
    #include <chrono>
    #include <ctime>
    #include <string>
    #include <sstream>
    #include <filesystem>
    #include <fstream>
    #include <winspool.h>
    #include <vector>
    #include <shlwapi.h>
    #include <iostream>
    #include <codecvt>
    #define CURL_STATICLIB
    #include <curl/curl.h>

    #include "esc.h"
    #include "escpos_print.h"

    namespace fs = std::filesystem;

    std::wstring generatedPdfPath;
    std::wstring conversionError;
    bool conversionDone = false;

    static const std::wstring kBillsDir = L"C:\\BillBox\\Bills\\";
    static const std::wstring kTempPdf = kBillsDir + L"TEMP_CONVERTED.pdf";
    static const std::wstring kResPng = kBillsDir + L"res.png";
    static const std::wstring kDefaultPrn = L"C:\\BillBox\\Prints\\print.prn";

    struct Prem_def {
        bool curl_status;
        std::string response;
        std::string error;
    };

    std::string WStringToUTF8(const std::wstring& wstr) {
        if (wstr.empty()) return {};
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string str(size_needed - 1, 0); // exclude null terminator
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], size_needed, nullptr, nullptr);
        return str;
    }

    static void PrintNow(const char* tag) {
        SYSTEMTIME st;
        GetLocalTime(&st);

        /*
        std::cout
            << "[TIME] " << tag << " -> "
            << st.wHour << ":"
            << st.wMinute << ":"
            << st.wSecond << "."
            << st.wMilliseconds
            << std::endl;
            */
    }
    static size_t read_callback(void* ptr, size_t size, size_t nmemb, void* stream) {
        FILE* file = static_cast<FILE*>(stream);
        return fread(ptr, size, nmemb, file);
    }

    Prem_def UploadPDF(const std::wstring& filePathW,
        const std::wstring& mobileW,
        const std::wstring& newDirW,
        const std::wstring& filenameW,
        const std::string& format,
        std::string& responseOut,
        std::string errorOut)
    {
        FILE* file = _wfopen(filePathW.c_str(), L"rb");
        if (!file) {
            errorOut = "Failed to open file.";
            return { false,responseOut,errorOut };
        }

        CURL* curl = curl_easy_init();
        if (!curl) {
            errorOut ="Failed to initialize CURL";
            fclose(file);
           // return false;
            return { false,responseOut,errorOut };
        }

        std::string mobile = WStringToUTF8(mobileW);
        std::string newDir = WStringToUTF8(newDirW);
        std::string filename = WStringToUTF8(filenameW);

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/pdf");
        std::string mobileHeader = "x-mobile: " + mobile;
        std::string dirHeader = "x-new-dir: " + newDir;
        std::string filenameHeader = "x-filename: " + filename;
        std::string json = "x-format: " + format;
        headers = curl_slist_append(headers, mobileHeader.c_str());
        headers = curl_slist_append(headers, dirHeader.c_str());
        headers = curl_slist_append(headers, filenameHeader.c_str());
        headers = curl_slist_append(headers, json.c_str());

        fseek(file, 0, SEEK_END);
        curl_off_t fsize = ftell(file);
        fseek(file, 0, SEEK_SET);

        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, "https://api.billbox.co.in/upload-bill");
     //   curl_easy_setopt(curl, CURLOPT_URL, "https://hxgmir9iw6.execute-api.ap-south-2.amazonaws.com/test/upload-bill");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);
        curl_easy_setopt(curl, CURLOPT_READDATA, file);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, fsize);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
            +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
                size_t total = size * nmemb;
                std::string* resp = static_cast<std::string*>(userdata);
                resp->append(ptr, total);
                return total;
            });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        CURLcode res = curl_easy_perform(curl);
        bool success = false;

        if (res == CURLE_OK) {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            if (http_code >= 200 && http_code < 300) {
                success = true;
                responseOut = response;
            }
            else {
                errorOut = "Server returned HTTP " + std::to_string(http_code) + ": " + response;
            }
        }
        else {
            errorOut = "Upload failed: " + std::string(curl_easy_strerror(res));
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        fclose(file);
        return { true,responseOut,errorOut };
      //  return success;
    }


    static inline bool FileExistsW(const std::wstring& p) {
        DWORD a = GetFileAttributesW(p.c_str());
        return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }



    std::wstring ReadStringFromIniFile(const std::wstring& filePath) {
        std::wifstream file(filePath);

        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file");
        }

        std::wstring content;
        std::wstring line;

        // Read all lines and concatenate them
        while (std::getline(file, line)) {
            // Trim whitespace from beginning and end
            size_t start = line.find_first_not_of(L" \t\r\n");
            size_t end = line.find_last_not_of(L" \t\r\n");

            if (start != std::wstring::npos && end != std::wstring::npos) {
                content += line.substr(start, end - start + 1);
            }
        }

        file.close();

        return content;
    }

    bool Convert() {
        PrintNow("Convert.exe start");
        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);

        std::string command = R"(C:\BillBox\common\convert.exe C:\BillBox\Bills\res.png C:\BillBox\Bills\TEMP_CONVERTED.pdf)";
        std::vector<char> cmd(command.begin(), command.end());
        cmd.push_back('\0');

        BOOL success = CreateProcessA(
            nullptr, cmd.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi
        );

        if (!success) {
            conversionError = L"Failed to start convert.exe.";
            return false;
        }

        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        generatedPdfPath = kTempPdf;
        PrintNow("Convert.exe end");
        return true;
    }
    /*
    void BackgroundConvert() {
        PrintNow("BG: Start");
        std::wstring error, out;

        if (!ConvertDefaultPrnToPng(out, error)) {
            conversionError = L"PRN → PNG conversion failed.";
            conversionDone = true;
            return;
        }

        if (!Convert()) {
            conversionError = L"PNG → PDF conversion failed.";
            conversionDone = true;
            return;
        }

        conversionDone = true;
        PrintNow("BG: End");
    } */

    bool Print(std::wstring& error) {
        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);

        std::string command = R"(C:\BillBox\common\printing.exe)";
        std::vector<char> cmd(command.begin(), command.end());
        cmd.push_back('\0');

        BOOL success = CreateProcessA(
            nullptr, cmd.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi
        );

        if (!success) {
            error = L"Failed to start printing.exe.";
            return false;
        }

     //   WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    }
    #include <windows.h>

    void launch_detached(const wchar_t* exePath)
    {
        STARTUPINFO si{};
        PROCESS_INFORMATION pi{};

        si.cb = sizeof(si);

        BOOL ok = CreateProcessW(
            exePath,       
            nullptr,        
            nullptr, nullptr,
            FALSE,
            CREATE_NO_WINDOW | DETACHED_PROCESS,
            nullptr,
            nullptr,
            &si,
            &pi
        );

        if (ok) {
        
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }
    
    void OpenCashDrawer(const std::wstring& printer)
    {
        HANDLE hPrinter;

        LPWSTR p = const_cast<LPWSTR>(printer.c_str());

        if (!OpenPrinterW(p, &hPrinter, NULL))
            return;

        DOC_INFO_1W doc{};
        doc.pDocName = (LPWSTR)L"Drawer";
        doc.pDatatype = (LPWSTR)L"RAW";

        if (!StartDocPrinterW(hPrinter, 1, (LPBYTE)&doc))
        {
            ClosePrinter(hPrinter);
            return;
        }

        StartPagePrinter(hPrinter);

        unsigned char cmd[] = { 0x1B,0x70,0x00,0x19,0xFA };

        DWORD written;
        WritePrinter(hPrinter, cmd, sizeof(cmd), &written);

        EndPagePrinter(hPrinter);
        EndDocPrinter(hPrinter);
        ClosePrinter(hPrinter);
    }
    int WINAPI wWinMain(
        HINSTANCE hInstance,
        HINSTANCE hPrevInstance,
        PWSTR     pCmdLine,
        int       nCmdShow
    )
 //   int main(   )

    {

        bool openDrawer = false;

        if (pCmdLine && wcslen(pCmdLine) > 0)
        {
            if (pCmdLine[0] == L'1')
                openDrawer = true;
        }
        std::wstring png_error, png_out;
        std::wstring error;
        bool png_convert = ConvertDefaultPrnToPng(png_out, png_error);
    
        if (!png_convert) {
            MessageBox(nullptr, L"Conversion prn to png Failed", L"Error", MB_OK | MB_ICONERROR);
            return 1;
        }
        bool printing = PrintEscPosFromPng(kResPng, error);
    //    bool printing = Print(error);
        if (!printing) {
            MessageBox(nullptr, error.data(), L"Error", MB_OK | MB_ICONERROR);
            return 1;
        }

        if (openDrawer)
        {
            std::wstring printer = ReadStringFromIniFile(L"C:\\BillBox\\common\\printer_config.ini");
            OpenCashDrawer(printer);
        }
        bool pdf_convert = Convert();
        if (!pdf_convert) {
            MessageBox(nullptr, L"Conversion from png to pdf failed", L"Error", MB_OK | MB_ICONERROR);
            return 1;
        }
        std::string response,_error;
        std::wstring store_id = ReadStringFromIniFile(L"C:\\BillBox\\common\\store_id.ini");

        Prem_def curr_status = UploadPDF(
            L"C:\\BillBox\\Bills\\TEMP_CONVERTED.pdf",
            L"customer_no",
            store_id,
            L".pdf",
            "JSON",
            response,
            _error
        );
        
        if (curr_status.curl_status != false) {
            std::cout << "Sucess upload\n";
            std::cout << "Response: "<<response;
        }
        else {
            std::cout << "Failed: \n";
            std::cout << "Response: " << response;
            std::cout << "\n error: " << _error;
        }
        return 0;
    }
