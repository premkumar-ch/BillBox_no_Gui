#include <windows.h>
#include <winspool.h>
#include <vector>
#include <string>
#include <fstream>
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <iostream>
#undef min

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static const unsigned char CUT[] = { 0x1D, 0x56, 0x00 };
static const unsigned char DRAWER_1[] = { 0x1B, 0x70, 0x00, 0x32, 0xFA };


static std::string WStringToUTF8(const std::wstring& wstr)
{
    int size = WideCharToMultiByte(CP_UTF8, 0,
        wstr.c_str(), -1,
        nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        throw std::runtime_error("WideCharToMultiByte failed");

    std::string out(size, 0);
    WideCharToMultiByte(CP_UTF8, 0,
        wstr.c_str(), -1,
        &out[0], size, nullptr, nullptr);
    out.pop_back();
    return out;
}

static std::wstring ReadPrinterName()
{
    std::wifstream f(L"C:\\BillBox\\common\\printer_config.ini");
    if (!f) return L"";
    std::wstring name;
    std::getline(f, name);
    return name;
}

static void WriteRaw(HANDLE hPrinter, const void* data, DWORD size)
{
    DWORD written = 0;
    if (!WritePrinter(hPrinter, (LPVOID)data, size, &written) || written != size)
        throw std::runtime_error("WritePrinter failed");
}

// ?? nearest-neighbour resize ??????????????????????????????????????????????????

static std::vector<unsigned char> ResizeNN(
    const std::vector<unsigned char>& src,
    int src_w, int src_h,
    int dst_w, int dst_h)
{
    std::vector<unsigned char> dst(dst_w * dst_h);
    float sx = (float)src_w / dst_w;
    float sy = (float)src_h / dst_h;

    for (int y = 0; y < dst_h; y++)
        for (int x = 0; x < dst_w; x++)
            dst[y * dst_w + x] =
            src[(int)(y * sy) * src_w + (int)(x * sx)];

    return dst;
}


bool PrintEscPosFromPng(const std::wstring& pngPath, std::wstring& error)
{
    HANDLE hPrinter = nullptr;
    bool docStarted = false;
    bool pageStarted = false;

    try
    {
        std::wstring printer = ReadPrinterName();
        if (printer.empty())
            throw std::runtime_error("Printer name empty");

        int w, h, ch;
        std::string path = WStringToUTF8(pngPath);
        unsigned char* img = stbi_load(path.c_str(), &w, &h, &ch, 1);
        if (!img)
            throw std::runtime_error("PNG load failed");

        std::vector<float> pixels(img, img + w * h);
        stbi_image_free(img);

        if (w > 560)
        {
            int new_w = 560;
            int new_h = (int)(h * (560.0f / w));

            std::vector<unsigned char> tmp(pixels.size());
            for (size_t i = 0; i < pixels.size(); i++)
                tmp[i] = (unsigned char)std::clamp(pixels[i], 0.f, 255.f);

            tmp = ResizeNN(tmp, w, h, new_w, new_h);
            w = new_w;
            h = new_h;
            pixels.assign(tmp.begin(), tmp.end());
        }

        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                int   idx = y * w + x;
                float old_val = pixels[idx];
                float new_val = (old_val < 128.f) ? 0.f : 255.f;
                pixels[idx] = new_val;

                float err = old_val - new_val;

                auto spread = [&](int nx, int ny, float weight)   // FIX 1
                    {
                        if (nx >= 0 && nx < w && ny < h)
                        {
                            float& p = pixels[ny * w + nx];
                            p = std::clamp(p + err * weight, 0.f, 255.f); // FIX 2
                        }
                    };

                spread(x + 1, y, 7.f / 16.f);
                spread(x - 1, y + 1, 3.f / 16.f);
                spread(x, y + 1, 5.f / 16.f);
                spread(x + 1, y + 1, 1.f / 16.f);
            }
        }

        // 5. Pack bits
        int bytes_per_row = (w + 7) / 8;

        // 6. Open printer + start doc/page with full error checks
        if (!OpenPrinterW((LPWSTR)printer.c_str(), &hPrinter, nullptr))
            throw std::runtime_error("OpenPrinter failed");

        DOC_INFO_1W doc{};
        doc.pDocName = (LPWSTR)L"POS";
        doc.pDatatype = (LPWSTR)L"RAW";

        if (!StartDocPrinterW(hPrinter, 1, (LPBYTE)&doc))
            throw std::runtime_error("StartDocPrinter failed");
        docStarted = true;

        if (!StartPagePrinter(hPrinter))             // FIX 4
            throw std::runtime_error("StartPagePrinter failed");
        pageStarted = true;

        const int SLICE = 24;

        for (int y = 0; y < h; y += SLICE)
        {
            int slice_h = std::min(SLICE, h - y);

            unsigned char header[8] =
            {
                0x1D, 0x76, 0x30, 0x00,
                (unsigned char)(bytes_per_row & 0xFF),
                (unsigned char)(bytes_per_row >> 8),
                (unsigned char)(slice_h & 0xFF),
                (unsigned char)(slice_h >> 8)
            };
            WriteRaw(hPrinter, header, sizeof(header));

            std::vector<unsigned char> line(bytes_per_row);

            for (int row = 0; row < slice_h; row++)
            {
                std::fill(line.begin(), line.end(), 0);

                for (int x = 0; x < w; x++)
                    if (pixels[(y + row) * w + x] < 128.f)  // FIX 3: was == 0.f
                        line[x / 8] |= (0x80 >> (x % 8));

                WriteRaw(hPrinter, line.data(), bytes_per_row);
            }
        }
        unsigned char FEED[] = { 0x1B, 0x64, 8 };
        WriteRaw(hPrinter, FEED, sizeof(FEED));
        WriteRaw(hPrinter, CUT, sizeof(CUT));

        if (pageStarted) EndPagePrinter(hPrinter);
        if (docStarted)  EndDocPrinter(hPrinter);
        ClosePrinter(hPrinter);
        return true;
    }
    catch (const std::exception& e)
    {
        std::string msg = e.what();
        error = std::wstring(msg.begin(), msg.end());

        if (pageStarted) EndPagePrinter(hPrinter);
        if (docStarted)  EndDocPrinter(hPrinter);
        if (hPrinter)    ClosePrinter(hPrinter);

        return false;
    }
}
