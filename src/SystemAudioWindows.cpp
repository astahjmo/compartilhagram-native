#include "SystemAudio.h"
#include "AudioProcessTree.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonObject>
#include <QtEndian>
#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <atomic>
#include <new>
#include <algorithm>
#include <chrono>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace {
struct Handle {
  HANDLE value = nullptr;
  explicit Handle(HANDLE handle = nullptr) : value(handle) {}
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
};
struct Apartment {
  HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
quint64 creationTime(HANDLE process) {
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user))
    return 0;
  return (quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
AudioProcessTree::Processes processes() {
  AudioProcessTree::Processes result;
  Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (snapshot.value == INVALID_HANDLE_VALUE ||
      !Process32FirstW(snapshot.value, &entry))
    return result;
  do {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                               entry.th32ProcessID));
    wchar_t path[32768];
    DWORD size = DWORD(std::size(path));
    if (!process.value || !QueryFullProcessImageNameW(process.value, 0, path, &size))
      continue;
    auto created = creationTime(process.value);
    if (created)
      result.insert(entry.th32ProcessID,
                    {entry.th32ParentProcessID,
                     QString::fromWCharArray(path, int(size)).toLower(), created});
  } while (Process32NextW(snapshot.value, &entry));
  return result;
}
HRESULT sessions(QSet<quint32> &pids) {
  ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                               CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  if (FAILED(hr)) return hr;
  ComPtr<IMMDeviceCollection> devices;
  hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
  if (FAILED(hr)) return hr;
  UINT count = 0;
  if (FAILED(hr = devices->GetCount(&count))) return hr;
  for (UINT i = 0; i < count; ++i) {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioSessionManager2> manager;
    ComPtr<IAudioSessionEnumerator> listing;
    if (FAILED(hr = devices->Item(i, &device)) ||
        FAILED(hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                     nullptr, &manager)) ||
        FAILED(hr = manager->GetSessionEnumerator(&listing))) return hr;
    int size = 0;
    if (FAILED(hr = listing->GetCount(&size))) return hr;
    for (int j = 0; j < size; ++j) {
      ComPtr<IAudioSessionControl> control;
      ComPtr<IAudioSessionControl2> session;
      DWORD pid = 0;
      AudioSessionState state;
      if (SUCCEEDED(listing->GetSession(j, &control)) &&
          SUCCEEDED(control.As(&session)) &&
          SUCCEEDED(session->GetState(&state)) && state != AudioSessionStateExpired &&
          SUCCEEDED(session->GetProcessId(&pid)) && pid)
        pids.insert(pid);
    }
  }
  return S_OK;
}
// All callback state is consumed only after the completion event is signaled.
// IAgileObject permits delivery on the Windows MTA activation thread.
class Activation final : public IActivateAudioInterfaceCompletionHandler,
                         public IAgileObject {
  std::atomic<ULONG> references_{1};
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid == __uuidof(IUnknown) ||
        iid == __uuidof(IActivateAudioInterfaceCompletionHandler))
      *out = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
    else if (iid == __uuidof(IAgileObject))
      *out = static_cast<IAgileObject *>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    auto remaining = --references_;
    if (!remaining) delete this;
    return remaining;
  }
  Handle complete{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
  ComPtr<IAudioClient> client;
  HRESULT result = E_PENDING;
  HRESULT STDMETHODCALLTYPE ActivateCompleted(
      IActivateAudioInterfaceAsyncOperation *operation) override {
    ComPtr<IUnknown> object;
    HRESULT activation = E_FAIL;
    result = operation->GetActivateResult(&activation, &object);
    if (SUCCEEDED(result)) result = activation;
    if (SUCCEEDED(result)) result = object.As(&client);
    SetEvent(complete.value);
    return S_OK;
  }
};
struct Input {
  Handle process;
  Handle ready{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
  ComPtr<IAudioClient> client;
  ComPtr<IAudioCaptureClient> capture;
  QByteArray pending;
  quint64 created = 0;
  ~Input() { if (client) client->Stop(); }
  HRESULT start(quint32 pid, quint64 expectedCreation, HANDLE stop) {
    process.value = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                FALSE, pid);
    if (!process.value || !ready.value) return HRESULT_FROM_WIN32(GetLastError());
    created = creationTime(process.value);
    if (created != expectedCreation) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    ComPtr<Activation> activation;
    activation.Attach(new (std::nothrow) Activation);
    if (!activation || !activation->complete.value) return E_OUTOFMEMORY;
    activation->parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activation->parameters.ProcessLoopbackParams.TargetProcessId = pid;
    activation->parameters.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT options{};
    options.vt = VT_BLOB;
    options.blob.cbSize = sizeof(activation->parameters);
    options.blob.pBlobData = reinterpret_cast<BYTE *>(&activation->parameters);
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient), &options, activation.Get(), &operation);
    if (FAILED(hr)) return hr;
    HANDLE events[] = {stop, activation->complete.value};
    auto wait = WaitForMultipleObjects(2, events, FALSE, 5000);
    if (wait != WAIT_OBJECT_0 + 1)
      return HRESULT_FROM_WIN32(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_CANCELLED);
    if (FAILED(activation->result)) return activation->result;
    client = activation->client;
    WAVEFORMATEX format{WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0};
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        0, 0, &format, nullptr);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = client->SetEventHandle(ready.value))) return hr;
    if (FAILED(hr = client->GetService(IID_PPV_ARGS(&capture)))) return hr;
    return client->Start();
  }
  HRESULT read() {
    if (WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT)
      return HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
    UINT32 frames = 0;
    HRESULT hr;
    while (SUCCEEDED(hr = capture->GetNextPacketSize(&frames)) && frames) {
      BYTE *data = nullptr;
      DWORD flags = 0;
      if (FAILED(hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
        return hr;
      if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) pending.clear();
      const auto bytes = qsizetype(frames) * 4;
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
        pending.append(QByteArray(bytes, '\0'));
      else
        pending.append(reinterpret_cast<const char *>(data), bytes);
      hr = capture->ReleaseBuffer(frames);
      if (FAILED(hr)) return hr;
      if (pending.size() > 19200) pending.remove(0, pending.size() - 19200);
    }
    return hr;
  }
};
} // namespace

struct SystemAudio::WindowsState {
  Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  std::thread thread;
  ~WindowsState() {
    if (stop.value) SetEvent(stop.value);
    if (thread.joinable()) thread.join();
  }
};
SystemAudio::SystemAudio(QObject *parent) : QObject(parent) {}
SystemAudio::~SystemAudio() { stop(); }
void SystemAudio::stop() {
  windows_.reset();
  ++generation_;
  source_ = nullptr;
  selection_ = {};
}
void SystemAudio::discover() { stop(); launch(); }
void SystemAudio::start(libwebrtc::scoped_refptr<libwebrtc::RTCAudioSource> source,
                        AudioSelection selection) {
  stop();
  source_ = source;
  selection_ = std::move(selection);
  if (selection_.mode != AudioSelection::None) launch();
}
void SystemAudio::launch() {
  windows_ = std::make_unique<WindowsState>();
  if (!windows_->stop.value) {
    emit failed(tr("Não foi possível iniciar a captura de áudio do Windows."));
    return;
  }
  const int generation = generation_;
  const auto selection = selection_;
  const bool recording = source_ != nullptr;
  const HANDLE stopEvent = windows_->stop.value;
  windows_->thread = std::thread([this, generation, selection, recording, stopEvent] {
    auto error = [this, generation](HRESULT hr) {
      QMetaObject::invokeMethod(this, [this, generation, hr] {
        if (generation == generation_)
          emit failed(tr("Falha no áudio por aplicativo do Windows (0x%1). "
                         "A captura requer Windows 11. O áudio completo não será "
                         "usado como alternativa.")
                          .arg(quint32(hr), 8, 16, QLatin1Char('0')));
      }, Qt::QueuedConnection);
    };
    Apartment apartment;
    if (FAILED(apartment.result)) { error(apartment.result); return; }
    std::map<quint32, std::unique_ptr<Input>> inputs;
    auto refresh = std::chrono::steady_clock::time_point::min();
    auto next = std::chrono::steady_clock::now();
    while (WaitForSingleObject(stopEvent, 0) == WAIT_TIMEOUT) {
      auto now = std::chrono::steady_clock::now();
      if (now >= refresh) {
        auto tree = processes();
        QSet<quint32> active;
        HRESULT hr = sessions(active);
        if (FAILED(hr)) { error(hr); return; }
        auto roots = AudioProcessTree::roots(tree, active,
                                             quint32(QCoreApplication::applicationPid()));
        auto accepts = [&](quint32 pid) {
          return selection.accepts(tree[pid].executable) &&
              (selection.mode != AudioSelection::Exclude ||
               !AudioProcessTree::containsExcludedApplication(tree, pid, selection.applications));
        };
        QJsonArray listing;
        QSet<QString> seen;
        for (auto pid : roots) {
          const auto key = tree[pid].executable;
          if (!seen.contains(key)) {
            seen.insert(key);
            listing.append(QJsonObject{{"key", key},
                {"name", QFileInfo(key).fileName() + tr(" (inclui subprocessos)")}});
          }
        }
        QMetaObject::invokeMethod(this, [this, generation, listing] {
          if (generation == generation_) emit applicationsChanged(listing);
        }, Qt::QueuedConnection);
        for (auto it = inputs.begin(); it != inputs.end();) {
          if (!roots.contains(it->first) ||
              tree[it->first].created != it->second->created ||
              !accepts(it->first))
            it = inputs.erase(it);
          else ++it;
        }
        if (recording) for (auto pid : roots) {
          if (!accepts(pid) || inputs.contains(pid)) continue;
          auto input = std::make_unique<Input>();
          hr = input->start(pid, tree[pid].created, stopEvent);
          if (FAILED(hr)) { error(hr); return; }
          inputs.emplace(pid, std::move(input));
        }
        refresh = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      }
      if (recording) {
        QList<QByteArray> buffers;
        for (auto it = inputs.begin(); it != inputs.end();) {
          HRESULT hr = it->second->read();
          if (hr == HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED)) {
            it = inputs.erase(it);
            refresh = std::chrono::steady_clock::time_point::min();
            continue;
          }
          if (FAILED(hr)) { error(hr); return; }
          buffers.append(it->second->pending.left(1920));
          it->second->pending.remove(0, std::min(qsizetype(1920), it->second->pending.size()));
          ++it;
        }
        auto pcm = mixPcm(buffers);
        QMetaObject::invokeMethod(this, [this, generation, pcm] {
          if (generation != generation_ || !source_) return;
          source_->CaptureFrame(pcm.constData(), 16, 48000, 2, 480);
          int peak = 0;
          for (int i = 0; i < 960; ++i)
            peak = std::max(peak, std::abs(int(qFromLittleEndian<int16_t>(pcm.constData() + i * 2))));
          emit levelChanged(double(peak) / 32768.0);
          emit samples(pcm);
        }, Qt::QueuedConnection);
      }
      next += std::chrono::milliseconds(10);
      now = std::chrono::steady_clock::now();
      if (next < now) next = now;
      auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count();
      if (WaitForSingleObject(stopEvent, DWORD(delay)) != WAIT_TIMEOUT) break;
    }
  });
}
