// LooperXLooper.cpp - LooperX Looper: looper de pedal para el MPC (efecto de inserción).
//
// SPDX-License-Identifier: MIT
//
// Se pone como INSERT en una pista de audio que recibe la entrada (Input 1/2 con monitor), en un
// submix (para loopear lo que entra en él, p. ej. el sinte LooperX Granular) o en el máster.
//
// Flujo de señal:
//   entrada ──┬── (Monitor) ───────────────────────────────┐
//             └── grabación/overdub → 16 slots de loop ─(VOL n)─┴─ (Output) → salida
//
// Cada slot es un pedal: botón LOOP (vacío → espera compás → REC → PLAY → DUB → PLAY...),
// STOP (parar/arrancar cuantizado) y CLEAR (dos pulsaciones). Todo se cuantiza a los compases
// del MPC (o a tiempo, o libre), con largo AUTO o fijo en tiempos (4/4, 4/8, 4/16, 4/24...).
// Los loops se guardan dentro del proyecto del MPC y se pueden exportar como WAV (24 bits) a la
// SD/USB o al disco interno del MPC.
//
// Con "Link Pads" activado, LooperX Pads (instrumento) dispara estos slots con los pads y
// LooperX Granular puede usar estos loops como fuente (memoria compartida en el proceso; ver
// _shared/lx_link.h; no forma parte del contrato documentado del SDK).
//
// Reglas del SDK que se cumplen: nada de memoria, locks, ficheros ni logs en process(); el
// parámetro 0 es el volumen de salida (el host lo "empuja" al cargar); los botones son
// parámetros on/off y una pulsación es cualquier cambio hecho desde la pantalla o un Q-Link
// (llegan por el hilo de audio, según host-contract.md); los cambios por carga de proyecto o
// automatización (hilo de UI) no disparan acciones.

#include "mpc_plugin.h"

#include <pthread.h>
#include <sys/stat.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "../../_shared/lx_link.h"
#include "../../_shared/lx_looper.h"
#include "../../_shared/lx_wavwrite.h"

using namespace lx;

namespace {

const int kMemMB[5] = {16, 32, 64, 128, 256};

uintptr_t selfThread() { return (uintptr_t)pthread_self(); }

std::string dbText(float gain)
{
    if (gain <= 0.0005f) return "-inf dB";
    char b[24];
    std::snprintf(b, sizeof b, "%+.1f dB", 20.0 * std::log10((double)gain));
    return b;
}

bool isDir(const std::string& p)
{
    struct stat st {};
    return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void mkdirs(const std::string& path)
{
    std::string cur;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        cur = path.substr(0, j);
        if (!cur.empty()) ::mkdir(cur.c_str(), 0775);
        i = j + 1;
    }
}

// Unidad donde está el plugin: ".../<unidad>/VST2 Plugins/<carpeta del plugin>"
std::string pluginDriveRoot()
{
    std::string f = mpc::pluginFolder();
    std::string parent = f.substr(0, f.find_last_of('/'));
    if (parent.size() >= 12 && parent.compare(parent.size() - 12, 12, "VST2 Plugins") == 0)
        return parent.substr(0, parent.find_last_of('/'));
    return parent;
}

// Disco interno del MPC: un montaje bajo /media cuyo nombre contenga "internal".
std::string internalDriveRoot()
{
    std::ifstream m("/proc/mounts");
    std::string dev, mnt, rest;
    while (m >> dev >> mnt && std::getline(m, rest))
        if (mnt.rfind("/media/", 0) == 0 && mnt.find("internal") != std::string::npos) return mnt;
    if (isDir("/media/az01-internal")) return "/media/az01-internal";
    return "";
}

// Primera unidad extraíble montada (SD/USB) que no sea el disco interno.
std::string removableDriveRoot()
{
    std::string root = pluginDriveRoot();
    if (root.find("internal") == std::string::npos && isDir(root)) return root;
    std::ifstream m("/proc/mounts");
    std::string dev, mnt, rest;
    while (m >> dev >> mnt && std::getline(m, rest))
        if (mnt.rfind("/media/", 0) == 0 && mnt.find("internal") == std::string::npos) return mnt;
    return "";
}

}  // namespace

class LooperXLooper : public mpc::Plugin
{
public:
    enum Params {
        kOutput, kMonitor, kLength, kQuantize, kAfterRec, kDubLevel, kLoopMem, kStopAll, kClearAll, kUndo,
        kExport, kExportTo, kLiveStatus, kLink, kLatePress, kSync, kMpcStop, kSlotBase
    };
    enum SlotParam { kLoop = 0, kStop = 1, kClear = 2, kVol = 3, kPerSlot = 4 };
    static int slotParam(int s, int which) { return kSlotBase + s * kPerSlot + which; }
    static constexpr int kNumParams = kSlotBase + kSlots * kPerSlot;

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Looper";
        i.vendor = "LooperX";
        i.uniqueId = MPC_FOURCC('L', 'x', 'L', 'p');
        i.version = 1000;
        i.isInstrument = false;
        i.numInputs = 2;
        i.numOutputs = 2;
        i.tailSamples = 1;
        return i;
    }

    LooperXLooper() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam("out", "Output", 0.0f, 1.25f, 1.0f, "", 2));
        addParam(floatParam("mon", "Monitor", 0.0f, 1.0f, 1.0f, "", 2));
        addParam(intParam("len", "Length", 0, 128, 0));
        addParam(choiceParam("qnt", "Quantize", {"Bar", "Beat", "Free"}, 0));
        addParam(choiceParam("after", "After Rec", {"Play", "Dub"}, 0));
        addParam(floatParam("dub", "Dub Level", 0.5f, 1.0f, 1.0f, "", 2));
        addParam(choiceParam("mem", "Loop Mem", {"16 MB", "32 MB", "64 MB", "128 MB", "256 MB"}, 1));
        addParam(boolParam("stopall", "Stop All", false));
        addParam(boolParam("clrall", "Clear All", false));
        addParam(boolParam("undo", "Undo", false));
        addParam(boolParam("export", "Export WAV", false));
        addParam(choiceParam("dest", "Export To", {"SD/USB", "MPC ONE"}, 0));
        addParam(boolParam("live", "Live Status", true));
        addParam(boolParam("link", "Link Pads", true));
        addParam(intParam("late", "Late Press", 0, 250, 80, "ms"));
        addParam(choiceParam("sync", "Sync", {"MPC", "Free"}, 0));
        addParam(choiceParam("mpcstop", "MPC Stop", {"Stops", "Keeps"}, 0));
        for (int s = 0; s < kSlots; ++s) {
            char id[16], nm[16];
            std::snprintf(id, sizeof id, "loop%d", s + 1); std::snprintf(nm, sizeof nm, "LOOP %d", s + 1);
            addParam(boolParam(id, nm, false));
            std::snprintf(id, sizeof id, "stop%d", s + 1); std::snprintf(nm, sizeof nm, "STOP %d", s + 1);
            addParam(boolParam(id, nm, false));
            std::snprintf(id, sizeof id, "clr%d", s + 1); std::snprintf(nm, sizeof nm, "CLEAR %d", s + 1);
            addParam(boolParam(id, nm, false));
            std::snprintf(id, sizeof id, "vol%d", s + 1); std::snprintf(nm, sizeof nm, "VOL %d", s + 1);
            addParam(floatParam(id, nm, 0.0f, 1.25f, 1.0f, "", 2));
        }
        if (numParams() != kNumParams) mpc::log("LooperX Looper: parameter count mismatch");
        for (int i = 0; i < kNumParams; ++i) { lastPlain_[i].store(get(i)); pressCount_[i].store(0); pressSeen_[i] = 0; }

        addProgram("Default", defaults());

        token_ = (uint32_t)((uintptr_t)this ^ (uint32_t)std::time(nullptr) ^ 0x5A5A1234u) | 1u;
        privateLink_ = newPrivateLink();
        shared_ = openSharedLink();
        link_ = privateLink_;
        // Pool inicial (fuera del audio). Si el proyecto trae otro tamaño, se cambia al cargar.
        memMB_.store(kMemMB[1]);
        pool_ = new ChunkPool();
        if (!pool_->create((size_t)kMemMB[1] << 20)) { delete pool_; pool_ = new ChunkPool(); pool_->create((size_t)8 << 20); memMB_.store(8); }
        core_.attach(link_, pool_);
        core_.useSettings(&set_);
        publishPool(link_, pool_);
        tryLink(true);
        worker_ = std::thread([this] { workerLoop(); });
    }

    ~LooperXLooper() override
    {
        quit_.store(true);
        if (worker_.joinable()) worker_.join();
        releaseShared(true);
        unpublishAndDrain(privateLink_);
        if (ChunkPool* r = retire_.exchange(nullptr)) { delete r; }
        if (ChunkPool* p = pendingPool_.exchange(nullptr)) { delete p; }
        if (Staged* st = staged_.exchange(nullptr)) { delete st->pool; delete st; }
        delete pool_;
        freePrivateLink(privateLink_);
        closeSharedLink(shared_);
    }

    std::vector<float> defaults() const
    {
        std::vector<float> v((size_t)kNumParams);
        for (int i = 0; i < kNumParams; ++i) v[(size_t)i] = param(i).def;
        return v;
    }

    void prepare(double sampleRate, int) override
    {
        core_.prepare(sampleRate);
    }

    void reset() override {}

    // Pulsaciones: cualquier cambio de un botón hecho desde la pantalla o un Q-Link (hilo de audio).
    void parameterChanged(int index, float plain) override
    {
        if (index < 0 || index >= kNumParams) return;
        float prev = lastPlain_[index].exchange(plain);
        if (index == kLoopMem) memRequest_.store(kMemMB[mpc::clamp((int)std::lround(plain), 0, 4)]);
        if (!isButton(index) || prev == plain) return;
        if (selfThread() != audioThread_.load(std::memory_order_relaxed)) return;  // carga/automatización
        pressCount_[index].fetch_add(1, std::memory_order_relaxed);
    }

    void process(mpc::ProcessContext& ctx) override
    {
        audioThread_.store(selfThread(), std::memory_order_relaxed);
        const int n = ctx.numFrames;
        installPendingState();
        maybeSwitchLink();
        fillSettings();
        core_.useSettings(&set_);
        if (discardPresses_.exchange(false, std::memory_order_acq_rel))
            for (int i = 0; i < kNumParams; ++i) pressSeen_[i] = pressCount_[i].load(std::memory_order_relaxed);
        consumePresses();
        consumeLinkCommands();

        HostTime ht;
        const mpc::Transport& t = ctx.transport;
        ht.valid = t.valid; ht.playing = t.playing; ht.tempoValid = t.tempoValid; ht.ppqValid = t.ppqValid;
        ht.bpm = t.bpm; ht.ppq = t.ppqPos; ht.barStartPpq = t.barStartPpq;
        ht.num = t.timeSigNumerator; ht.den = t.timeSigDenominator;
        numerator_.store(t.valid && t.timeSigNumerator > 0 ? t.timeSigNumerator : 4, std::memory_order_relaxed);

        const float* inL = ctx.numInputs > 0 && ctx.inputs ? ctx.inputs[0] : nullptr;
        const float* inR = ctx.numInputs > 1 && ctx.inputs ? ctx.inputs[1] : inL;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : ctx.outputs[0];
        // En el sitio: el núcleo lee cada frame antes de escribirlo.
        for (int off = 0; off < n; off += kBlock) {
            int m = std::min(kBlock, n - off);
            const float* a = inL ? inL + off : silence_;
            const float* b = inR ? inR + off : silence_;
            core_.process(a, b, outL + off, outR + off, m, ht, set_);
            ht.ppq += (double)m * (ht.bpm > 1 ? ht.bpm : core_.bpm()) / 60.0 / sampleRate();
        }
        if (pool_) freeSecs_.store((int)pool_->secondsFree(sampleRate()), std::memory_order_relaxed);
        if (core_.memoryFullFlag()) memFull_.store(true);
        // ¿Ha cambiado algo visible? (para refrescar la pantalla desde el hilo de trabajo)
        uint32_t sig = 0;
        for (int s = 0; s < kSlots; ++s)
            sig = sig * 31u + (uint32_t)core_.state(s) * 7u + (uint32_t)core_.pending(s) + (core_.clearArmed(s) ? 1000u : 0u);
        sig = sig * 31u + (core_.clearAllArmed() ? 1u : 0u) + (uint32_t)exportState_.load() * 3u;
        if (sig != lastSig_) { lastSig_ = sig; displayDirty_.store(true, std::memory_order_release); }
    }

    // ---- Texto de cada control (hilo de UI) -------------------------------------------------
    bool formatValue(int index, float plain, std::string& out) override
    {
        char b[48];
        if (index == kOutput || index == kMonitor || (index >= kSlotBase && (index - kSlotBase) % kPerSlot == kVol)) {
            out = dbText(plain); return true;
        }
        if (index == kDubLevel) { std::snprintf(b, sizeof b, "%.0f%%", plain * 100.0); out = b; return true; }
        if (index == kLength) {
            // Notación del usuario: "4/N" = N negras (4/4 = 1 compás de 4/4, 4/16 = 4 compases).
            // Texto fijo (no depende del compás) para que coincida con los pasos de la rueda del skin.
            int beats = (int)std::lround(plain);
            if (beats <= 0) { out = "AUTO"; return true; }
            std::snprintf(b, sizeof b, "4/%d", beats);
            out = b; return true;
        }
        if (index == kLatePress) { std::snprintf(b, sizeof b, "%d ms", (int)std::lround(plain)); out = b; return true; }
        if (index == kLoopMem) {
            int mb = kMemMB[mpc::clamp((int)std::lround(plain), 0, 4)];
            if (mb != memMB_.load()) std::snprintf(b, sizeof b, "%dMB*", mb);   // * = al vaciar los loops
            else std::snprintf(b, sizeof b, "%dMB %ds", mb, freeSecs_.load(std::memory_order_relaxed));
            out = b; return true;
        }
        if (index == kStopAll) { out = "STOP ALL"; return true; }
        if (index == kClearAll) { out = core_.clearAllArmed() ? "SURE?" : "CLEAR ALL"; return true; }
        if (index == kUndo) {
            int s = core_.lastSlot();
            if (s >= 0 && core_.hasUndo(s)) std::snprintf(b, sizeof b, "UNDO %d", s + 1);
            else std::snprintf(b, sizeof b, "UNDO");
            out = b; return true;
        }
        if (index == kExport) {
            static const char* st[] = {"EXPORT", "SAVING", "DONE", "ERROR", "NO DRIVE", "NO LOOPS"};
            out = st[mpc::clamp(exportState_.load(), 0, 5)]; return true;
        }
        if (index == kLink) {
            if (plain < 0.5f) out = "OFF";
            else if (linkedShared_.load()) out = "LINKED";
            else out = shared_ ? "BUSY" : "NO LINK";
            return true;
        }
        if (index == kLiveStatus) { out = plain >= 0.5f ? "On" : "Off"; return true; }
        if (index >= kSlotBase) {
            int s = (index - kSlotBase) / kPerSlot, which = (index - kSlotBase) % kPerSlot;
            int x = core_.state(s), pend = core_.pending(s);
            double bar = core_.barBeats() > 0 ? core_.barBeats() : 4.0;
            double barFrames = bar / (core_.bpm() / 60.0) * sampleRate();
            if (which == kLoop) {
                if (core_.clearArmed(s)) { out = "CLEAR?"; return true; }
                switch (x) {
                    case kEmpty: out = "EMPTY"; break;
                    case kArmed: out = "WAIT"; break;
                    case kRecording:
                        std::snprintf(b, sizeof b, "REC %.1f", barFrames > 0 ? core_.recFrames(s) / barFrames : 0.0);
                        out = b; break;
                    default: {
                        double bars = core_.lengthBeats(s) / bar;
                        const char* w = x == kOverdub ? "DUB" : x == kStopped ? "STOP" : "PLAY";
                        if (pend == LooperCore::kStartPlay) w = ">PLAY";
                        if (pend == LooperCore::kStopPlay) w = ">STOP";
                        if (std::fabs(bars - std::round(bars)) < 0.01) std::snprintf(b, sizeof b, "%s %.0fb", w, bars);
                        else std::snprintf(b, sizeof b, "%s %.1fb", w, bars);
                        out = b;
                    }
                }
                if (x == kRecording && pend == LooperCore::kCloseRec) out = "REC>|";
                return true;
            }
            if (which == kStop) { out = x == kEmpty || x == kArmed ? "-" : x == kStopped ? "START" : "STOP"; return true; }
            if (which == kClear) { out = x == kEmpty ? "-" : core_.clearArmed(s) ? "SURE?" : "CLEAR"; return true; }
        }
        return false;
    }

    // ---- Estado del proyecto: los loops viajan dentro del proyecto del MPC --------------------
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        // Todo lo de aquí corre en el hilo de UI; el audio no toca estos vectores.
        if (staged_.load(std::memory_order_acquire) && !loadedRaw_.empty()) { out = loadedRaw_; return; }
        uint64_t sig = stateSignature();
        if (!loadedRaw_.empty() && installEpoch_.load() == loadedEpoch_ && sig == installSig_.load()) { out = loadedRaw_; return; }
        if (!cache_.empty() && sig == cacheSig_) { out = cache_; return; }
        out.clear();
        put(out, "LXLP", 4);
        putU32(out, 1);
        putU32(out, kSlots);
        LinkBlock* l = link_;
        PoolReadGuard g(l);
        size_t budget = (size_t)100 << 20;  // tope prudente dentro de los 128 MB del host
        for (int s = 0; s < kSlots; ++s) {
            LinkSlot& L = l->slot[s];
            int x = L.state.load();
            int len = L.lengthFrames.load();
            bool present = (x == kPlaying || x == kOverdub || x == kStopped) && len > 0 && g.base() &&
                           (size_t)len * 4 + 64 <= budget;
            out.push_back(present ? 1 : 0);
            if (!present) continue;
            budget -= (size_t)len * 4;
            out.push_back((uint8_t)(x == kStopped ? kStopped : kPlaying));
            putU32(out, (uint32_t)len);
            putF64(out, (double)L.lengthBeats.load());
            putF64(out, core_.recBpm(s));
            float peak = 1e-9f;
            for (int i = 0; i < len; ++i) {
                const float* f = slotFrame(g, L, i);
                if (f) peak = std::max(peak, std::max(std::fabs(f[0]), std::fabs(f[1])));
            }
            float scale = std::max(peak, 1.0f / 32767.0f);
            putF32(out, scale);
            size_t pos = out.size();
            out.resize(pos + (size_t)len * 4);
            uint8_t* d = out.data() + pos;
            for (int i = 0; i < len; ++i) {
                const float* f = slotFrame(g, L, i);
                for (int c = 0; c < 2; ++c) {
                    float v = f ? f[c] / scale : 0.f;
                    int16_t q = (int16_t)mpc::clamp((int)std::lrint(v * 32767.0f), -32767, 32767);
                    *d++ = (uint8_t)(q & 0xFF);
                    *d++ = (uint8_t)((q >> 8) & 0xFF);
                }
            }
        }
        cache_ = out;
        cacheSig_ = sig;
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        // Los parámetros del proyecto ya se han escrito: lo que parezca una "pulsación" en esta carga
        // no lo es (en el host_sim todo corre en un hilo); se descarta antes del siguiente bloque.
        discardPresses_.store(true, std::memory_order_release);
        auto* st = new Staged();
        loadedRaw_.assign(data, data + size);
        loadedEpoch_ = ++loadCounter_;
        st->epoch = loadedEpoch_;
        size_t pos = 0;
        auto need = [&](size_t k) { return pos + k <= size; };
        if (!need(12) || std::memcmp(data, "LXLP", 4) != 0) { delete st; return; }
        pos = 4;
        uint32_t ver = getU32(data, pos), slots = getU32(data, pos);
        if (ver != 1 || slots > (uint32_t)kSlots) { delete st; return; }
        // Pool nuevo para el proyecto: tamaño elegido, o mayor si los loops no caben.
        size_t totalFrames = 0;
        {
            size_t p2 = pos;
            for (uint32_t s = 0; s < slots && p2 < size; ++s) {
                uint8_t present = data[p2++];
                if (!present) continue;
                if (p2 + 1 + 4 + 8 + 8 + 4 > size) break;
                p2 += 1;
                uint32_t len = getU32(data, p2);
                p2 += 8 + 8 + 4;
                totalFrames += len;
                p2 += (size_t)len * 4;
            }
        }
        int wantMB = kMemMB[mpc::clamp(getInt(kLoopMem), 0, 4)];
        size_t needBytes = (totalFrames / kChunkFrames + 2 * kSlots + 4) * (size_t)kChunkFrames * 8;
        size_t bytes = std::max((size_t)wantMB << 20, needBytes);
        st->pool = new ChunkPool();
        if (!st->pool->create(bytes)) { delete st->pool; delete st; return; }
        st->mb = (int)(bytes >> 20);
        for (uint32_t s = 0; s < slots; ++s) {
            if (!need(1)) break;
            uint8_t present = data[pos++];
            if (!present) continue;
            if (!need(1 + 4 + 8 + 8 + 4)) break;
            StagedSlot& ss = st->slot[s];
            ss.state = data[pos++];
            uint32_t len = getU32(data, pos);
            ss.lengthBeats = getF64(data, pos);
            ss.recBpm = getF64(data, pos);
            float scale = getF32(data, pos);
            if (!need((size_t)len * 4) || len == 0) break;
            int chunks = (int)((len - 1) / kChunkFrames + 1);
            if (chunks > kMaxChunksPerSlot) { pos += (size_t)len * 4; continue; }
            bool ok = true;
            for (int c = 0; c < chunks; ++c) {
                uint32_t idx = st->pool->alloc();
                if (idx == kNoChunk) { ok = false; break; }
                std::memset(st->pool->ptr(idx), 0, sizeof(float) * (size_t)kChunkFrames * 2);
                ss.chunks.push_back(idx);
            }
            if (!ok) { pos += (size_t)len * 4; ss.chunks.clear(); continue; }
            for (uint32_t i = 0; i < len; ++i) {
                float* f = st->pool->ptr(ss.chunks[i >> kChunkShift]) + ((i & kChunkMask) << 1);
                for (int c = 0; c < 2; ++c) {
                    int16_t q = (int16_t)(data[pos] | (data[pos + 1] << 8));
                    pos += 2;
                    f[c] = (float)q / 32767.0f * scale;
                }
            }
            ss.present = true;
            ss.lengthFrames = (int)len;
        }
        if (Staged* old = staged_.exchange(st, std::memory_order_acq_rel)) { delete old->pool; delete old; }
        cache_.clear();
    }

private:
    static constexpr int kBlock = 512;
    struct StagedSlot { bool present = false; int state = kPlaying; int lengthFrames = 0; double lengthBeats = 0, recBpm = 120; std::vector<uint32_t> chunks; };
    struct Staged { ChunkPool* pool = nullptr; int mb = 32; uint32_t epoch = 0; StagedSlot slot[kSlots]; };

    static bool isButton(int i)
    {
        if (i == kStopAll || i == kClearAll || i == kUndo || i == kExport) return true;
        if (i >= kSlotBase) { int w = (i - kSlotBase) % kPerSlot; return w == kLoop || w == kStop || w == kClear; }
        return false;
    }

    void fillSettings()
    {
        set_.output = get(kOutput);
        set_.monitor = get(kMonitor);
        set_.lengthBeats = getInt(kLength);
        set_.quantize = getInt(kQuantize);
        set_.recToDub = getInt(kAfterRec) == 1;
        set_.dubLevel = get(kDubLevel);
        set_.lateMs = getInt(kLatePress);
        set_.sync = getInt(kSync) == 0;
        set_.stopWithMpc = getInt(kMpcStop) == 0;
        for (int s = 0; s < kSlots; ++s) set_.vol[s] = get(slotParam(s, kVol));
    }

    bool takePress(int i)
    {
        uint32_t c = pressCount_[i].load(std::memory_order_relaxed);
        if (c == pressSeen_[i]) return false;
        pressSeen_[i] = c;
        return true;
    }

    void consumePresses()
    {
        for (int s = 0; s < kSlots; ++s) {
            if (takePress(slotParam(s, kLoop))) core_.press(s);
            if (takePress(slotParam(s, kStop))) core_.stopSlot(s);
            if (takePress(slotParam(s, kClear))) core_.requestClear(s);
        }
        if (takePress(kStopAll)) core_.stopAll();
        if (takePress(kClearAll)) core_.requestClearAll();
        if (takePress(kUndo)) core_.undo();
        if (takePress(kExport)) exportRequest_.store(true, std::memory_order_release);
    }

    void consumeLinkCommands()
    {
        if (!linkedShared_.load(std::memory_order_relaxed)) return;
        LinkBlock* l = shared_;
        for (int s = 0; s < kSlots; ++s) {
            uint32_t p = l->press[s].load(std::memory_order_acquire);
            uint32_t st = l->stop[s].load(std::memory_order_acquire);
            uint32_t c = l->clear[s].load(std::memory_order_acquire);
            if (p != seenPress_[s]) { seenPress_[s] = p; core_.press(s); }
            if (st != seenStop_[s]) { seenStop_[s] = st; core_.stopSlot(s); }
            if (c != seenClear_[s]) { seenClear_[s] = c; core_.requestClear(s); }
        }
        uint32_t sa = l->stopAll.load(std::memory_order_acquire), u = l->undo.load(std::memory_order_acquire);
        if (sa != seenStopAll_) { seenStopAll_ = sa; core_.stopAll(); }
        if (u != seenUndo_) { seenUndo_ = u; core_.undo(); }
        l->lengthSetting.store(set_.lengthBeats, std::memory_order_relaxed);
    }

    void syncLinkSeen(LinkBlock* l)
    {
        for (int s = 0; s < kSlots; ++s) {
            seenPress_[s] = l->press[s].load(); seenStop_[s] = l->stop[s].load(); seenClear_[s] = l->clear[s].load();
        }
        seenStopAll_ = l->stopAll.load(); seenUndo_ = l->undo.load();
    }

    // ---- Enlace con Pads/Granular -------------------------------------------------------------
    // Intenta ser el Looper "publicado" en el bloque compartido (solo uno por proyecto).
    void tryLink(bool fromCtor)
    {
        (void)fromCtor;
        if (!shared_ || linkedShared_.load()) return;
        uint32_t expected = 0;
        if (!shared_->ownerToken.compare_exchange_strong(expected, token_)) {
            // Ya hay otro Looper enlazado ("BUSY"). Se reintenta en cada bloque: si ese Looper se
            // borra o apaga su Link Pads, este toma el enlace solo.
            return;
        }
        copySlots(shared_, link_);
        LinkBlock* old = link_;
        link_ = shared_;
        core_.setLink(link_);
        publishPool(link_, pool_);
        unpublish(old);
        syncLinkSeen(link_);
        linkedShared_.store(true);
    }

    // drain = esperar a que Pads/Granular dejen de leer (solo fuera del audio, p. ej. al cerrar).
    void releaseShared(bool drain)
    {
        if (!shared_ || !linkedShared_.load()) return;
        copySlots(privateLink_, shared_);
        link_ = privateLink_;
        core_.setLink(link_);
        publishPool(link_, pool_);
        if (drain) unpublishAndDrain(shared_); else unpublish(shared_);
        for (auto& s : shared_->slot) s.state.store(kEmpty);
        shared_->ownerToken.store(0);
        linkedShared_.store(false);
    }

    // Cambio de "Link Pads" en caliente (hilo de audio, al principio del bloque).
    void maybeSwitchLink()
    {
        bool want = getBool(kLink);
        if (want == linkedShared_.load(std::memory_order_relaxed)) return;
        if (want) tryLink(false);
        else releaseShared(false);
    }

    static void copySlots(LinkBlock* dst, LinkBlock* src)
    {
        if (!dst || !src || dst == src) return;
        for (int s = 0; s < kSlots; ++s) {
            LinkSlot& d = dst->slot[s];
            LinkSlot& o = src->slot[s];
            int used = o.usedChunks.load();
            for (int i = 0; i < kMaxChunksPerSlot; ++i) d.chunk[i].store(i < used ? o.chunk[i].load() : kNoChunk);
            d.usedChunks.store(used);
            d.lengthFrames.store(o.lengthFrames.load());
            d.recFrames.store(o.recFrames.load());
            d.lengthBeats.store(o.lengthBeats.load());
            d.phase.store(o.phase.load());
            d.generation.fetch_add(1);
            d.state.store(o.state.load());
        }
    }

    static void publishPool(LinkBlock* l, ChunkPool* p)
    {
        if (!l) return;
        l->poolChunks.store(p ? p->numChunks() : 0, std::memory_order_seq_cst);
        l->poolBase.store(p ? (uintptr_t)p->base() : 0, std::memory_order_seq_cst);
    }
    static void unpublish(LinkBlock* l)
    {
        if (!l) return;
        l->poolBase.store(0, std::memory_order_seq_cst);
    }
    static void unpublishAndDrain(LinkBlock* l)
    {
        if (!l) return;
        l->poolBase.store(0, std::memory_order_seq_cst);
        for (int i = 0; i < 2000 && l->readers.load(std::memory_order_seq_cst) > 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // ---- Cambios de memoria y carga de proyecto (hilo de audio, principio de bloque) ----------
    void installPendingState()
    {
        if (Staged* st = staged_.exchange(nullptr, std::memory_order_acq_rel)) {
            for (int s = 0; s < kSlots; ++s) core_.clearSlot(s);
            ChunkPool* old = pool_;
            pool_ = st->pool;
            memMB_.store(st->mb);
            core_.attach(link_, pool_);
            publishPool(link_, pool_);
            for (int s = 0; s < kSlots; ++s) {
                StagedSlot& ss = st->slot[s];
                if (ss.present) core_.installLoop(s, ss.chunks, ss.lengthFrames, ss.lengthBeats, ss.state, ss.recBpm);
            }
            installSig_.store(stateSignature());
            installEpoch_.store(st->epoch);
            st->pool = nullptr;
            retireLater(old);
            stagedDone_.store(st, std::memory_order_release);   // el hilo de trabajo lo borra
            return;
        }
        if (ChunkPool* p = pendingPool_.exchange(nullptr, std::memory_order_acq_rel)) {
            if (core_.swapPool(p)) {
                ChunkPool* old = pool_;
                pool_ = p;
                memMB_.store(pendingMB_.load());
                publishPool(link_, pool_);
                retireLater(old);
            } else {
                retireLater(p);   // ya hay loops: no se cambia
            }
        }
    }

    void retireLater(ChunkPool* p)
    {
        if (!p) return;
        ChunkPool* prev = retire_.exchange(p, std::memory_order_acq_rel);
        if (prev) retire2_.store(prev, std::memory_order_release);
    }

    uint64_t stateSignature() const
    {
        uint64_t h = 1469598103934665603ull;
        for (int s = 0; s < kSlots; ++s) {
            const LinkSlot& L = link_->slot[s];
            int x = L.state.load();
            h = (h ^ (uint64_t)(x == kOverdub ? kPlaying : x)) * 1099511628211ull;
            h = (h ^ (uint64_t)L.generation.load()) * 1099511628211ull;
            h = (h ^ (uint64_t)L.lengthFrames.load()) * 1099511628211ull;
        }
        return h;
    }

    static const float* slotFrame(const PoolReadGuard& g, const LinkSlot& L, int i)
    {
        uint32_t c = L.chunk[i >> kChunkShift].load(std::memory_order_relaxed);
        const float* base = g.chunkPtr(c);
        return base ? base + ((i & kChunkMask) << 1) : nullptr;
    }

    // ---- Hilo de trabajo: exportar, memoria, liberar, refrescar pantalla ----------------------
    void workerLoop()
    {
        auto last = std::chrono::steady_clock::now();
        while (!quit_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            // Liberar pools retirados cuando nadie los lee
            for (std::atomic<ChunkPool*>* slot : {&retire_, &retire2_}) {
                ChunkPool* r = slot->load(std::memory_order_acquire);
                if (!r) continue;
                if (privateLink_ && privateLink_->readers.load(std::memory_order_seq_cst) > 0) continue;
                if (shared_ && shared_->readers.load(std::memory_order_seq_cst) > 0) continue;
                if (slot->compare_exchange_strong(r, nullptr)) delete r;
            }
            if (Staged* d = stagedDone_.exchange(nullptr)) delete d;
            // Cambio de tamaño de memoria (solo si no hay loops)
            int want = memRequest_.load();
            if (want > 0 && want != memMB_.load() && !pendingPool_.load() && !staged_.load() && core_.allEmpty()) {
                auto* p = new ChunkPool();
                if (p->create((size_t)want << 20)) { pendingMB_.store(want); pendingPool_.store(p, std::memory_order_release); }
                else { delete p; memRequest_.store(memMB_.load()); }
            }
            // Exportar
            if (exportRequest_.exchange(false)) doExport();
            // Refrescar la pantalla al cambiar de estado (máx. ~2,5 veces por segundo)
            auto now = std::chrono::steady_clock::now();
            if (getBool(kLiveStatus) && displayDirty_.load(std::memory_order_acquire) &&
                now - last > std::chrono::milliseconds(400)) {
                displayDirty_.store(false);
                last = now;
                requestDisplayUpdate();
            }
        }
    }

    void doExport()
    {
        exportState_.store(1);
        std::string root = getInt(kExportTo) == 1 ? internalDriveRoot() : removableDriveRoot();
        if (const char* env = std::getenv("LOOPERX_EXPORT_ROOT")) root = env;   // pruebas en el PC
        if (root.empty() || !isDir(root)) { exportState_.store(4); return; }
        std::string dir = root + "/LooperX/Loops";
        mkdirs(dir);
        std::time_t tt = std::time(nullptr);
        char stamp[32];
        std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&tt));
        int written = 0;
        bool ok = true;
        LinkBlock* l = link_;
        PoolReadGuard g(l);
        for (int s = 0; s < kSlots; ++s) {
            LinkSlot& L = l->slot[s];
            int x = L.state.load(), len = L.lengthFrames.load();
            if (!(x == kPlaying || x == kOverdub || x == kStopped) || len <= 0 || !g.base()) continue;
            char name[200];
            const double beats = (double)L.lengthBeats.load();
            if (beats >= 0.5)
                std::snprintf(name, sizeof name, "%s/LooperX_%s_L%02d_%.0fbpm_%.0fbeats.wav", dir.c_str(), stamp, s + 1,
                              core_.recBpm(s), beats);
            else   // loop libre (Quantize Free): sin tempo, se indica la duración
                std::snprintf(name, sizeof name, "%s/LooperX_%s_L%02d_free_%.2fs.wav", dir.c_str(), stamp, s + 1,
                              (double)len / sampleRate());
            ok = writeWav24(name, (int)sampleRate(), len, [&](int64_t i, float& a, float& b) {
                const float* f = slotFrame(g, L, (int)i);
                if (!f) return false;
                a = f[0]; b = f[1];
                return true;
            }) && ok;
            ++written;
        }
        exportState_.store(written == 0 ? 5 : ok ? 2 : 3);
    }

    // ---- Serialización ------------------------------------------------------------------------
    static void put(std::vector<uint8_t>& o, const char* s, size_t n) { o.insert(o.end(), s, s + n); }
    static void putU32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back((uint8_t)(v >> (8 * i))); }
    static void putF32(std::vector<uint8_t>& o, float f) { uint32_t v; std::memcpy(&v, &f, 4); putU32(o, v); }
    static void putF64(std::vector<uint8_t>& o, double d) { uint64_t v; std::memcpy(&v, &d, 8); for (int i = 0; i < 8; ++i) o.push_back((uint8_t)(v >> (8 * i))); }
    static uint32_t getU32(const uint8_t* d, size_t& p) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= (uint32_t)d[p + (size_t)i] << (8 * i); p += 4; return v; }
    static float getF32(const uint8_t* d, size_t& p) { uint32_t v = getU32(d, p); float f; std::memcpy(&f, &v, 4); return f; }
    static double getF64(const uint8_t* d, size_t& p) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= (uint64_t)d[p + (size_t)i] << (8 * i); p += 8; double x; std::memcpy(&x, &v, 8); return x; }

    LooperCore core_;
    LooperSettings set_;
    ChunkPool* pool_ = nullptr;
    std::atomic<int> memMB_{32};
    std::atomic<int> freeSecs_{0};
    LinkBlock* privateLink_ = nullptr;
    LinkBlock* shared_ = nullptr;
    LinkBlock* link_ = nullptr;
    std::atomic<bool> linkedShared_{false};
    uint32_t token_ = 1;

    std::atomic<uintptr_t> audioThread_{0};
    std::atomic<float> lastPlain_[kNumParams];
    std::atomic<uint32_t> pressCount_[kNumParams];
    uint32_t pressSeen_[kNumParams];
    uint32_t seenPress_[kSlots] = {}, seenStop_[kSlots] = {}, seenClear_[kSlots] = {}, seenStopAll_ = 0, seenUndo_ = 0;
    std::atomic<int> numerator_{4};

    std::atomic<ChunkPool*> pendingPool_{nullptr};
    std::atomic<int> pendingMB_{32};
    std::atomic<ChunkPool*> retire_{nullptr};
    std::atomic<ChunkPool*> retire2_{nullptr};
    std::atomic<int> memRequest_{0};
    std::atomic<Staged*> staged_{nullptr};
    std::atomic<Staged*> stagedDone_{nullptr};
    // Solo hilo de UI:
    std::vector<uint8_t> cache_, loadedRaw_;
    uint64_t cacheSig_ = 0;
    uint32_t loadedEpoch_ = 0, loadCounter_ = 0;
    // Escritos por el audio al instalar una carga:
    std::atomic<uint64_t> installSig_{0};
    std::atomic<uint32_t> installEpoch_{0};

    std::atomic<bool> discardPresses_{false};
    std::atomic<bool> exportRequest_{false};
    std::atomic<int> exportState_{0};
    std::atomic<bool> memFull_{false};
    std::atomic<bool> displayDirty_{false};
    uint32_t lastSig_ = 0;
    std::atomic<bool> quit_{false};
    std::thread worker_;
    float silence_[kBlock] = {};
};

MPC_PLUGIN_ENTRY(LooperXLooper)
