// lx_link.h - LooperX: estado compartido entre los plugins LooperX dentro del proceso del MPC.
//
// SPDX-License-Identifier: MIT
//
// El MPC carga todos los plugins dentro de su propio proceso (host-contract.md: "the plugin runs
// inside the MPC process"). Cada plugin es una .so distinta, así que no comparten variables
// estáticas; para que "LooperX Pads" dispare los loops de "LooperX Looper" y "LooperX Granular"
// pueda granular un loop en vivo, los tres mapean el mismo bloque de memoria con nombre
// (/dev/shm/looperx-link-<uid>, o /tmp si /dev/shm no existe).
//
// IMPORTANTE: esto NO forma parte del contrato documentado del SDK de Akai. Es memoria compartida
// normal de Linux entre librerías del mismo proceso (sin tocar el sistema). Si no se puede abrir,
// cada plugin funciona solo y el enlace aparece como OFF.
//
// Todo lo que hay en el bloque son atómicos de 32 bits sin locks: el audio de cada plugin nunca
// espera a otro. La memoria de audio de los loops es del Looper; aquí solo se publica su
// dirección base y los índices de sus bloques ("chunks"), con un contador de lectores para que el
// Looper no la libere mientras otro plugin la está leyendo.
#ifndef LX_LINK_H
#define LX_LINK_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lx {

constexpr int kSlots = 16;
constexpr int kChunkShift = 14;                     // 16384 frames ≈ 0,37 s a 44,1 kHz
constexpr int kChunkFrames = 1 << kChunkShift;
constexpr int kChunkMask = kChunkFrames - 1;
constexpr int kMaxChunksPerSlot = 520;              // ≈ 193 s por loop
constexpr uint32_t kNoChunk = 0xFFFFFFFFu;
constexpr uint32_t kLinkMagic = 0x4C58534Bu;        // "LXSK"
constexpr uint32_t kLinkVersion = 3;

enum SlotState : int32_t { kEmpty = 0, kArmed, kRecording, kPlaying, kOverdub, kStopped };

inline const char* slotStateName(int s)
{
    switch (s) {
        case kEmpty: return "EMPTY";
        case kArmed: return "WAIT";
        case kRecording: return "REC";
        case kPlaying: return "PLAY";
        case kOverdub: return "DUB";
        case kStopped: return "STOP";
        default: return "?";
    }
}

struct LinkSlot
{
    std::atomic<int32_t> state;
    std::atomic<int32_t> lengthFrames;   // frames válidos del loop (0 si vacío)
    std::atomic<int32_t> usedChunks;
    std::atomic<uint32_t> generation;    // cambia cuando el contenido se rehace (nuevo/borrado)
    std::atomic<int32_t> recFrames;      // frames grabados durante REC
    std::atomic<float> lengthBeats;
    std::atomic<float> phase;            // 0..1, posición de reproducción (para pantallas)
    std::atomic<uint32_t> chunk[kMaxChunksPerSlot];  // índices en el pool del Looper
};

struct LinkBlock
{
    uint32_t magic;
    uint32_t version;
    std::atomic<int32_t> pid;
    std::atomic<uint32_t> ownerToken;    // instancia de Looper que publica sus loops (0 = ninguna)
    std::atomic<uint32_t> heartbeat;     // el Looper lo incrementa en cada bloque de audio
    std::atomic<uintptr_t> poolBase;     // float* del pool del Looper en ESTE proceso (0 = no hay)
    std::atomic<int32_t> poolChunks;
    std::atomic<int32_t> readers;        // otros plugins leyendo el pool ahora mismo
    // Órdenes desde LooperX Pads (contadores: cada incremento = una pulsación)
    std::atomic<uint32_t> press[kSlots];
    std::atomic<uint32_t> stop[kSlots];
    std::atomic<uint32_t> clear[kSlots];
    std::atomic<uint32_t> stopAll;
    std::atomic<uint32_t> undo;
    // Información para pantallas
    std::atomic<float> bpm;
    std::atomic<int32_t> beatsPerBar;
    std::atomic<int32_t> lengthSetting;  // largo elegido en el Looper (0 = AUTO)
    LinkSlot slot[kSlots];
};

// Lectura segura del pool del Looper desde otro plugin (Dekker con seq_cst: o el lector ve base 0,
// o el Looper ve readers > 0 y espera antes de liberar).
class PoolReadGuard
{
public:
    explicit PoolReadGuard(LinkBlock* l) : l_(l)
    {
        if (!l_) return;
        l_->readers.fetch_add(1, std::memory_order_seq_cst);
        base_ = reinterpret_cast<const float*>(l_->poolBase.load(std::memory_order_seq_cst));
        chunks_ = l_->poolChunks.load(std::memory_order_seq_cst);
    }
    ~PoolReadGuard() { if (l_) l_->readers.fetch_sub(1, std::memory_order_seq_cst); }
    const float* base() const { return base_; }
    int chunks() const { return chunks_; }
    const float* chunkPtr(uint32_t idx) const
    {
        if (!base_ || idx >= (uint32_t)chunks_) return nullptr;
        return base_ + (size_t)idx * (size_t)kChunkFrames * 2;
    }
private:
    LinkBlock* l_ = nullptr;
    const float* base_ = nullptr;
    int chunks_ = 0;
};

// Abre (o crea) el bloque compartido. Devuelve nullptr si no se puede: el plugin funciona solo.
// Se llama desde el constructor del plugin (hilo de UI), nunca desde el audio.
inline LinkBlock* openSharedLink(bool* createdFresh = nullptr)
{
    const size_t size = sizeof(LinkBlock);
    char name[96];
    std::snprintf(name, sizeof name, "looperx-link-%u", (unsigned)getuid());
    const char* dirs[2] = {"/dev/shm/", "/tmp/"};
    for (const char* d : dirs) {
        char path[160];
        std::snprintf(path, sizeof path, "%s%s", d, name);
        int fd = ::open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) continue;
        struct stat st {};
        if (fstat(fd, &st) != 0) { ::close(fd); continue; }
        if ((size_t)st.st_size < size && ftruncate(fd, (off_t)size) != 0) { ::close(fd); continue; }
        void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) continue;
        auto* l = static_cast<LinkBlock*>(p);
        const int32_t me = (int32_t)getpid();
        bool fresh = false;
        // Bloque nuevo, de otra versión o de un proceso MPC anterior: se reinicia.
        if (l->magic != kLinkMagic || l->version != kLinkVersion || l->pid.load() != me) {
            std::memset(p, 0, size);
            for (auto& s : l->slot)
                for (auto& c : s.chunk) c.store(kNoChunk, std::memory_order_relaxed);
            l->version = kLinkVersion;
            l->pid.store(me);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            l->magic = kLinkMagic;
            fresh = true;
        }
        if (createdFresh) *createdFresh = fresh;
        return l;
    }
    return nullptr;
}

inline void closeSharedLink(LinkBlock* l)
{
    if (l) munmap(l, sizeof(LinkBlock));
}

// Bloque privado (cuando no hay enlace o el Looper no es el dueño).
inline LinkBlock* newPrivateLink()
{
    void* p = std::calloc(1, sizeof(LinkBlock));
    if (!p) return nullptr;
    auto* l = static_cast<LinkBlock*>(p);
    for (auto& s : l->slot)
        for (auto& c : s.chunk) c.store(kNoChunk, std::memory_order_relaxed);
    l->magic = kLinkMagic;
    l->version = kLinkVersion;
    return l;
}

inline void freePrivateLink(LinkBlock* l) { std::free(l); }

// ¿Hay un Looper vivo publicando? (el latido avanza)
class LooperAliveWatch
{
public:
    // Llamar con regularidad (p. ej. una vez por bloque de audio, ~3 ms). Devuelve true si el
    // latido ha cambiado en el último segundo aproximadamente.
    bool update(LinkBlock* l, int framesThisBlock, double sr)
    {
        if (!l) return false;
        uint32_t hb = l->heartbeat.load(std::memory_order_relaxed);
        if (hb != last_) { last_ = hb; silentFrames_ = 0; }
        else silentFrames_ += framesThisBlock;
        return l->ownerToken.load(std::memory_order_relaxed) != 0 && silentFrames_ < (int64_t)(sr * 1.0);
    }
private:
    uint32_t last_ = 0;
    int64_t silentFrames_ = 1 << 30;
};

}  // namespace lx

#endif
