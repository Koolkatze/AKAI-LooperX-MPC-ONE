// vsthost.h - host VST2 mínimo para probar varios plugins LooperX en el MISMO proceso (como el MPC).
// Usa la ABI independiente del SDK (framework/mpc_vst2_abi.h). Solo para pruebas en el PC.
#pragma once
#include <dlfcn.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "mpc_vst2_abi.h"

namespace th {

struct Transport {
    bool playing = false;
    double bpm = 120.0, ppq = 0.0, sr = 44100.0, samplePos = 0.0;
    int num = 4, den = 4;
};
inline Transport g_tr;
inline VstTimeInfo g_ti;
inline std::atomic<int> g_updateDisplay{0}, g_automate{0};

inline VstIntPtr hostCb(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void* ptr, float)
{
    switch (op) {
        case audioMasterVersion: return 2400;
        case audioMasterGetTime: {
            std::memset(&g_ti, 0, sizeof g_ti);
            g_ti.sampleRate = g_tr.sr;
            g_ti.samplePos = g_tr.samplePos;
            g_ti.tempo = g_tr.bpm;
            g_ti.ppqPos = g_tr.ppq;
            double bar = g_tr.num * 4.0 / g_tr.den;
            g_ti.barStartPos = std::floor(g_tr.ppq / bar) * bar;
            g_ti.timeSigNumerator = g_tr.num;
            g_ti.timeSigDenominator = g_tr.den;
            g_ti.flags = kVstTempoValid | kVstPpqPosValid | kVstBarsValid | kVstTimeSigValid |
                         (g_tr.playing ? kVstTransportPlaying : 0);
            return (VstIntPtr)&g_ti;
        }
        case audioMasterUpdateDisplay: ++g_updateDisplay; return 1;
        case audioMasterAutomate: ++g_automate; return 0;
        case audioMasterGetSampleRate: return (VstIntPtr)g_tr.sr;
        case audioMasterGetBlockSize: return 128;
        case audioMasterGetVendorString: if (ptr) std::strcpy((char*)ptr, "Akai"); return 1;
        case audioMasterGetProductString: if (ptr) std::strcpy((char*)ptr, "MPC 3"); return 1;
        case audioMasterCanDo: return 1;
        default: return 0;
    }
}

class Plug {
public:
    explicit Plug(const std::string& path) {
        lib_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!lib_) { std::fprintf(stderr, "dlopen %s: %s\n", path.c_str(), dlerror()); return; }
        using MainFn = AEffect* (*)(audioMasterCallback);
        auto fn = (MainFn)dlsym(lib_, "VSTPluginMain");
        if (!fn) return;
        e_ = fn(hostCb);
    }
    ~Plug() { close(); }
    bool ok() const { return e_ != nullptr; }
    AEffect* e() { return e_; }
    VstIntPtr disp(VstInt32 op, VstInt32 idx = 0, VstIntPtr v = 0, void* p = nullptr, float o = 0) { return e_->dispatcher(e_, op, idx, v, p, o); }
    void prepare(double sr = 44100, int block = 128) {
        disp(effOpen);
        disp(effSetSampleRate, 0, 0, nullptr, (float)sr);
        disp(effSetBlockSize, 0, block);
        disp(effMainsChanged, 0, 1);
        disp(effStartProcess);
        block_ = block;
    }
    void close() {
        if (e_) { disp(effStopProcess); disp(effMainsChanged, 0, 0); disp(effClose); e_ = nullptr; }
    }
    int numParams() const { return e_->numParams; }
    float get(int i) { return e_->getParameter(e_, i); }
    void set(int i, float v) { e_->setParameter(e_, i, v); }
    // Pulsar un botón on/off (como tocar la pantalla del MPC)
    void press(int i) { set(i, get(i) < 0.5f ? 1.f : 0.f); }
    std::string text(int i) { char b[64] = {}; disp(effGetParamDisplay, i, 0, b); return b; }
    std::string name(int i) { char b[64] = {}; disp(effGetParamName, i, 0, b); return b; }
    int find(const std::string& n) { for (int i = 0; i < numParams(); ++i) if (name(i) == n) return i; return -1; }
    std::vector<uint8_t> chunk() { void* p = nullptr; VstIntPtr n = disp(effGetChunk, 0, 0, &p); return n > 0 && p ? std::vector<uint8_t>((uint8_t*)p, (uint8_t*)p + n) : std::vector<uint8_t>(); }
    void setChunk(std::vector<uint8_t>& c) { disp(effSetChunk, 0, (VstIntPtr)c.size(), c.data()); }
    void midi(const std::vector<std::vector<uint8_t>>& msgs) {
        evs_.assign(msgs.size(), VstMidiEvent{});
        std::vector<char> buf(sizeof(VstEvents) + sizeof(VstEvent*) * (msgs.size() + 2));
        auto* ve = (VstEvents*)buf.data();
        ve->numEvents = (int)msgs.size();
        for (size_t k = 0; k < msgs.size(); ++k) {
            evs_[k].type = kVstMidiType; evs_[k].byteSize = sizeof(VstMidiEvent);
            for (size_t j = 0; j < 3 && j < msgs[k].size(); ++j) evs_[k].midiData[j] = (char)msgs[k][j];
            ve->events[k] = (VstEvent*)&evs_[k];
        }
        disp(effProcessEvents, 0, 0, ve);
    }
    // Procesa n frames en el sitio (como el MPC): io[0], io[1] son entrada y salida.
    void process(float* L, float* R, int n) {
        float* io[2] = {L, R};
        e_->processReplacing(e_, io, io, n);
    }
private:
    void* lib_ = nullptr;
    AEffect* e_ = nullptr;
    int block_ = 128;
    std::vector<VstMidiEvent> evs_;
};

}  // namespace th
