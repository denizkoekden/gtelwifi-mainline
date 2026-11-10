// SPDX-License-Identifier: Apache-2.0
// In-process Codec2 contract test: no global HAL registration or audio playback.
#include <C2Component.h>
#include "AvcSyntax.h"
#include "M4vSyntax.h"
#include <C2ComponentFactory.h>
#include <C2Config.h>
#include <C2PlatformSupport.h>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>
#include <sys/resource.h>
using namespace android;
#if defined(SPRD_VP8_TEST) || defined(SPRD_MPEG4_TEST) || defined(SPRD_H263_TEST)
extern "C" C2ComponentFactory *CreateCodec2Factory();
extern "C" void DestroyCodec2Factory(C2ComponentFactory *);
// These wrappers are defined before the selected codec's factory aliases.
static C2ComponentFactory *createAvcFactory() { return CreateCodec2Factory(); }
static void destroyAvcFactory(C2ComponentFactory *f) { DestroyCodec2Factory(f); }
#ifdef SPRD_VP8_TEST
extern "C" C2ComponentFactory *CreateVp8Codec2Factory();
extern "C" void DestroyVp8Codec2Factory(C2ComponentFactory *);
#define CreateCodec2Factory CreateVp8Codec2Factory
#define DestroyCodec2Factory DestroyVp8Codec2Factory
#elif defined(SPRD_MPEG4_TEST)
extern "C" C2ComponentFactory *CreateMpeg4Codec2Factory();
extern "C" void DestroyMpeg4Codec2Factory(C2ComponentFactory *);
#define CreateCodec2Factory CreateMpeg4Codec2Factory
#define DestroyCodec2Factory DestroyMpeg4Codec2Factory
#else
extern "C" C2ComponentFactory *CreateH263Codec2Factory();
extern "C" void DestroyH263Codec2Factory(C2ComponentFactory *);
#define CreateCodec2Factory CreateH263Codec2Factory
#define DestroyCodec2Factory DestroyH263Codec2Factory
#endif
#else
extern "C" C2ComponentFactory *CreateCodec2Factory();
extern "C" void DestroyCodec2Factory(C2ComponentFactory *);
#endif
static void require(bool ok, const char *message) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
class Listener : public C2Component::Listener {
public:
    std::mutex mutex;
    std::condition_variable changed;
    std::set<uint64_t> outputs;
    uint64_t completed = 0;
    bool error = false, eos = false;
    FILE *pixels = nullptr;
    void onWorkDone_nb(std::weak_ptr<C2Component>, std::list<std::unique_ptr<C2Work>> works) override {
        std::lock_guard lock(mutex);
        for (auto &w : works) {
            if (w->result != C2_OK) { fprintf(stderr, "work result=%d\n", w->result); error = true; }
            for (const auto &wl : w->worklets) {
                const auto &o = wl->output;
                if (!(o.flags & C2FrameData::FLAG_INCOMPLETE)) ++completed;
                if (o.flags & C2FrameData::FLAG_END_OF_STREAM) eos = true;
                for (const auto &buffer : o.buffers) {
                    uint64_t index = o.ordinal.frameIndex.peekull();
                    if (!outputs.insert(index).second ||
                        o.ordinal.timestamp.peekll() != int64_t(index * 33333 + 9000000000ULL)) error = true;
                    for (const auto &block : buffer->data().graphicBlocks()) {
                        auto view = block.map().get();
                        if (view.error() != C2_OK) { error = true; continue; }
                        auto crop = view.crop();
                        if (pixels) {
                            const auto &l = view.layout();
                            for (uint32_t y = 0; y < crop.height; ++y)
                                if (fwrite(view.data()[0] + y * l.planes[0].rowInc, 1,
                                           crop.width, pixels) != crop.width) error = true;
                            for (uint32_t y = 0; y < crop.height / 2; ++y)
                                if (fwrite(view.data()[1] + y * l.planes[1].rowInc, 1,
                                           crop.width, pixels) != crop.width) error = true;
                        }
                    }
                    fprintf(stderr, "output index=%llu pts=%lld\n",
                        (unsigned long long)index, (long long)o.ordinal.timestamp.peekll());
                }
            }
        }
        changed.notify_all();
    }
    void onTripped_nb(std::weak_ptr<C2Component>,
                      std::vector<std::shared_ptr<C2SettingResult>>) override {
        std::lock_guard lock(mutex); error = true; changed.notify_all();
    }
    void onError_nb(std::weak_ptr<C2Component>, uint32_t code) override {
        std::lock_guard lock(mutex); fprintf(stderr, "component error=%u\n", code);
        error = true; changed.notify_all();
    }
};
static std::vector<std::vector<uint8_t>> readUnits(const char *name) {
    FILE *f = fopen(name, "rb"); require(f, "open AU file");
    std::vector<std::vector<uint8_t>> result;
    for (;;) {
        uint8_t b[4]; size_t n = fread(b, 1, 4, f);
        if (!n && feof(f)) break;
        require(n == 4, "truncated length");
        uint32_t size = b[0] | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
        require(size && size <= 2 * 1024 * 1024, "AU size");
        result.emplace_back(size);
        require(fread(result.back().data(), 1, size, f) == size, "truncated AU");
    }
    fclose(f); return result;
}
int main(int argc, char **argv) {
    require(argc >= 3 && argc <= 4, "usage: c2-sprd-avc-test INPUT.au EXPECTED_FRAMES [OUTPUT.nv12]");
    const auto original = readUnits(argv[1]);
    const unsigned rounds = getenv("VSP_C2_ROUNDS") ? strtoul(getenv("VSP_C2_ROUNDS"), nullptr, 10) : 1;
    require(rounds > 0 && rounds <= 10, "round count");
    size_t expected = strtoul(argv[2], nullptr, 10);
    auto *factory = CreateCodec2Factory(); require(factory, "factory");
    std::shared_ptr<C2Component> component;
    require(factory->createComponent(1, &component, [](C2Component *c) { delete c; }) == C2_OK, "create");
    auto listener = std::make_shared<Listener>();
    if (argc == 4) { listener->pixels = fopen(argv[3], "wb"); require(listener->pixels, "open pixels"); }
    require(component->setListener_vb(listener, C2_MAY_BLOCK) == C2_OK, "listener");
    std::shared_ptr<C2BlockPool> linear;
    require(GetCodec2BlockPool(C2BlockPool::BASIC_LINEAR, component, &linear) == C2_OK, "linear pool");
    require(component->start() == C2_OK, "start");
#if defined(SPRD_VP8_TEST) || defined(SPRD_MPEG4_TEST) || defined(SPRD_H263_TEST)
    auto *avcFactory = createAvcFactory(); require(avcFactory, "AVC lease test factory");
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        std::shared_ptr<C2Component> blocked;
        require(avcFactory->createComponent(2 + attempt, &blocked,
            [](C2Component *c) { delete c; }) == C2_OK, "create competing AVC");
        require(blocked->setListener_vb(std::make_shared<Listener>(), C2_MAY_BLOCK) == C2_OK,
            "competing AVC listener");
        auto blockedStatus = blocked->start();
        fprintf(stderr, "competing AVC start=%d expected=%d\n", blockedStatus, C2_NO_MEMORY);
        require(blockedStatus == C2_NO_MEMORY, "decoder excludes competing AVC session");
        blocked->release(); blocked.reset();
    }
    destroyAvcFactory(avcFactory);
#endif
    for (unsigned round = 0; round < rounds; ++round) {
    auto units = original;
    if (round) {
        if (round & 1) {
            std::list<std::unique_ptr<C2Work>> flushed;
            require(component->flush_sm(C2Component::FLUSH_COMPONENT, &flushed) == C2_OK, "flush");
            require(flushed.empty(), "no orphaned work after EOS");
        } else {
            require(component->stop() == C2_OK, "restart stop");
            require(component->start() == C2_OK, "restart start");
        }
        // AVC reuses cached CSD. VP8 instead requires its real keyframe again.
#if !defined(SPRD_VP8_TEST) && !defined(SPRD_MPEG4_TEST) && !defined(SPRD_H263_TEST)
        for (auto &unit : units) {
            auto nals = sprd::splitAnnexB(unit.data(), unit.size()); require(bool(nals), "Annex B");
            std::vector<uint8_t> stripped;
            for (auto n : *nals) if (n.type != 7 && n.type != 8) {
                stripped.insert(stripped.end(), {0, 0, 0, 1});
                stripped.insert(stripped.end(), unit.begin() + n.offset, unit.begin() + n.offset + n.size);
            }
            unit = std::move(stripped);
        }
#elif defined(SPRD_MPEG4_TEST)
        auto first = sprd::m4v::mpeg4(units.front().data(), units.front().size());
        require(first && first->picture, "MPEG4 restart keyframe");
        auto lastSequence = first->sequence;
        for (const auto &unit : units) {
            auto a = sprd::m4v::mpeg4(unit.data(), unit.size(), lastSequence);
            require(bool(a), "MPEG4 resize test syntax");
            lastSequence = a->sequence;
        }
        // Cached CSD is reusable only for the sequence active at flush. A
        // resize vector loops back to a different sequence and must resend it.
        if (*first->sequence == *lastSequence)
            units.front().erase(units.front().begin(), units.front().begin() + first->pictureOffset);
#endif
        std::lock_guard lock(listener->mutex);
        listener->outputs.clear(); listener->completed = 0; listener->eos = false;
    }
    struct rusage before{}, after{}; getrusage(RUSAGE_SELF, &before);
    auto start = std::chrono::steady_clock::now();
    const bool dataEos = getenv("VSP_C2_DATA_EOS") != nullptr;
    const size_t workCount = units.size() + (dataEos ? 0 : 1);
    for (uint64_t index = 0; index < workCount; ++index) {
        auto work = std::make_unique<C2Work>();
        work->input.ordinal.frameIndex = index;
        work->input.ordinal.timestamp = int64_t(index * 33333 + 9000000000ULL);
        work->input.flags = index + 1 == workCount ? C2FrameData::FLAG_END_OF_STREAM : C2FrameData::flags_t(0);
        work->worklets.emplace_back(new C2Worklet);
        if (index < units.size()) {
            std::shared_ptr<C2LinearBlock> block;
            require(linear->fetchLinearBlock(units[index].size(),
                {C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE}, &block) == C2_OK, "allocate input");
            { auto view = block->map().get(); require(view.error() == C2_OK, "map input");
              memcpy(view.data(), units[index].data(), units[index].size()); }
            work->input.buffers.push_back(C2Buffer::CreateLinearBuffer(
                block->share(0, units[index].size(), C2Fence())));
        }
        std::list<std::unique_ptr<C2Work>> queue; queue.push_back(std::move(work));
        require(component->queue_nb(&queue) == C2_OK, "queue");
        // Bound the input queue without forcing output from B-frame-delayed work.
        if (index >= 20) {
            std::unique_lock lock(listener->mutex);
            require(listener->changed.wait_for(lock, std::chrono::seconds(20), [&] {
                return listener->error || listener->completed + 20 >= index;
            }) && !listener->error, "bounded queue progress");
        }
    }
    {
        std::unique_lock lock(listener->mutex);
        require(listener->changed.wait_for(lock, std::chrono::seconds(40), [&] {
            return listener->error || (listener->eos && listener->completed == workCount);
        }), "EOS timeout / pending work");
        require(!listener->error && listener->outputs.size() == expected, "output count / timestamp / status");
    }
    getrusage(RUSAGE_SELF, &after);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    double cpu = after.ru_utime.tv_sec - before.ru_utime.tv_sec +
        after.ru_stime.tv_sec - before.ru_stime.tv_sec +
        (after.ru_utime.tv_usec - before.ru_utime.tv_usec +
         after.ru_stime.tv_usec - before.ru_stime.tv_usec) * 1e-6;
    printf("PASS round=%u outputs=%zu completed=%llu seconds=%.3f fps=%.2f cpu_one_core=%.1f%%\n",
        round, listener->outputs.size(), (unsigned long long)listener->completed, seconds,
        expected / seconds, cpu / seconds * 100);
    }
    require(component->stop() == C2_OK, "stop");
    require(component->release() == C2_OK, "release");
    component.reset(); DestroyCodec2Factory(factory);
#if defined(SPRD_VP8_TEST) || defined(SPRD_MPEG4_TEST) || defined(SPRD_H263_TEST)
    avcFactory = createAvcFactory();
    std::shared_ptr<C2Component> available;
    require(avcFactory->createComponent(4, &available,
        [](C2Component *c) { delete c; }) == C2_OK, "create AVC after VP8 release");
    require(available->setListener_vb(std::make_shared<Listener>(), C2_MAY_BLOCK) == C2_OK,
        "released AVC listener");
    require(available->start() == C2_OK, "AVC lease available after decoder release");
    require(available->stop() == C2_OK, "stop AVC lease test");
    require(available->release() == C2_OK, "release AVC lease test");
    available.reset(); destroyAvcFactory(avcFactory);
#endif
    if (listener->pixels) require(!fclose(listener->pixels), "close pixels");
}
