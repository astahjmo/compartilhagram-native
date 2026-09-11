// Standalone WASAPI playback fixture. The integration test copies it under two
// names so each tone has a distinct application identity. No microphone is used.
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const double frequency = std::atof(argv[1]);
  if (frequency <= 0 || frequency >= 24000) return 2;
  auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr)) return 3;
  auto play = [&]() -> HRESULT {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    HRESULT result;
    if (FAILED(result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return result;
    if (FAILED(result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
      return result;
    if (FAILED(result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                         nullptr, &client))) return result;
    WAVEFORMATEX format{WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0};
    if (FAILED(result = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 1000000, 0, &format, nullptr))) return result;
    if (FAILED(result = client->GetService(IID_PPV_ARGS(&render)))) return result;
    UINT32 capacity;
    if (FAILED(result = client->GetBufferSize(&capacity))) return result;
    if (FAILED(result = client->Start())) return result;
    std::puts("READY");
    std::fflush(stdout);
    uint64_t sample = 0;
    auto end = GetTickCount64() + 30000;
    while (GetTickCount64() < end) {
      UINT32 padding;
      if (FAILED(result = client->GetCurrentPadding(&padding))) return result;
      UINT32 frames = capacity - padding;
      if (frames) {
        BYTE *data;
        if (FAILED(result = render->GetBuffer(frames, &data))) return result;
        auto pcm = reinterpret_cast<int16_t *>(data);
        for (UINT32 i = 0; i < frames; ++i, ++sample) {
          auto value = int16_t(6000 * std::sin(2 * 3.141592653589793 * frequency * sample / 48000));
          pcm[i * 2] = pcm[i * 2 + 1] = value;
        }
        if (FAILED(result = render->ReleaseBuffer(frames, 0))) return result;
      }
      Sleep(5);
    }
    return client->Stop();
  };
  hr = play();
  CoUninitialize();
  if (FAILED(hr)) std::fprintf(stderr, "WASAPI error: 0x%08lx\n", (unsigned long)hr);
  return FAILED(hr) ? 1 : 0;
}
