// Analisi dell'hardware per scegliere il modello: memoria, processori, GPU.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#else
#include <fstream>
#include <sstream>
#endif
#include "ai/Assistant.h"
#include <string>
#include <thread>

namespace cad::ai {

#ifdef _WIN32
namespace {
std::string Utf8(const wchar_t* w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}
}  // namespace
#endif

HardwareInfo DetectHardware() {
  HardwareInfo hw;
  hw.threads = (int)std::max(1u, std::thread::hardware_concurrency());
#ifdef _WIN32
  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof(ms);
  if (GlobalMemoryStatusEx(&ms)) hw.ramGB = double(ms.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
  IDXGIFactory1* f = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f))) && f) {
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
      DXGI_ADAPTER_DESC1 d{};
      if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
        GpuInfo g;
        g.name = Utf8(d.Description);
        g.vramGB = double(d.DedicatedVideoMemory) / (1024.0 * 1024.0 * 1024.0);
        // NVIDIA (0x10DE) e AMD (0x1002) con almeno 1 GB dedicato: scheda discreta
        g.discrete = (d.VendorId == 0x10DE || d.VendorId == 0x1002) && g.vramGB >= 1.0;
        hw.gpus.push_back(g);
      }
      a->Release();
    }
    f->Release();
  }
#elif defined(__APPLE__)
  int64_t mem = 0;
  size_t len = sizeof(mem);
  if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0) hw.ramGB = double(mem) / (1024.0 * 1024.0 * 1024.0);
#if defined(__aarch64__) || defined(__arm64__)
  hw.unifiedMemory = true;
  hw.gpus.push_back({"Apple Silicon (Metal)", 0, false});
#endif
#else
  std::ifstream in("/proc/meminfo");
  for (std::string l; std::getline(in, l);)
    if (l.rfind("MemTotal:", 0) == 0) { std::istringstream s(l.substr(9)); double kb = 0; s >> kb; hw.ramGB = kb / (1024.0 * 1024.0); }
#endif
  return hw;
}

}  // namespace cad::ai
