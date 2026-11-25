// LooperXPads.cpp - LooperX Pads: controla LooperX Looper con los pads del MPC o por MIDI externo.
//
// SPDX-License-Identifier: MIT
//
// En el MPC los efectos no reciben MIDI, así que el Looper (efecto) no puede leer los pads. Este
// instrumento sí los recibe y le pasa las pulsaciones al Looper por el enlace en memoria
// compartida (lx_link.h; NO forma parte del contrato documentado del SDK de Akai: experimental).
// No produce sonido.
//
// Notas (desde "Base Note", por defecto 36 = C1 en la numeración del MPC; usa LEARN):
//   Banco A (base+0..15)  = LOOP 1..16  (vacío → graba, graba → reproduce, PLAY ↔ DUB)
//   Banco B (base+16..31) = STOP 1..16  (para / arranca)
//   Banco C (base+32..47) = CLEAR 1..16 (dos golpes para borrar)
//   Banco D (base+48)     = STOP ALL,  (base+49) = UNDO
// Con "Layout" = 1 BANK solo se usan los 16 primeros (LOOP).
// CC (pedaleras, controladores): CC102..117 = LOOP 1..16, CC118 = STOP ALL, CC119 = UNDO
// (valor >= 64 = pulsación). Canal: ALL o 1..16.
// La pantalla muestra el estado de cada loop y sus botones también disparan los loops.

#include "mpc_plugin.h"

#include <pthread.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "../../_shared/lx_link.h"

using namespace lx;

namespace {

const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
std::string noteName(int n)   // convención del MPC: 60 = C3
{
    char b[12];
    std::snprintf(b, sizeof b, "%s%d", kNoteNames[((n % 12) + 12) % 12], n / 12 - 2);
    return b;
}

uintptr_t selfThread() { return (uintptr_t)pthread_self(); }

constexpr int kCcBase = 102;

}  // namespace

class LooperXPads : public mpc::Plugin
{
public:
    enum Params { kActive, kBaseNote, kLearn, kChannel, kLayout, kCcCtrl, kStatus, kLoopBase, kStopAll = kLoopBase + kSlots, kUndo, kNumParams };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Pads";
        i.vendor = "LooperX";
        i.uniqueId = MPC_FOURCC('L', 'x', 'P', 'd');
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        i.tailSamples = 1;
        return i;
    }

    LooperXPads() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(boolParam("on", "Active", true));
        addParam(intParam("base", "Base Note", 0, 127, 36));
        addParam(boolParam("learn", "Learn", false));
        addParam(intParam("chan", "Channel", 0, 16, 0));
        addParam(choiceParam("layout", "Layout", {"4 BANKS", "1 BANK"}, 0));
        addParam(boolParam("cc", "CC Ctrl", true));
        addParam(boolParam("status", "Status", true));
        for (int s = 0; s < kSlots; ++s) {
            char id[16], nm[16];
            std::snprintf(id, sizeof id, "loop%d", s + 1); std::snprintf(nm, sizeof nm, "LOOP %d", s + 1);
            addParam(boolParam(id, nm, false));
        }
        addParam(boolParam("stopall", "Stop All", false));
        addParam(boolParam("undo", "Undo", false));
        if (numParams() != kNumParams) mpc::log("LooperX Pads: parameter count mismatch");
        for (int i = 0; i < kNumParams; ++i) last_[i].store(get(i));
        std::vector<float> d((size_t)kNumParams);
        for (int i = 0; i < kNumParams; ++i) d[(size_t)i] = param(i).def;
        addProgram("Default", d);

        shared_ = openSharedLink();
        worker_ = std::thread([this] { workerLoop(); });
    }

    ~LooperXPads() override
    {
        quit_.store(true);
        if (worker_.joinable()) worker_.join();
        closeSharedLink(shared_);
    }

    // Botones de la pantalla: cualquier cambio desde la pantalla/Q-Link (hilo de audio) = pulsación.
    void parameterChanged(int index, float plain) override
    {
        if (index < 0 || index >= kNumParams) return;
        float prev = last_[index].exchange(plain);
        if (prev == plain || selfThread() != audioThread_.load(std::memory_order_relaxed)) return;
        if (index == kLearn) learnArmed_.store(!learnArmed_.load());
        else if (index >= kLoopBase && index < kLoopBase + kSlots) send(&LinkBlock::press, index - kLoopBase);
        else if (index == kStopAll) sendAll(&LinkBlock::stopAll);
        else if (index == kUndo) sendAll(&LinkBlock::undo);
    }

    void process(mpc::ProcessContext& ctx) override
    {
        audioThread_.store(selfThread(), std::memory_order_relaxed);
        const bool alive = watch_.update(shared_, ctx.numFrames, ctx.sampleRate);
        alive_.store(alive, std::memory_order_relaxed);
        const int chan = getInt(kChannel);
        const bool active = getBool(kActive), fourBanks = getInt(kLayout) == 0, cc = getBool(kCcCtrl);
        const int base = getInt(kBaseNote);
        for (int i = 0; i < ctx.numMidiEvents; ++i) {
            const mpc::MidiEvent& e = ctx.midi[i];
            if (chan > 0 && e.channel() != chan - 1) continue;
            if (e.isNoteOn()) {
                if (learnArmed_.exchange(false)) { learned_.store(e.data[1]); continue; }
                if (!active) continue;
                int k = e.data[1] - base;
                if (k < 0) continue;
                if (k < 16) send(&LinkBlock::press, k);
                else if (!fourBanks) continue;
                else if (k < 32) send(&LinkBlock::stop, k - 16);
                else if (k < 48) send(&LinkBlock::clear, k - 32);
                else if (k == 48) sendAll(&LinkBlock::stopAll);
                else if (k == 49) sendAll(&LinkBlock::undo);
            } else if (e.isController() && cc && active) {
                int n = e.data[1], v = e.data[2];
                if (n < kCcBase || n > kCcBase + 17) continue;
                bool down = v >= 64;
                bool was = ccDown_[n - kCcBase];
                ccDown_[n - kCcBase] = down;
                if (!down || was) continue;                 // flanco de subida
                if (n < kCcBase + 16) send(&LinkBlock::press, n - kCcBase);
                else if (n == kCcBase + 16) sendAll(&LinkBlock::stopAll);
                else sendAll(&LinkBlock::undo);
            }
        }
        // Sin sonido: las salidas ya vienen a cero (instrumento sin entradas).
        uint32_t sig = alive ? 1u : 0u;
        if (shared_) for (int s = 0; s < kSlots; ++s) sig = sig * 7u + (uint32_t)shared_->slot[s].state.load(std::memory_order_relaxed);
        sig += learnArmed_.load() ? 99991u : 0u;
        if (sig != lastSig_) { lastSig_ = sig; displayDirty_.store(true, std::memory_order_relaxed); }
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char b[48];
        const bool alive = shared_ && alive_.load(std::memory_order_relaxed);
        if (index == kBaseNote) { out = noteName((int)std::lround(plain)); return true; }
        if (index == kChannel) { int c = (int)std::lround(plain); if (c == 0) out = "ALL"; else { std::snprintf(b, sizeof b, "CH %d", c); out = b; } return true; }
        if (index == kLearn) { out = learnArmed_.load() ? "HIT PAD" : "LEARN"; return true; }
        if (index == kCcCtrl) { out = plain >= 0.5f ? "CC102-119" : "OFF"; return true; }
        if (index == kStatus) { out = !shared_ ? "NO LINK" : alive ? "LINKED" : "NO LOOPER"; return true; }
        if (index == kStopAll) { out = "STOP ALL"; return true; }
        if (index == kUndo) { out = "UNDO"; return true; }
        if (index >= kLoopBase && index < kLoopBase + kSlots) {
            if (!alive) { out = "-"; return true; }
            const LinkSlot& sl = shared_->slot[index - kLoopBase];
            int st = sl.state.load(std::memory_order_relaxed);
            float beats = sl.lengthBeats.load(std::memory_order_relaxed);
            int bpb = shared_->beatsPerBar.load(std::memory_order_relaxed);
            if ((st == kPlaying || st == kOverdub || st == kStopped) && beats > 0.5f && bpb > 0) {
                float bars = beats / (float)bpb;
                if (std::fabs(bars - std::round(bars)) < 0.01f) std::snprintf(b, sizeof b, "%s %db", slotStateName(st), (int)std::lround(bars));
                else std::snprintf(b, sizeof b, "%s %.0f/4", slotStateName(st), beats);
                out = b;
            } else out = slotStateName(st);
            return true;
        }
        return false;
    }

private:
    void send(std::atomic<uint32_t> (LinkBlock::*arr)[kSlots], int s)
    {
        if (shared_ && s >= 0 && s < kSlots) (shared_->*arr)[s].fetch_add(1, std::memory_order_acq_rel);
    }
    void sendAll(std::atomic<uint32_t> LinkBlock::*m)
    {
        if (shared_) (shared_->*m).fetch_add(1, std::memory_order_acq_rel);
    }

    void workerLoop()
    {
        auto lastUpdate = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        while (!quit_.load()) {
            int n = learned_.exchange(-1);
            if (n >= 0) {                 // LEARN: el pad tocado pasa a ser LOOP 1
                set(kBaseNote, (float)n);
                notifyHostParameterChanged(kBaseNote);
                displayDirty_.store(true);
            }
            auto now = std::chrono::steady_clock::now();
            if (displayDirty_.load() && now - lastUpdate > std::chrono::milliseconds(300)) {
                displayDirty_.store(false);
                lastUpdate = now;
                requestDisplayUpdate();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }

    LinkBlock* shared_ = nullptr;
    LooperAliveWatch watch_;
    std::atomic<bool> alive_{false}, learnArmed_{false}, displayDirty_{false}, quit_{false};
    std::atomic<int> learned_{-1};
    std::atomic<uintptr_t> audioThread_{0};
    std::atomic<float> last_[kNumParams];
    bool ccDown_[18] = {};
    uint32_t lastSig_ = 0;
    std::thread worker_;
};

MPC_PLUGIN_ENTRY(LooperXPads)
