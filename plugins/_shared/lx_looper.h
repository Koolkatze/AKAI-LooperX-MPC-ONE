// lx_looper.h - LooperX: motor de looper estilo pedal, sincronizado al transporte del MPC.
//
// SPDX-License-Identifier: MIT
//
// Independiente del framework del SDK (se prueba solo en el PC). Lo usa "LooperX Looper".
//
// Reloj: un contador interno continuo en tiempos (negras) que avanza al tempo del MPC. Cuando el
// MPC está en PLAY, la rejilla de compases se alinea con la del MPC (barStartPpq); los loops NO
// saltan cuando la secuencia del MPC da la vuelta, porque su fase sale del reloj interno
// continuo. Grabar, cerrar, arrancar y parar se cuantizan a compás, a tiempo o libre.
//
// Pedal (botón LOOP de cada slot):
//   EMPTY -> WAIT (espera al compás) -> REC -> (cierre cuantizado o largo fijo) -> PLAY
//   PLAY -> DUB (overdub) -> PLAY ...      STOP: PLAY/DUB -> STOP -> PLAY (cuantizado)
//   Pulsación tardía (dentro de "Late Press" ms tras un compás): se aplica a ese compás y se
//   recupera el audio que faltaba de un historial de 1 s.
//
// Memoria: un pool de bloques de 16384 frames estéreo float reservado fuera del audio. Los loops
// se guardan como índices de bloque en el LinkBlock (compartido con Pads/Granular si hay enlace).
#ifndef LX_LOOPER_H
#define LX_LOOPER_H

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "lx_link.h"

namespace lx {

class SpinLock
{
public:
    void lock() { while (f_.test_and_set(std::memory_order_acquire)) {} }
    void unlock() { f_.clear(std::memory_order_release); }
private:
    std::atomic_flag f_ = ATOMIC_FLAG_INIT;
};

// Pool de bloques de audio. Se crea y destruye fuera del hilo de audio; alloc/release son seguros
// desde cualquier hilo (spinlock de muy pocas instrucciones).
class ChunkPool
{
public:
    ~ChunkPool() { destroy(); }
    bool create(size_t bytes)
    {
        destroy();
        size_t n = bytes / ((size_t)kChunkFrames * 2 * sizeof(float));
        if (n < 4) return false;
        base_ = static_cast<float*>(std::malloc(n * (size_t)kChunkFrames * 2 * sizeof(float)));
        if (!base_) return false;
        free_.resize(n);
        for (size_t i = 0; i < n; ++i) free_[i] = (uint32_t)(n - 1 - i);
        top_ = (int)n;
        num_ = (int)n;
        // Tocar cada página ahora para que el kernel la asigne fuera del audio.
        for (size_t i = 0; i < n * (size_t)kChunkFrames * 2; i += 1024) base_[i] = 0.f;
        return true;
    }
    void destroy()
    {
        std::free(base_);
        base_ = nullptr;
        free_.clear();
        top_ = num_ = 0;
    }
    uint32_t alloc()
    {
        lock_.lock();
        uint32_t c = top_ > 0 ? free_[(size_t)--top_] : kNoChunk;
        lock_.unlock();
        return c;
    }
    void release(uint32_t c)
    {
        if (c == kNoChunk || c >= (uint32_t)num_) return;
        lock_.lock();
        if (top_ < num_) free_[(size_t)top_++] = c;
        lock_.unlock();
    }
    float* ptr(uint32_t c) const { return base_ + (size_t)c * (size_t)kChunkFrames * 2; }
    float* base() const { return base_; }
    int numChunks() const { return num_; }
    int freeChunks() const { return top_; }
    double secondsFree(double sr) const { return (double)top_ * kChunkFrames / sr; }
    double secondsTotal(double sr) const { return (double)num_ * kChunkFrames / sr; }
private:
    float* base_ = nullptr;
    std::vector<uint32_t> free_;
    int top_ = 0, num_ = 0;
    SpinLock lock_;
};

struct HostTime
{
    bool valid = false, playing = false, tempoValid = false, ppqValid = false;
    double bpm = 120.0, ppq = 0.0, barStartPpq = 0.0;
    int num = 4, den = 4;
};

struct LooperSettings
{
    int lengthBeats = 0;        // 0 = AUTO (cierras tú); N = cierre automático a N tiempos
    int quantize = 0;           // 0 compás, 1 tiempo, 2 libre
    bool recToDub = false;      // tras REC pasa a DUB (como muchos pedales) en vez de PLAY
    float dubLevel = 1.0f;      // 1 = sin decaimiento en overdub
    float monitor = 1.0f;       // entrada que pasa a la salida
    float output = 1.0f;
    float vol[kSlots];
    int lateMs = 80;
    bool sync = true;           // alinear la rejilla con el MPC cuando está en PLAY
    bool stopWithMpc = true;    // al parar el MPC se paran los loops; al darle a PLAY arrancan desde el principio
    LooperSettings() { for (float& v : vol) v = 1.0f; }
};

class LooperCore
{
public:
    enum Pending { kNone = 0, kStartRec, kCloseRec, kStartPlay, kStopPlay };

    // link: dónde vive el estado de los slots (compartido o privado). pool: memoria de audio.
    void attach(LinkBlock* link, ChunkPool* pool)
    {
        link_ = link;
        pool_ = pool;
    }
    // Ajustes a usar en las órdenes (press/stop...) que llegan entre bloques. process() también los fija.
    void useSettings(const LooperSettings* s) { set_ = s ? s : &defaults_; }
    void setLink(LinkBlock* link) { link_ = link; }
    ChunkPool* pool() const { return pool_; }
    LinkBlock* link() const { return link_; }

    void prepare(double sr)
    {
        sr_ = sr;
        histLen_ = (int)sr;              // 1 s de historial para pulsaciones tardías
        hist_.assign((size_t)histLen_ * 2, 0.0f);
        histPos_ = 0;
    }

    // Cambia el pool por otro (solo si no hay ningún loop). Llamar desde el hilo de audio.
    bool swapPool(ChunkPool* p)
    {
        if (!allEmpty()) return false;
        pool_ = p;
        return true;
    }

    bool allEmpty() const
    {
        for (int s = 0; s < kSlots; ++s) if (st(s) != kEmpty) return false;
        return true;
    }

    // ---- Órdenes (hilo de audio, entre bloques) --------------------------------------------
    void press(int s)
    {
        if (!valid(s)) return;
        SlotRt& r = rt_[s];
        lastSlot_ = s;
        switch (st(s)) {
            case kEmpty: beginRecArm(s); break;
            case kArmed: setSt(s, kEmpty); r.pending = kNone; break;
            case kRecording: requestClose(s); break;
            case kPlaying:
                if (r.pending == kStopPlay) { r.pending = kNone; break; }
                beginOverdub(s);
                break;
            case kOverdub: endOverdub(s); break;
            case kStopped: requestStart(s); break;
        }
    }
    void stopSlot(int s)
    {
        if (!valid(s)) return;
        SlotRt& r = rt_[s];
        switch (st(s)) {
            case kArmed: setSt(s, kEmpty); r.pending = kNone; break;
            case kRecording: requestClose(s); r.stopAfterClose = true; break;
            case kOverdub: endOverdub(s); requestStop(s); break;
            case kPlaying: if (r.pending == kStopPlay) r.pending = kNone; else requestStop(s); break;
            case kStopped: requestStart(s); break;
            default: break;
        }
    }
    // Borrado con confirmación: la primera pulsación arma, la segunda (en < 3 s) borra.
    void requestClear(int s)
    {
        if (!valid(s) || st(s) == kEmpty) return;
        SlotRt& r = rt_[s];
        if (r.clearArmedUntil > frameClock_) { clearSlot(s); r.clearArmedUntil = 0; }
        else r.clearArmedUntil = frameClock_ + (int64_t)(sr_ * 3.0);
    }
    bool clearArmed(int s) const { return valid(s) && rt_[s].clearArmedUntil > frameClock_; }
    void stopAll()
    {
        for (int s = 0; s < kSlots; ++s) {
            int x = st(s);
            if (x == kPlaying || x == kOverdub) { if (x == kOverdub) endOverdub(s); requestStop(s); }
            else if (x == kArmed) { setSt(s, kEmpty); rt_[s].pending = kNone; }
            else if (x == kRecording) { requestClose(s); rt_[s].stopAfterClose = true; }
        }
    }
    void requestClearAll()
    {
        if (clearAllArmedUntil_ > frameClock_) { for (int s = 0; s < kSlots; ++s) clearSlot(s); clearAllArmedUntil_ = 0; }
        else clearAllArmedUntil_ = frameClock_ + (int64_t)(sr_ * 3.0);
    }
    bool clearAllArmed() const { return clearAllArmedUntil_ > frameClock_; }
    void undo() { if (valid(lastSlot_)) undoSlot(lastSlot_); }
    void undoSlot(int s)
    {
        if (!valid(s)) return;
        SlotRt& r = rt_[s];
        if (st(s) == kOverdub) endOverdub(s);
        if (r.hasUndo) { swapUndo(s); r.hasUndo = false; r.hasRedo = true; }
        else if (r.hasRedo) { swapUndo(s); r.hasRedo = false; r.hasUndo = true; }
    }
    int lastSlot() const { return lastSlot_; }

    // ---- Consultas --------------------------------------------------------------------------
    int state(int s) const { return valid(s) ? st(s) : kEmpty; }
    int pending(int s) const { return valid(s) ? rt_[s].pending : kNone; }
    bool hasUndo(int s) const { return valid(s) && rt_[s].hasUndo; }
    int lengthFrames(int s) const { return valid(s) ? link_->slot[s].lengthFrames.load(std::memory_order_relaxed) : 0; }
    double lengthBeats(int s) const { return valid(s) ? rt_[s].lengthBeats : 0.0; }
    double recBpm(int s) const { return valid(s) ? rt_[s].recBpm : 120.0; }
    int recFrames(int s) const { return valid(s) ? link_->slot[s].recFrames.load(std::memory_order_relaxed) : 0; }
    double bpm() const { return bpm_; }
    double barBeats() const { return barLen_; }
    double beatUnit() const { return beatLen_; }
    double sampleRate() const { return sr_; }
    int64_t frameClock() const { return frameClock_; }
    float peak(int s) const { return valid(s) ? rt_[s].peak : 0.f; }
    bool memoryFullFlag() { bool f = memFull_; memFull_ = false; return f; }

    // Frame i del slot s (para exportar/guardar). Solo con el pool vivo.
    const float* frame(int s, int i) const
    {
        uint32_t c = link_->slot[s].chunk[i >> kChunkShift].load(std::memory_order_relaxed);
        if (c == kNoChunk || !pool_) return nullptr;
        return pool_->ptr(c) + ((i & kChunkMask) << 1);
    }

    // ---- Instalar un loop ya hecho (cargar proyecto). Desde el hilo de audio, en reset(). -----
    // chunks: índices ya llenos en ESTE pool.
    void installLoop(int s, const std::vector<uint32_t>& chunks, int lengthFrames, double lengthBeats,
                     int state, double recBpm)
    {
        if (!valid(s)) return;
        clearSlot(s);
        LinkSlot& L = link_->slot[s];
        int n = (int)std::min<size_t>(chunks.size(), kMaxChunksPerSlot);
        for (int i = 0; i < n; ++i) L.chunk[i].store(chunks[(size_t)i], std::memory_order_relaxed);
        L.usedChunks.store(n, std::memory_order_relaxed);
        L.lengthFrames.store(lengthFrames, std::memory_order_relaxed);
        L.lengthBeats.store((float)lengthBeats, std::memory_order_relaxed);
        L.generation.fetch_add(1, std::memory_order_release);
        SlotRt& r = rt_[s];
        r.lengthBeats = lengthBeats > 0 ? lengthBeats : (double)lengthFrames * bpsAt(recBpm);
        r.recBpm = recBpm;
        r.anchorBeat = gridOrigin_;
        r.gain = 0.f;
        r.pending = kNone;
        setSt(s, state == kStopped ? kStopped : kPlaying);
    }

    void clearSlot(int s)
    {
        if (!valid(s)) return;
        LinkSlot& L = link_->slot[s];
        setSt(s, kEmpty);
        int used = L.usedChunks.load(std::memory_order_relaxed);
        L.lengthFrames.store(0, std::memory_order_relaxed);
        L.usedChunks.store(0, std::memory_order_relaxed);
        L.recFrames.store(0, std::memory_order_relaxed);
        L.generation.fetch_add(1, std::memory_order_release);
        for (int i = 0; i < used && i < kMaxChunksPerSlot; ++i) {
            uint32_t c = L.chunk[i].exchange(kNoChunk, std::memory_order_relaxed);
            if (pool_) pool_->release(c);
        }
        freeUndo(s);
        SlotRt& r = rt_[s];
        r.pending = kNone;
        r.gain = 0.f;
        r.lengthBeats = 0;
        r.stopAfterClose = false;
        r.clearArmedUntil = 0;
    }

    // ---- Proceso ----------------------------------------------------------------------------
    // in/out pueden ser los mismos buffers (el MPC procesa en el sitio).
    void process(const float* inL, const float* inR, float* outL, float* outR, int n, const HostTime& ht,
                 const LooperSettings& set)
    {
        set_ = &set;
        if (!link_) return;
        // Tempo y compás
        if (ht.valid && ht.tempoValid && ht.bpm > 1.0) bpm_ = ht.bpm;
        bps_ = bpm_ / 60.0 / sr_;
        int num = ht.valid && ht.num > 0 ? ht.num : 4, den = ht.valid && ht.den > 0 ? ht.den : 4;
        barLen_ = (double)num * 4.0 / (double)den;
        beatLen_ = 4.0 / (double)den;
        const double clk0 = clk_;

        // Alineación con el MPC
        const bool hostPlaying = ht.valid && ht.playing && ht.ppqValid;
        if (set.sync && hostPlaying) {
            double barStart = ht.barStartPpq <= ht.ppq + 1e-9 ? ht.barStartPpq : ht.ppq;
            gridOrigin_ = clk0 - (ht.ppq - barStart);
            if (!wasHostPlaying_) {
                // El MPC acaba de arrancar: todos los loops empiezan juntos desde el principio.
                for (int s = 0; s < kSlots; ++s) {
                    int x = st(s);
                    if (x == kPlaying || x == kOverdub || (x == kStopped && rt_[s].stoppedByMpc)) {
                        rt_[s].anchorBeat = gridOrigin_;
                        if (x == kStopped && rt_[s].stoppedByMpc) setSt(s, kPlaying);
                        rt_[s].stoppedByMpc = false;
                    }
                }
            }
        }
        if (wasHostPlaying_ && !hostPlaying && set.stopWithMpc) {
            // El MPC se ha parado: se paran los loops (vuelven al darle a PLAY).
            for (int s = 0; s < kSlots; ++s) {
                int x = st(s);
                if (x == kOverdub) endOverdub(s);
                if (x == kPlaying || x == kOverdub) { setSt(s, kStopped); rt_[s].stoppedByMpc = true; rt_[s].pending = kNone; }
                if (x == kRecording) { requestClose(s); }
            }
        }
        wasHostPlaying_ = hostPlaying;

        // Si no hay ningún loop y el MPC está parado, la rejilla empieza en la primera pulsación.
        const int lateFrames = (int)(set.lateMs * 0.001 * sr_);
        const float ramp = 1.0f / (float)(0.006 * sr_);

        // Historial de entrada (para pulsaciones tardías)
        for (int i = 0; i < n; ++i) {
            int h = (int)((histPos_ + i) % histLen_);
            hist_[(size_t)h * 2] = inL[i];
            hist_[(size_t)h * 2 + 1] = inR[i];
        }

        // Preparar acciones pendientes de este bloque
        for (int s = 0; s < kSlots; ++s) rt_[s].actAt = -1;
        for (int s = 0; s < kSlots; ++s) {
            SlotRt& r = rt_[s];
            if (r.pending == kNone) continue;
            int f = beatToFrame(r.pendingBeat, clk0);
            if (f < n) r.actAt = f < 0 ? 0 : f;
        }

        float pk[kSlots] = {};
        for (int i = 0; i < n; ++i) {
            const float l = inL[i], rr = inR[i];
            const double beat = clk0 + (double)i * bps_;
            float accL = 0.f, accR = 0.f;
            for (int s = 0; s < kSlots; ++s) {
                SlotRt& r = rt_[s];
                if (r.actAt == i) { r.actAt = -1; firePending(s, beat, lateFrames); }
                int x = st(s);
                if (x == kEmpty || x == kArmed) continue;
                LinkSlot& L = link_->slot[s];
                if (x == kRecording) {
                    int w = L.recFrames.load(std::memory_order_relaxed);
                    if (!writeFrame(s, w, l, rr)) { closeRecording(s, w, beat); memFull_ = true; continue; }
                    L.recFrames.store(w + 1, std::memory_order_relaxed);
                    continue;
                }
                const int len = L.lengthFrames.load(std::memory_order_relaxed);
                if (len <= 0 || r.lengthBeats <= 0) continue;
                const float target = (x == kStopped) ? 0.f : 1.f;
                if (r.gain < target) r.gain = std::min(target, r.gain + ramp);
                else if (r.gain > target) r.gain = std::max(target, r.gain - ramp);
                double ph = (beat - r.anchorBeat) / r.lengthBeats;
                ph -= std::floor(ph);
                double pos = ph * (double)len;
                int i0 = (int)pos;
                if (i0 >= len) i0 = len - 1;
                float fr = (float)(pos - (double)i0);
                if (fr < 1e-4f) fr = 0.f;
                int i1 = i0 + 1 >= len ? 0 : i0 + 1;
                if (r.gain > 0.f) {
                    const float* a = frame(s, i0);
                    const float* b = frame(s, i1);
                    if (a && b) {
                        float g = r.gain * set.vol[s];
                        float sl = (a[0] + (b[0] - a[0]) * fr) * g, sr = (a[1] + (b[1] - a[1]) * fr) * g;
                        accL += sl; accR += sr;
                        float m = std::fabs(sl) > std::fabs(sr) ? std::fabs(sl) : std::fabs(sr);
                        if (m > pk[s]) pk[s] = m;
                    }
                }
                if (x == kOverdub) {
                    int wi = fr >= 0.5f ? i1 : i0;
                    overdubFrame(s, wi, l, rr, set.dubLevel);
                }
            }
            outL[i] = (accL + l * set.monitor) * set.output;
            outR[i] = (accR + rr * set.monitor) * set.output;
        }

        for (int s = 0; s < kSlots; ++s) {
            rt_[s].peak = std::max(rt_[s].peak * 0.8f, pk[s]);
            LinkSlot& L = link_->slot[s];
            int len = L.lengthFrames.load(std::memory_order_relaxed);
            if (len > 0 && rt_[s].lengthBeats > 0) {
                double ph = (clk0 + n * bps_ - rt_[s].anchorBeat) / rt_[s].lengthBeats;
                L.phase.store((float)(ph - std::floor(ph)), std::memory_order_relaxed);
            } else {
                L.phase.store(0.f, std::memory_order_relaxed);
            }
        }
        histPos_ = (histPos_ + n) % histLen_;
        clk_ = clk0 + (double)n * bps_;
        frameClock_ += n;
        if (link_) {
            link_->heartbeat.fetch_add(1, std::memory_order_relaxed);
            link_->bpm.store((float)bpm_, std::memory_order_relaxed);
            link_->beatsPerBar.store(num, std::memory_order_relaxed);
        }
    }

private:
    struct SlotRt
    {
        double anchorBeat = 0, lengthBeats = 0, recStartBeat = 0, recBpm = 120, pendingBeat = 0;
        int pending = kNone;
        int actAt = -1;
        float gain = 0.f, peak = 0.f;
        bool stopAfterClose = false, stoppedByMpc = false;
        bool hasUndo = false, hasRedo = false;
        int64_t clearArmedUntil = 0;
        uint32_t undoChunk[kMaxChunksPerSlot];
        uint8_t undoTouched[kMaxChunksPerSlot];
        SlotRt() { for (auto& c : undoChunk) c = kNoChunk; std::memset(undoTouched, 0, sizeof undoTouched); }
    };

    bool valid(int s) const { return s >= 0 && s < kSlots && link_ != nullptr; }
    int st(int s) const { return link_->slot[s].state.load(std::memory_order_relaxed); }
    void setSt(int s, int v) { link_->slot[s].state.store(v, std::memory_order_release); }
    double bpsAt(double bpm) const { return bpm / 60.0 / sr_; }

    // Frame del bloque actual en que cae el tiempo `beat` (puede ser negativo o >= n).
    int beatToFrame(double beat, double clk0) const
    {
        double f = (beat - clk0) / bps_;
        if (f <= 0) return f > -1e9 ? (int)std::floor(f) : -1000000000;
        if (f > 2e9) return 2000000000;
        return (int)std::ceil(f - 1e-6);
    }
    double unitBeats() const
    {
        const LooperSettings& set = *set_;
        return set.quantize == 0 ? barLen_ : set.quantize == 1 ? beatLen_ : 0.0;
    }
    double nextBoundary(double beat) const
    {
        double u = unitBeats();
        if (u <= 0) return beat;
        double k = std::ceil((beat - gridOrigin_) / u - 1e-9);
        return gridOrigin_ + k * u;
    }
    // Límite reciente si la pulsación llega tarde (dentro de la ventana); si no, NaN.
    double lateBoundary(double beat) const
    {
        double u = unitBeats();
        if (u <= 0) return NAN;
        double k = std::floor((beat - gridOrigin_) / u + 1e-9);
        double b = gridOrigin_ + k * u;
        double framesLate = (beat - b) / bps_;
        double win = set_->lateMs * 0.001 * sr_;
        if (framesLate >= 0 && framesLate <= win && framesLate < histLen_ - 1) return b;
        return NAN;
    }
    bool anyContent() const
    {
        for (int s = 0; s < kSlots; ++s) { int x = st(s); if (x != kEmpty && x != kArmed) return true; }
        return false;
    }

    void beginRecArm(int s)
    {
        SlotRt& r = rt_[s];
        const double now = clk_;
        // Primer loop con el MPC parado y sin rejilla previa: la rejilla empieza ahora mismo.
        if (!anyContent() && !wasHostPlaying_) gridOrigin_ = now;
        r.stopAfterClose = false;
        if (unitBeats() <= 0 || std::fabs(now - gridOrigin_) < 1e-9) {
            startRecording(s, now, 0);
            return;
        }
        double late = lateBoundary(now);
        if (!std::isnan(late)) {
            int backfill = (int)std::llround((now - late) / bps_);
            startRecording(s, late, backfill);
            return;
        }
        r.pending = kStartRec;
        r.pendingBeat = nextBoundary(now);
        setSt(s, kArmed);
    }

    void startRecording(int s, double startBeat, int backfillFrames)
    {
        SlotRt& r = rt_[s];
        LinkSlot& L = link_->slot[s];
        // vaciar por si acaso
        int used = L.usedChunks.load(std::memory_order_relaxed);
        for (int i = 0; i < used; ++i) { uint32_t c = L.chunk[i].exchange(kNoChunk); if (pool_) pool_->release(c); }
        L.usedChunks.store(0, std::memory_order_relaxed);
        L.lengthFrames.store(0, std::memory_order_relaxed);
        L.recFrames.store(0, std::memory_order_relaxed);
        L.generation.fetch_add(1, std::memory_order_release);
        freeUndo(s);
        r.recStartBeat = startBeat;
        r.anchorBeat = startBeat;
        r.recBpm = bpm_;
        r.pending = kNone;
        r.gain = 0.f;
        setSt(s, kRecording);
        // Recuperar del historial los frames entre el límite y ahora
        int w = 0;
        for (int k = backfillFrames; k > 0; --k) {
            int h = (int)(((histPos_ - k) % histLen_ + histLen_) % histLen_);
            if (!writeFrame(s, w, hist_[(size_t)h * 2], hist_[(size_t)h * 2 + 1])) break;
            ++w;
        }
        L.recFrames.store(w, std::memory_order_relaxed);
        // Largo fijo: programar el cierre automático
        if (set_->lengthBeats > 0) {
            r.pending = kCloseRec;
            r.pendingBeat = startBeat + (double)set_->lengthBeats * beatLen_;
        }
    }

    void requestClose(int s)
    {
        SlotRt& r = rt_[s];
        const double now = clk_;
        if (unitBeats() <= 0) { closeRecording(s, link_->slot[s].recFrames.load(), now); return; }
        double late = lateBoundary(now);
        if (!std::isnan(late) && late > r.recStartBeat + 1e-9) {
            int keep = (int)std::llround((late - r.recStartBeat) / bps_);
            closeRecording(s, keep, late);
            return;
        }
        double b = nextBoundary(now);
        if (b <= r.recStartBeat + 1e-9) b = nextBoundary(r.recStartBeat + unitBeats() * 0.5);
        if (r.pending == kCloseRec) b = std::min(b, r.pendingBeat);  // nunca más tarde que el largo fijo
        r.pending = kCloseRec;
        r.pendingBeat = b;
    }

    void requestStart(int s)
    {
        SlotRt& r = rt_[s];
        const double now = clk_;
        if (unitBeats() <= 0) { r.anchorBeat = now; setSt(s, kPlaying); r.pending = kNone; return; }
        double late = lateBoundary(now);
        if (!std::isnan(late)) { r.anchorBeat = late; setSt(s, kPlaying); r.pending = kNone; return; }
        r.pending = kStartPlay;
        r.pendingBeat = nextBoundary(now);
    }

    void requestStop(int s)
    {
        SlotRt& r = rt_[s];
        const double now = clk_;
        if (unitBeats() <= 0 || !std::isnan(lateBoundary(now))) { setSt(s, kStopped); r.pending = kNone; return; }
        r.pending = kStopPlay;
        r.pendingBeat = nextBoundary(now);
    }

    void firePending(int s, double beat, int /*lateFrames*/)  // NOLINT
    {
        SlotRt& r = rt_[s];
        int p = r.pending;
        double at = r.pendingBeat;
        r.pending = kNone;
        switch (p) {
            case kStartRec: startRecording(s, at, 0); break;
            case kCloseRec: {
                int keep = (int)std::llround((at - r.recStartBeat) / bps_);
                closeRecording(s, keep, at);
                break;
            }
            case kStartPlay: r.anchorBeat = at; setSt(s, kPlaying); break;
            case kStopPlay: setSt(s, kStopped); break;
            default: break;
        }
        (void)beat;
    }

    // Cierra la grabación con `keep` frames. Lo grabado de más (cierre tardío) se funde con el
    // principio del loop para que el empalme no haga clic.
    void closeRecording(int s, int keep, double closeBeat)
    {
        SlotRt& r = rt_[s];
        LinkSlot& L = link_->slot[s];
        const int written = L.recFrames.load(std::memory_order_relaxed);
        const int minLen = (int)(sr_ * 0.05);
        r.pending = kNone;
        if (keep > written) keep = written;
        if (keep < minLen) { clearSlot(s); return; }
        int xf = std::min(written - keep, (int)(sr_ * 0.01));
        if (xf >= 32) {
            for (int k = 0; k < xf; ++k) {
                float a = (float)k / (float)xf;
                float* d = mframe(s, k);
                const float* x = frame(s, keep + k);
                if (d && x) { d[0] = d[0] * a + x[0] * (1.f - a); d[1] = d[1] * a + x[1] * (1.f - a); }
            }
        } else {
            int fade = std::min((int)(sr_ * 0.003), keep / 4);
            for (int k = 0; k < fade; ++k) {
                float a = (float)k / (float)fade;
                float* d = mframe(s, k);
                float* e = mframe(s, keep - 1 - k);
                if (d) { d[0] *= a; d[1] *= a; }
                if (e) { e[0] *= a; e[1] *= a; }
            }
        }
        // Liberar los bloques que sobran
        int need = (keep - 1) / kChunkFrames + 1;
        int used = L.usedChunks.load(std::memory_order_relaxed);
        for (int i = need; i < used; ++i) { uint32_t c = L.chunk[i].exchange(kNoChunk); if (pool_) pool_->release(c); }
        L.usedChunks.store(std::min(used, need), std::memory_order_relaxed);
        L.lengthFrames.store(keep, std::memory_order_relaxed);
        double lb = closeBeat - r.recStartBeat;
        if (lb <= 0 || unitBeats() <= 0) lb = (double)keep * bps_;
        r.lengthBeats = lb;
        L.lengthBeats.store((float)lb, std::memory_order_relaxed);
        L.generation.fetch_add(1, std::memory_order_release);
        r.anchorBeat = r.recStartBeat;
        r.gain = 1.f;
        r.hasUndo = r.hasRedo = false;
        if (r.stopAfterClose) { setSt(s, kStopped); r.stopAfterClose = false; }
        else setSt(s, set_ && set_->recToDub ? kOverdub : kPlaying);
    }

    void beginOverdub(int s) { freeUndo(s); setSt(s, kOverdub); }
    void endOverdub(int s) { setSt(s, kPlaying); link_->slot[s].generation.fetch_add(1, std::memory_order_release); }

    float* mframe(int s, int i)
    {
        if (i < 0 || (i >> kChunkShift) >= kMaxChunksPerSlot) return nullptr;
        uint32_t c = link_->slot[s].chunk[i >> kChunkShift].load(std::memory_order_relaxed);
        if (c == kNoChunk || !pool_) return nullptr;
        return pool_->ptr(c) + ((i & kChunkMask) << 1);
    }

    bool writeFrame(int s, int i, float l, float r)
    {
        int ci = i >> kChunkShift;
        if (ci >= kMaxChunksPerSlot || !pool_) return false;
        LinkSlot& L = link_->slot[s];
        int used = L.usedChunks.load(std::memory_order_relaxed);
        while (used <= ci) {
            uint32_t c = pool_->alloc();
            if (c == kNoChunk) return false;
            std::memset(pool_->ptr(c), 0, sizeof(float) * (size_t)kChunkFrames * 2);
            L.chunk[used].store(c, std::memory_order_release);
            L.usedChunks.store(++used, std::memory_order_release);
        }
        float* d = pool_->ptr(L.chunk[ci].load(std::memory_order_relaxed)) + ((i & kChunkMask) << 1);
        d[0] = l;
        d[1] = r;
        return true;
    }

    void overdubFrame(int s, int i, float l, float r, float fb)
    {
        SlotRt& rr = rt_[s];
        int ci = i >> kChunkShift;
        if (ci >= kMaxChunksPerSlot) return;
        LinkSlot& L = link_->slot[s];
        uint32_t c = L.chunk[ci].load(std::memory_order_relaxed);
        if (c == kNoChunk || !pool_) return;
        if (!rr.undoTouched[ci]) {  // copia para UNDO la primera vez que se toca el bloque
            uint32_t u = pool_->alloc();
            if (u != kNoChunk) {
                std::memcpy(pool_->ptr(u), pool_->ptr(c), sizeof(float) * (size_t)kChunkFrames * 2);
                rr.undoChunk[ci] = u;
                rr.undoTouched[ci] = 1;
                rr.hasUndo = true;
                rr.hasRedo = false;
            }
        }
        float* d = pool_->ptr(c) + ((i & kChunkMask) << 1);
        d[0] = d[0] * fb + l;
        d[1] = d[1] * fb + r;
    }

    void freeUndo(int s)
    {
        SlotRt& r = rt_[s];
        for (int i = 0; i < kMaxChunksPerSlot; ++i)
            if (r.undoTouched[i]) {
                if (pool_) pool_->release(r.undoChunk[i]);
                r.undoChunk[i] = kNoChunk;
                r.undoTouched[i] = 0;
            }
        r.hasUndo = r.hasRedo = false;
    }

    void swapUndo(int s)
    {
        SlotRt& r = rt_[s];
        LinkSlot& L = link_->slot[s];
        int used = L.usedChunks.load(std::memory_order_relaxed);
        for (int i = 0; i < used; ++i)
            if (r.undoTouched[i]) {
                uint32_t cur = L.chunk[i].load(std::memory_order_relaxed);
                L.chunk[i].store(r.undoChunk[i], std::memory_order_release);
                r.undoChunk[i] = cur;
            }
        L.generation.fetch_add(1, std::memory_order_release);
    }

    LinkBlock* link_ = nullptr;
    ChunkPool* pool_ = nullptr;
    LooperSettings defaults_;
    const LooperSettings* set_ = &defaults_;
    SlotRt rt_[kSlots];
    double sr_ = 44100.0, bpm_ = 120.0, bps_ = 120.0 / 60.0 / 44100.0;
    double barLen_ = 4.0, beatLen_ = 1.0;
    double clk_ = 0.0, gridOrigin_ = 0.0;
    int64_t frameClock_ = 0;
    bool wasHostPlaying_ = false;
    int lastSlot_ = -1;
    int64_t clearAllArmedUntil_ = 0;
    bool memFull_ = false;
    std::vector<float> hist_;
    int histLen_ = 44100;
    int64_t histPos_ = 0;
};

}  // namespace lx

#endif
