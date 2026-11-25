// LooperXGranular.cpp - LooperX Granular: sintetizador granular para el MPC (instrumento).
//
// SPDX-License-Identifier: MIT
//
// Fuente del sonido ("Source"):
//   FILE   - un WAV: los de fábrica (Content/Samples), los tuyos en <unidad>/LooperX/Samples y los
//            loops exportados por LooperX Looper en <unidad>/LooperX/Loops (SD/USB y disco interno).
//   LOOPER - un slot vivo de LooperX Looper (LOOP 1..16): se granula mientras el loop suena e
//            incluso mientras se sobregraba. Usa el enlace en memoria compartida (lx_link.h), que
//            no forma parte del contrato documentado del SDK de Akai; si no hay Looper, "NO LOOPER".
//
// Se toca por MIDI: pads del MPC (con aftertouch polifónico), teclado, o cualquier controlador
// externo enrutado a la pista: nota, velocidad, aftertouch, pitch bend, rueda de modulación (CC1),
// sustain (CC64), All Notes Off (CC123) y All Sound Off (CC120).
// "Keys" = PITCH (la nota cambia el tono, Root Key suena original) o POSITION (cada nota elige una
// de 16 zonas del sonido: los 16 pads de un banco recorren el sample).
//
// Para grabar lo que tocas como un loop nuevo sin parar los demás: manda esta pista a un submix
// que tenga LooperX Looper como insert y pulsa un LOOP vacío (ver README).
//
// Reglas del SDK: los WAV se leen en un hilo propio (nunca en process()); el audio solo cambia un
// puntero ya preparado. Parámetro 0 = Gain (el host lo "empuja" al preparar).

#include "mpc_plugin.h"
#include "dsp/wav.h"

#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../_shared/lx_granular.h"
#include "../../_shared/lx_link.h"

using namespace lx;

namespace {

constexpr int kMaxFiles = 128;
constexpr double kMaxSeconds = 90.0;   // límite por WAV (≈ 32 MB en memoria)

bool isDir(const std::string& p) { struct stat st {}; return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }

bool hasWavExt(const std::string& n)
{
    if (n.size() < 5 || n[0] == '.') return false;
    std::string e = n.substr(n.size() - 4);
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e == ".wav";
}

std::string pluginDriveRoot()
{
    std::string f = mpc::pluginFolder();
    std::string parent = f.substr(0, f.find_last_of('/'));
    if (parent.size() >= 12 && parent.compare(parent.size() - 12, 12, "VST2 Plugins") == 0)
        return parent.substr(0, parent.find_last_of('/'));
    return "";
}

std::string internalDriveRoot()
{
    std::ifstream m("/proc/mounts");
    std::string dev, mnt, rest;
    while (m >> dev >> mnt && std::getline(m, rest))
        if (mnt.rfind("/media/", 0) == 0 && mnt.find("internal") != std::string::npos) return mnt;
    if (isDir("/media/az01-internal")) return "/media/az01-internal";
    return "";
}

const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
std::string noteName(int n)   // convención del MPC: 60 = C3
{
    char b[12];
    std::snprintf(b, sizeof b, "%s%d", kNoteNames[((n % 12) + 12) % 12], n / 12 - 2);
    return b;
}

struct FileEntry
{
    std::string key;    // "F/<rel>" fábrica, "D/<rel>" unidad del plugin, "I/<rel>" disco interno
    std::string path;   // ruta completa
    std::string label;  // nombre corto para la pantalla
};

struct Sample
{
    std::vector<float> data;   // estéreo intercalado a 44,1 kHz (o la frecuencia del host)
    int64_t frames = 0;
    std::string key;
    bool cut = false;
};

uintptr_t selfThread() { return (uintptr_t)pthread_self(); }

}  // namespace

class LooperXGranular : public mpc::Plugin
{
public:
    enum Params {
        kGain, kSource, kFile, kSlot, kRescan, kPosition, kScan, kSpray, kSize, kDensity, kPitch, kFine,
        kPitchRnd, kSpread, kReverse, kWindow, kAttack, kDecay, kSustain, kRelease, kCutoff, kReso, kFreeze,
        kLfoRate, kLfoPos, kVoices, kRootKey, kBendRange, kAtTarget, kModTarget, kKeys, kVelocity, kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Granular";
        i.vendor = "LooperX";
        i.uniqueId = MPC_FOURCC('L', 'x', 'G', 'r');
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        i.tailSamples = 44100 * 10;
        return i;
    }

    LooperXGranular() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam("gain", "Gain", -36.0f, 6.0f, -4.0f, "dB", 1));
        addParam(choiceParam("src", "Source", {"FILE", "LOOPER"}, 0));
        addParam(intParam("file", "File", 0, kMaxFiles - 1, 0));
        addParam(intParam("slot", "Slot", 1, kSlots, 1));
        addParam(boolParam("rescan", "Rescan", false));
        addParam(floatParam("pos", "Position", 0.0f, 1.0f, 0.3f, "", 3));
        addParam(floatParam("scan", "Scan", -2.0f, 2.0f, 0.0f, "x", 2));
        addParam(floatParam("spray", "Spray", 0.0f, 1000.0f, 40.0f, "ms", 0, 0.5f));
        addParam(logParam("size", "Size", 5.0f, 1000.0f, 90.0f, "ms", 0));
        addParam(logParam("dens", "Density", 1.0f, 200.0f, 24.0f, "/s", 1));
        addParam(intParam("pitch", "Pitch", -24, 24, 0, "st"));
        addParam(floatParam("fine", "Fine", -100.0f, 100.0f, 0.0f, "ct", 0));
        addParam(floatParam("prnd", "Pitch Rnd", 0.0f, 12.0f, 0.0f, "st", 2, 0.5f));
        addParam(floatParam("spread", "Spread", 0.0f, 1.0f, 0.6f, "", 2));
        addParam(floatParam("rev", "Reverse", 0.0f, 1.0f, 0.0f, "", 2));
        addParam(choiceParam("win", "Window", {"HANN", "TRI", "TUKEY", "PERC", "SWELL"}, 0));
        addParam(logParam("att", "Attack", 1.0f, 5000.0f, 30.0f, "ms", 0));
        addParam(logParam("dec", "Decay", 1.0f, 5000.0f, 300.0f, "ms", 0));
        addParam(floatParam("sus", "Sustain", 0.0f, 1.0f, 0.8f, "", 2));
        addParam(logParam("rel", "Release", 1.0f, 10000.0f, 800.0f, "ms", 0));
        addParam(logParam("cut", "Cutoff", 20.0f, 20000.0f, 12000.0f, "Hz", 0));
        addParam(floatParam("res", "Reso", 0.0f, 1.0f, 0.15f, "", 2));
        addParam(boolParam("frz", "Freeze", false));
        addParam(logParam("lfor", "LFO Rate", 0.01f, 20.0f, 0.2f, "Hz", 2));
        addParam(floatParam("lfop", "LFO>Pos", 0.0f, 1.0f, 0.0f, "", 2));
        addParam(intParam("voices", "Voices", 1, GranularEngine::kMaxVoices, 8));
        addParam(intParam("root", "Root Key", 24, 96, 60));
        addParam(intParam("bend", "Bend Rng", 0, 24, 2, "st"));
        addParam(choiceParam("at", "AT>", {"OFF", "DENSITY", "SIZE", "CUTOFF"}, 1));
        addParam(choiceParam("mod", "Mod>", {"OFF", "POSITION", "SPRAY", "SIZE", "CUTOFF", "LFO>POS"}, 1));
        addParam(choiceParam("keys", "Keys", {"PITCH", "POSITION"}, 0));
        addParam(floatParam("vel", "Velocity", 0.0f, 1.0f, 1.0f, "", 2));
        if (numParams() != kNumParams) mpc::log("LooperX Granular: parameter count mismatch");

        addPrograms();

        shared_ = openSharedLink();
        scanFiles();                      // rápido (solo lista carpetas)
        // El primer WAV se carga ya aquí (permitido en el constructor) para que suene a la primera;
        // los cambios posteriores los carga el hilo de trabajo.
        {
            std::lock_guard<std::mutex> lk(filesMu_);
            int i = getInt(kFile);
            if (i >= 0 && i < (int)files_.size()) { cur_ = loadSample(files_[(size_t)i]); if (cur_) initialKey_ = cur_->key; }
        }
        lastRescan_ = get(kRescan);
        worker_ = std::thread([this] { workerLoop(); });
    }

    ~LooperXGranular() override
    {
        quit_.store(true);
        if (worker_.joinable()) worker_.join();
        delete pending_.exchange(nullptr);
        delete retired_.exchange(nullptr);
        delete cur_;
        closeSharedLink(shared_);
    }

    void prepare(double sampleRate, int) override
    {
        engine_.prepare(sampleRate);
        hostRate_.store(sampleRate);
    }

    void reset() override { engine_.allSoundOff(); }

    void parameterChanged(int index, float plain) override
    {
        if (index == kRescan && plain != lastRescan_) {
            lastRescan_ = plain;
            if (selfThread() == audioThread_.load(std::memory_order_relaxed)) rescanReq_.store(true);
        }
    }

    void process(mpc::ProcessContext& ctx) override
    {
        audioThread_.store(selfThread(), std::memory_order_relaxed);
        // WAV nuevo preparado por el hilo: se cambia el puntero (sin liberar nada aquí).
        if (!retired_.load(std::memory_order_acquire)) {
            if (Sample* s = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
                retired_.store(cur_, std::memory_order_release);
                cur_ = s;
                curKey_.store(++curGen_, std::memory_order_relaxed);
            }
        }
        fillParams();

        // Fuente
        GrainSrc src;
        const bool looperSrc = getInt(kSource) == 1;
        const bool alive = alive_.update(shared_, ctx.numFrames, ctx.sampleRate);
        looperAlive_.store(alive, std::memory_order_relaxed);
        PoolReadGuard guard(looperSrc && alive ? shared_ : nullptr);
        if (looperSrc) {
            if (alive && guard.base()) {
                const LinkSlot& sl = shared_->slot[mpc::clamp(getInt(kSlot), 1, kSlots) - 1];
                const int st = sl.state.load(std::memory_order_acquire);
                int64_t len = sl.lengthFrames.load(std::memory_order_acquire);
                if (len <= 0 && st == kRecording) len = sl.recFrames.load(std::memory_order_acquire);
                len = std::min<int64_t>(len, (int64_t)kMaxChunksPerSlot * kChunkFrames);
                if (st != kEmpty && len > 0) { src.type = GrainSrc::kLink; src.len = len; src.slot = &sl; src.guard = &guard; }
            }
        } else if (cur_ && cur_->frames > 0) {
            src.type = GrainSrc::kBuffer; src.buf = cur_->data.data(); src.len = cur_->frames;
        }

        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : scratch_;
        int pos = 0, ev = 0;
        const int n = ctx.numFrames;
        while (pos < n) {
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= pos) handleMidi(ctx.midi[ev++]);
            int end = n;
            if (ev < ctx.numMidiEvents) end = std::max(pos + 1, std::min(n, ctx.midi[ev].sampleOffset));
            end = std::min(end, pos + GranularEngine::kMaxBlock);
            engine_.render(outL + pos, outR + pos, end - pos, gp_, src);
            pos = end;
        }
        while (ev < ctx.numMidiEvents) handleMidi(ctx.midi[ev++]);
        if (ctx.numOutputs <= 1) for (int i = 0; i < n; ++i) outL[i] = 0.5f * (outL[i] + outR[i]);

        // Firma de lo que se ve en pantalla (para refrescar el texto de Slot/File)
        uint32_t sig = (uint32_t)getInt(kSlot) * 131u + (alive ? 7u : 0u) + (uint32_t)curGen_ * 977u;
        if (looperSrc && shared_) sig += 13u * (uint32_t)shared_->slot[mpc::clamp(getInt(kSlot), 1, kSlots) - 1].state.load(std::memory_order_relaxed);
        if (sig != lastSig_) { lastSig_ = sig; displayDirty_.store(true, std::memory_order_relaxed); }
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char b[64];
        if (index == kFile) {
            std::lock_guard<std::mutex> lk(filesMu_);
            if (files_.empty()) { out = "NO FILES"; return true; }
            int i = mpc::clamp((int)std::lround(plain), 0, kMaxFiles - 1);
            if (i >= (int)files_.size()) { std::snprintf(b, sizeof b, "%d: -", i + 1); out = b; return true; }
            out = files_[(size_t)i].label;
            return true;
        }
        if (index == kSlot) {
            int s = mpc::clamp((int)std::lround(plain), 1, kSlots);
            if (!shared_ || !looperAlive_.load(std::memory_order_relaxed)) { std::snprintf(b, sizeof b, "L%d NO LOOPER", s); out = b; return true; }
            std::snprintf(b, sizeof b, "L%d %s", s, slotStateName(shared_->slot[s - 1].state.load(std::memory_order_relaxed)));
            out = b; return true;
        }
        if (index == kRescan) {
            std::lock_guard<std::mutex> lk(filesMu_);
            std::snprintf(b, sizeof b, "%d FILES", (int)files_.size());
            out = b; return true;
        }
        if (index == kRootKey) { out = noteName((int)std::lround(plain)); return true; }
        if (index == kPosition || index == kSpread || index == kReverse || index == kSustain || index == kReso ||
            index == kLfoPos || index == kVelocity) {
            std::snprintf(b, sizeof b, "%.0f%%", plain * 100.0); out = b; return true;
        }
        if (index == kCutoff) {
            if (plain >= 1000.0f) std::snprintf(b, sizeof b, "%.1f kHz", plain / 1000.0);
            else std::snprintf(b, sizeof b, "%.0f Hz", plain);
            out = b; return true;
        }
        if (index == kSize || index == kAttack || index == kDecay || index == kRelease) {
            if (plain >= 1000.0f) std::snprintf(b, sizeof b, "%.2f s", plain / 1000.0);
            else std::snprintf(b, sizeof b, "%.0f ms", plain);
            out = b; return true;
        }
        return false;
    }

    // Estado extra: qué WAV es (por nombre), para encontrarlo aunque cambie la lista de la tarjeta.
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        std::string key;
        {
            std::lock_guard<std::mutex> lk(filesMu_);
            if (resolvePending_) key = savedKey_;
            else {
                int i = getInt(kFile);
                if (i >= 0 && i < (int)files_.size()) key = files_[(size_t)i].key;
                else key = savedKey_;
            }
        }
        const char magic[4] = {'L', 'X', 'G', 'R'};
        out.insert(out.end(), magic, magic + 4);
        uint32_t ver = 1, len = (uint32_t)key.size();
        out.insert(out.end(), (uint8_t*)&ver, (uint8_t*)&ver + 4);
        out.insert(out.end(), (uint8_t*)&len, (uint8_t*)&len + 4);
        out.insert(out.end(), key.begin(), key.end());
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        if (size < 12 || std::memcmp(data, "LXGR", 4) != 0) return;
        uint32_t len = 0;
        std::memcpy(&len, data + 8, 4);
        if (len > size - 12 || len > 4096) return;
        std::lock_guard<std::mutex> lk(filesMu_);
        savedKey_.assign((const char*)data + 12, len);
        resolvePending_ = true;
    }

private:
    void addPrograms()
    {
        std::vector<float> d((size_t)kNumParams);
        for (int i = 0; i < kNumParams; ++i) d[(size_t)i] = param(i).def;
        addProgram("Init Cloud", d);
        auto v = d;
        v[kSize] = 30; v[kDensity] = 80; v[kSpray] = 5; v[kPitchRnd] = 0; v[kWindow] = 3; v[kAttack] = 2; v[kRelease] = 300;
        v[kSustain] = 0.6f; v[kKeys] = 1; v[kFile] = 6;
        addProgram("Pad Slicer", v);
        v = d;
        v[kSize] = 400; v[kDensity] = 12; v[kSpray] = 300; v[kPitchRnd] = 0.15f; v[kSpread] = 1; v[kAttack] = 1200;
        v[kRelease] = 4000; v[kCutoff] = 5000; v[kFreeze] = 1; v[kLfoRate] = 0.05f; v[kLfoPos] = 0.3f;
        addProgram("Frozen Drone", v);
        v = d;
        v[kSize] = 60; v[kDensity] = 120; v[kSpray] = 600; v[kPitchRnd] = 12; v[kReverse] = 0.5f; v[kSpread] = 1;
        v[kAttack] = 5; v[kRelease] = 1500; v[kFile] = 4;
        addProgram("Glitter Spray", v);
        v = d;
        v[kSource] = 1; v[kSlot] = 1; v[kSize] = 120; v[kDensity] = 30; v[kSpray] = 80; v[kScan] = 1; v[kAttack] = 10;
        addProgram("Live Loop 1", v);
    }

    void fillParams()
    {
        GrainParams& p = gp_;
        p.position = get(kPosition); p.scan = get(kScan); p.sprayMs = get(kSpray); p.sizeMs = get(kSize);
        p.density = get(kDensity); p.pitch = get(kPitch); p.fine = get(kFine); p.pitchRand = get(kPitchRnd);
        p.spread = get(kSpread); p.reverse = get(kReverse); p.window = getInt(kWindow);
        p.attackMs = get(kAttack); p.decayMs = get(kDecay); p.sustain = get(kSustain); p.releaseMs = get(kRelease);
        p.cutoff = get(kCutoff); p.reso = get(kReso); p.gainDb = get(kGain); p.freeze = getBool(kFreeze);
        p.lfoRate = get(kLfoRate); p.lfoPos = get(kLfoPos); p.voices = getInt(kVoices); p.rootKey = getInt(kRootKey);
        p.bendRange = getInt(kBendRange); p.atTarget = getInt(kAtTarget); p.modTarget = getInt(kModTarget);
        p.keyMode = getInt(kKeys); p.velAmount = get(kVelocity);
    }

    void handleMidi(const mpc::MidiEvent& e)
    {
        const uint8_t st = e.status();
        if (e.isNoteOn()) engine_.noteOn(e.data[1], (float)e.data[2] / 127.0f, gp_);
        else if (e.isNoteOff()) engine_.noteOff(e.data[1]);
        else if (st == 0xA0) engine_.polyPressure(e.data[1], (float)e.data[2] / 127.0f);
        else if (st == 0xD0) engine_.channelPressure((float)e.data[1] / 127.0f);
        else if (st == 0xE0) engine_.setBend((float)(((int)e.data[2] << 7 | e.data[1]) - 8192) / 8192.0f);
        else if (st == 0xB0) {
            switch (e.data[1]) {
                case 1: engine_.setMod((float)e.data[2] / 127.0f); break;
                case 64: engine_.setSustain(e.data[2] >= 64); break;
                case 120: engine_.allSoundOff(); break;
                case 121: engine_.setBend(0); engine_.setMod(0); engine_.setSustain(false); break;
                case 123: engine_.allNotesOff(); break;
                default: break;
            }
        }
    }

    // ---- Lista de WAV (constructor y hilo de trabajo; nunca el audio) ----------------------
    static void listWavs(const std::string& dir, const std::string& rel, const std::string& tag, int depth,
                         std::vector<FileEntry>& out)
    {
        if (depth > 2 || (int)out.size() >= kMaxFiles) return;
        DIR* d = opendir(dir.c_str());
        if (!d) return;
        std::vector<std::string> names;
        while (dirent* de = readdir(d)) if (de->d_name[0] != '.') names.push_back(de->d_name);
        closedir(d);
        std::sort(names.begin(), names.end());
        for (const auto& n : names) {
            std::string full = dir + "/" + n, r = rel.empty() ? n : rel + "/" + n;
            if (isDir(full)) { listWavs(full, r, tag, depth + 1, out); continue; }
            if (!hasWavExt(n) || (int)out.size() >= kMaxFiles) continue;
            FileEntry fe;
            fe.key = tag + "/" + r; fe.path = full;
            std::string base = n.substr(0, n.size() - 4);
            if (base.rfind("LooperX_", 0) == 0) base = base.substr(8);   // exportaciones del Looper
            fe.label = (tag == "F" ? "" : tag == "D" ? "SD:" : "INT:") + base;
            if (fe.label.size() > 22) fe.label = fe.label.substr(0, 22);
            out.push_back(fe);
        }
    }

    void scanFiles()
    {
        std::vector<FileEntry> list;
        listWavs(mpc::contentPath("Samples"), "", "F", 0, list);
        if (const char* env = std::getenv("LOOPERX_SAMPLES_ROOT")) {   // pruebas en el PC
            listWavs(std::string(env) + "/LooperX/Samples", "Samples", "D", 0, list);
            listWavs(std::string(env) + "/LooperX/Loops", "Loops", "D", 0, list);
        }
        std::string drive = pluginDriveRoot(), internal = internalDriveRoot();
        if (!drive.empty()) {
            listWavs(drive + "/LooperX/Samples", "Samples", "D", 0, list);
            listWavs(drive + "/LooperX/Loops", "Loops", "D", 0, list);
        }
        if (!internal.empty() && internal != drive) {
            listWavs(internal + "/LooperX/Samples", "Samples", "I", 0, list);
            listWavs(internal + "/LooperX/Loops", "Loops", "I", 0, list);
        }
        std::lock_guard<std::mutex> lk(filesMu_);
        files_.swap(list);
    }

    Sample* loadSample(const FileEntry& fe)
    {
        dsp::WavData w;
        if (!dsp::loadWav(fe.path, w) || w.numFrames() < 16 || w.numChannels() < 1) return nullptr;
        const double outRate = hostRate_.load() > 0 ? hostRate_.load() : 44100.0;
        const double ratio = w.sampleRate / outRate;
        int64_t inFrames = (int64_t)w.numFrames();
        int64_t outFrames = (int64_t)((double)inFrames / ratio);
        auto* s = new Sample();
        s->key = fe.key;
        const int64_t cap = (int64_t)(kMaxSeconds * outRate);
        if (outFrames > cap) { outFrames = cap; s->cut = true; }
        s->data.resize((size_t)outFrames * 2);
        const std::vector<float>& L = w.channels[0];
        const std::vector<float>& R = w.channels[w.numChannels() > 1 ? 1 : 0];
        for (int64_t i = 0; i < outFrames; ++i) {
            double x = (double)i * ratio;
            int64_t i0 = (int64_t)x;
            int64_t i1 = std::min(i0 + 1, inFrames - 1);
            float f = (float)(x - (double)i0);
            s->data[(size_t)i * 2] = L[(size_t)i0] + (L[(size_t)i1] - L[(size_t)i0]) * f;
            s->data[(size_t)i * 2 + 1] = R[(size_t)i0] + (R[(size_t)i1] - R[(size_t)i0]) * f;
        }
        s->frames = outFrames;
        return s;
    }

    void workerLoop()
    {
        std::string loadedKey = initialKey_;
        auto lastUpdate = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        while (!quit_.load()) {
            if (rescanReq_.exchange(false)) {
                std::string keep;
                { std::lock_guard<std::mutex> lk(filesMu_); int i = getInt(kFile); if (i >= 0 && i < (int)files_.size()) keep = files_[(size_t)i].key; }
                scanFiles();
                if (!keep.empty()) relocate(keep);
                displayDirty_.store(true);
            }
            // Proyecto recién abierto: busca el WAV guardado por su nombre.
            bool pending;
            std::string want;
            { std::lock_guard<std::mutex> lk(filesMu_); pending = resolvePending_; want = savedKey_; }
            if (pending) {
                if (!want.empty()) relocate(want);
                std::lock_guard<std::mutex> lk(filesMu_);
                resolvePending_ = false;
            }
            // Carga el WAV elegido si ha cambiado.
            FileEntry fe;
            bool have = false;
            {
                std::lock_guard<std::mutex> lk(filesMu_);
                int i = getInt(kFile);
                if (i >= 0 && i < (int)files_.size()) { fe = files_[(size_t)i]; have = true; }
            }
            if (have && fe.key != loadedKey) {
                Sample* s = loadSample(fe);
                loadedKey = fe.key;
                if (s) delete pending_.exchange(s, std::memory_order_acq_rel);
            }
            delete retired_.exchange(nullptr, std::memory_order_acq_rel);

            auto now = std::chrono::steady_clock::now();
            if (displayDirty_.load() && now - lastUpdate > std::chrono::milliseconds(400)) {
                displayDirty_.store(false);
                lastUpdate = now;
                requestDisplayUpdate();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    }

    // Pone el parámetro File apuntando a `key` si existe en la lista (hilo de trabajo).
    void relocate(const std::string& key)
    {
        int found = -1;
        {
            std::lock_guard<std::mutex> lk(filesMu_);
            for (size_t i = 0; i < files_.size(); ++i) if (files_[i].key == key) { found = (int)i; break; }
        }
        if (found >= 0 && found != getInt(kFile)) {
            set(kFile, (float)found);
            notifyHostParameterChanged(kFile);
            displayDirty_.store(true);
        }
    }

    GranularEngine engine_;
    GrainParams gp_;
    float scratch_[8192] = {};

    LinkBlock* shared_ = nullptr;
    LooperAliveWatch alive_;
    std::atomic<bool> looperAlive_{false};

    std::mutex filesMu_;
    std::vector<FileEntry> files_;
    std::string savedKey_, initialKey_;
    bool resolvePending_ = false;

    Sample* cur_ = nullptr;                    // solo el hilo de audio
    std::atomic<Sample*> pending_{nullptr};    // hilo → audio
    std::atomic<Sample*> retired_{nullptr};    // audio → hilo (para liberar)
    uint32_t curGen_ = 0;
    std::atomic<uint32_t> curKey_{0};

    std::atomic<double> hostRate_{44100.0};
    std::atomic<uintptr_t> audioThread_{0};
    std::atomic<bool> rescanReq_{false}, displayDirty_{false}, quit_{false};
    float lastRescan_ = 0;
    uint32_t lastSig_ = 0;
    std::thread worker_;
};

MPC_PLUGIN_ENTRY(LooperXGranular)
