#include <windows.h>
#include <winspool.h>
#include <vector>
#include <string>
#include <fstream>
#include <stdexcept>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static const unsigned char CUT[] = { 0x1D, 0x56, 0x00 };
static const unsigned char DRAWER_1[] = { 0x1B, 0x70, 0x00, 0x32, 0xFA };// cash drawer pin 2
//static const unsigned char DRAWER_2[] = { 0x1B, 0x70, 0x01, 0x32, 0xFA };// cash drawer pin 5

static std::string WStringToUTF8(const std::wstring& wstr)
{
    int size = WideCharToMultiByte(
        CP_UTF8, 0,
        wstr.c_str(), -1,
        nullptr, 0,
        nullptr, nullptr
    );

    if (size <= 0)
        throw std::runtime_error("WideCharToMultiByte failed");

    std::string out(size, 0);

    WideCharToMultiByte(
        CP_UTF8, 0,
        wstr.c_str(), -1,
        &out[0], size,
        nullptr, nullptr
    );

    out.pop_back();
    return out;
}

static std::wstring ReadPrinterName()
{
    std::wifstream f(L"C:\\BillBox\\common\\printer_config.ini");

    if (!f)
        return L"";

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

bool PrintEscPosFromPng(
    const std::wstring& pngPath,
    std::wstring& error
)
{
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

        std::vector<unsigned char> gray(img, img + (w * h));
        stbi_image_free(img);

        for (auto& p : gray)
            p = (p < 210) ? 0 : 255;
        if (w > 560)
        {
            float scale = 560.0f / w;
            int new_w = 560;
            int new_h = (int)(h * scale);

            std::vector<unsigned char> resized(new_w * new_h);

            for (int y = 0; y < new_h; y++)
            {
                for (int x = 0; x < new_w; x++)
                {
                    int src_x = (int)(x / scale);
                    int src_y = (int)(y / scale);

                    resized[y * new_w + x] = gray[src_y * w + src_x];
                }
            }

            gray.swap(resized);
            w = new_w;
            h = new_h;
        }
        int bytes_per_row = (w + 7) / 8;
      //  int bytes_per_row = (w + 7) / 8;

        HANDLE hPrinter = nullptr;

        if (!OpenPrinterW((LPWSTR)printer.c_str(), &hPrinter, nullptr))
            throw std::runtime_error("OpenPrinter failed");

        DOC_INFO_1W doc{};
        doc.pDocName = (LPWSTR)L"POS";
        doc.pDatatype = (LPWSTR)L"RAW";

        if (!StartDocPrinterW(hPrinter, 1, (LPBYTE)&doc))
        {
            ClosePrinter(hPrinter);
            throw std::runtime_error("StartDocPrinter failed");
        }

        StartPagePrinter(hPrinter);

        /* open drawer first */
     //   WriteRaw(hPrinter, DRAWER_1, sizeof(DRAWER_1));

        const int SLICE = 24;

        for (int y = 0; y < h; y += SLICE)
        {
            int slice_h = (y + SLICE <= h) ? SLICE : (h - y);

            unsigned char header[8] =
            {
                0x1D,0x76,0x30,0x00,
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
                {
                    if (gray[(y + row) * w + x] == 0)
                        line[x / 8] |= (0x80 >> (x % 8));
                }

                WriteRaw(hPrinter, line.data(), bytes_per_row);
            }
        }

        unsigned char FEED[] = { 0x1B, 0x64, 8 };

        WriteRaw(hPrinter, FEED, sizeof(FEED));
        WriteRaw(hPrinter, CUT, sizeof(CUT));

        EndPagePrinter(hPrinter);
        EndDocPrinter(hPrinter);
        ClosePrinter(hPrinter);

        return true;
    }
    catch (...)
    {
        error = L"ESC/POS printing failed";
        return false;
    }
}
