/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#include "GCImpl.hpp"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <atomic>
#include <mutex>

#include "ExternalRCRef.hpp"
#include "GlobalData.hpp"
#include "TestSupport.hpp"
#include "TracingGCTest.hpp"

using namespace kotlin;

namespace {

class GenerationalMarkAndSweepTest {
public:
    static constexpr bool kStickyMarks = true;

    ~GenerationalMarkAndSweepTest() {
        mm::GlobalsRegistry::Instance().ClearForTests();
        mm::ExternalRCRefRegistry::instance().clearForTests();
        mm::GlobalData::Instance().gc().ClearForTests();
        mm::GlobalData::Instance().allocator().clearForTests();
    }

    void SetUp() {}
};

} // namespace

// Full collections use the `cms` collector's concurrent marker: the same concurrency tests apply (copied from
// gc/cms/cpp/GCImplTest.cpp).
TYPED_TEST_P(TracingGCTest, WeakResurrectionAtMarkTermination) {
    std::vector<Mutator> mutators(kDefaultThreadCount);
    std::vector<test_support::RegularWeakReferenceImpl*> weaks(kDefaultThreadCount);
    std::vector<test_support::Object<Payload>*> roots(kDefaultThreadCount);

    // initialize threads
    for (int i = 0; i < kDefaultThreadCount; ++i) {
        mutators[i]
                .Execute([&, i](mm::ThreadData& threadData, Mutator& mutator) {
                    roots[i] = &AllocateObject(threadData);
                    mutator.AddGlobalRoot(roots[i]->header());

                    auto& weakReferee = AllocateObject(threadData);
                    auto& weakRef = [&threadData, &weakReferee]() -> test_support::RegularWeakReferenceImpl& {
                        ObjHolder holder;
                        return test_support::InstallWeakReference(threadData, weakReferee.header(), holder.slot());
                    }();
                    EXPECT_NE(weakRef.get(), nullptr);
                    weaks[i] = &weakRef;
                    mutator.AddGlobalRoot(weakRef.header());
                })
                .wait();
    }

    auto epoch = mm::GlobalData::Instance().gc().Schedule();
    std::atomic gcDone = false;

    // Spin until thread suspension is requested.
    while (!mm::IsThreadSuspensionRequested()) {
    }

    std::vector<std::future<void>> mutatorFutures;
    for (int i = 0; i < kDefaultThreadCount; ++i) {
        mutatorFutures.emplace_back(mutators[i].Execute([&, i](mm::ThreadData& threadData, Mutator& mutator) noexcept {
            while (!gc::mark::test_support::flushActionRequested() && !gcDone.load(std::memory_order_relaxed)) {
                safePoint(threadData);
            }

            threadData.gc().impl().mark_.onSafePoint();

            auto weakReferee = weaks[i]->get();
            (*roots[i])->field2 = weakReferee;
            bool resurrected = weakReferee != nullptr;

            while (!gcDone.load(std::memory_order_relaxed)) {
                safePoint(threadData);
            }

            if (resurrected) {
                EXPECT_NE(weaks[i]->get(), nullptr);
            } else {
                EXPECT_EQ(weaks[i]->get(), nullptr);
            }
        }));
    }

    mm::GlobalData::Instance().gc().WaitFinalizers(epoch);
    gcDone = true;

    for (auto& future : mutatorFutures) {
        future.wait();
    }

    for (int i = 0; i < kDefaultThreadCount; ++i) {
        mutators[i]
                .Execute([&, i](mm::ThreadData&, Mutator&) noexcept {
                    if (auto weakReferee = weaks[i]->get()) {
                        auto& extraObj = *mm::ExtraObjectData::Get(weakReferee);
                        extraObj.ClearRegularWeakReferenceImpl();
                        extraObj.Uninstall();
                        alloc::destroyExtraObjectData(extraObj);
                    }
                })
                .wait();
    }
}

TYPED_TEST_P(TracingGCTest, ReleaseStableRefDuringRSCollection) {
    std::vector<Mutator> mutators(kDefaultThreadCount);

    std::atomic<size_t> readyThreads = 0;
    std::atomic<bool> gcDone = false;

    std::vector<std::future<void>> mutatorFutures;
    for (int i = 0; i < kDefaultThreadCount; ++i) {
        mutatorFutures.push_back(mutators[i].Execute([&](mm::ThreadData& threadData, Mutator& mutator) {
            auto& obj = AllocateObject(threadData);
            mm::OwningExternalRCRef stableRef(obj.header());

            ++readyThreads;
            while (!mm::IsThreadSuspensionRequested()) {
            }

            mm::safePoint();

            mutator.AddStackRoot(obj.header());
            stableRef.reset();

            while (!gcDone) {
                mm::safePoint();
            }

            EXPECT_THAT(mutator.Alive(), testing::Contains(obj.header()));
        }));
    }

    while (readyThreads < kDefaultThreadCount) {
    }

    mm::GlobalData::Instance().gcScheduler().scheduleAndWaitFinalized();
    gcDone = true;

    for (auto& future : mutatorFutures) {
        future.wait();
    }
}

TYPED_TEST_P(TracingGCTest, TerminateInSTW) {
    // Try to drive the concurrent mark to terminate in STW (after several unsuccessful concurrent attempts).
    // Construct a linked list of arrays and continuously steal from the list just one step ahead of the GC's mark front.

    constexpr int kListLength = 1000;
    constexpr int kArraySize = 200;
    // too many threads over-pace the GC
    constexpr int kMutatorsCount = 2;

    std::atomic gcDone = false;
    std::atomic ready = 0;

    std::vector<Mutator> mutators{kMutatorsCount};
    std::vector<std::future<void>> futures{};
    for (auto& mutator : mutators) {
        futures.emplace_back(mutator.Execute([&ready, &gcDone](mm::ThreadData& threadData, Mutator&mutator) {
            //prepare
            auto& global = mutator.AddGlobalRoot();
            auto& local = mutator.AddStackRoot();

            for (int i = 0; i < kListLength; ++i) {
                auto& next = AllocateArray<kArraySize>(threadData);
                next.elements()[kArraySize - 1] = global->field1.accessor().load();
                global->field1 = next.header();
            }
            local->field1 = global->field1.accessor().load();

            ++ready;

            // wait for GC
            while (!mm::IsThreadSuspensionRequested()) {}
            mm::safePoint(threadData);

            // run
            while (!gcDone.load(std::memory_order_relaxed)) {
                auto* nextPtr = local->field1.accessor().load()->array();
                if (nextPtr == nullptr) break;

                auto& next = test_support::ObjectArray<kArraySize>::FromArrayHeader(nextPtr);
                auto lastAccessor = next.elements()[kArraySize - 1].accessor();
                local->field1 = lastAccessor.load();
                lastAccessor.store(nullptr);

                while (!mm::test_support::safePointsAreActive() && !gcDone.load(std::memory_order_relaxed)) {}
                mm::safePoint(threadData);
            }

            // let the GC catch up if we were too fast
            while (!gcDone) {
                mm::safePoint(threadData);
            }
        }));
    }

    while (ready < kMutatorsCount) {}

    mm::GlobalData::Instance().gcScheduler().scheduleAndWaitFinalized();
    gcDone = true;

    for (auto& future : futures) {
        future.wait();
    }
}

// The shared conformance suite. Every collection it requests is a full one (`GC::Schedule()`).
REGISTER_TYPED_TEST_SUITE_WITH_LISTS(
        TracingGCTest, TRACING_GC_TEST_LIST,
        WeakResurrectionAtMarkTermination,
        ReleaseStableRefDuringRSCollection,
        TerminateInSTW
);
INSTANTIATE_TYPED_TEST_SUITE_P(GEN, TracingGCTest, GenerationalMarkAndSweepTest);

// ---------------------------------------------------------------------------------------------------------------------
// Generational behaviour.
// ---------------------------------------------------------------------------------------------------------------------

namespace {

class GenerationalGCTest : public testing::Test {
public:
    GenerationalGCTest() {
        // The tests choose young or full collections explicitly: keep the heap-size policy from upgrading a young
        // collection to a full one (the allocator's byte counters are not reset between tests).
        mm::GlobalData::Instance().gcScheduler().config().targetHeapBytes = std::numeric_limits<int64_t>::max() / 2;
        mm::GlobalData::Instance().gc().ClearForTests();
    }

    ~GenerationalGCTest() override {
        mm::GlobalsRegistry::Instance().ClearForTests();
        mm::ExternalRCRefRegistry::instance().clearForTests();
        mm::GlobalData::Instance().gc().ClearForTests();
        mm::GlobalData::Instance().allocator().clearForTests();
    }

private:
    FinalizerHooksTestSupport finalizerHooks_;
};

gc::internal::GenerationalGC& generationalGC() {
    return mm::GlobalData::Instance().gc().impl().gc_;
}

void collect(bool full) {
    auto& gc = mm::GlobalData::Instance().gc();
    auto epoch = full ? gc.Schedule() : gc.ScheduleYoung();
    NativeOrUnregisteredThreadGuard guard(/* reentrant = */ true);
    gc.WaitFinalizers(epoch);
}

void collectYoung() {
    collect(false);
    ASSERT_THAT(generationalGC().lastCollectionWasYoung(), std::optional<bool>(true));
}

bool isOld(ObjHeader* object) {
    return alloc::objectDataForObject(object).old();
}

void collectFull() {
    collect(true);
    ASSERT_THAT(generationalGC().lastCollectionWasYoung(), std::optional<bool>(false));
}

} // namespace

TEST_F(GenerationalGCTest, YoungCollectionFreesYoungGarbage) {
    RunInNewThread([](mm::ThreadData& threadData) {
        GlobalObjectHolder global{threadData};
        auto& garbage1 = AllocateObject(threadData);
        auto& garbage2 = AllocateObject(threadData);
        ASSERT_THAT(Alive(threadData), testing::UnorderedElementsAre(global.header(), garbage1.header(), garbage2.header()));

        collectYoung();

        EXPECT_THAT(Alive(threadData), testing::UnorderedElementsAre(global.header()));
        EXPECT_TRUE(isOld(global.header())) << "survivors are promoted";
    });
}

TEST_F(GenerationalGCTest, OldGarbageSurvivesYoungCollectionsUntilFullCollection) {
    RunInNewThread([](mm::ThreadData& threadData) {
        GlobalObjectHolder global{threadData};
        auto& object = AllocateObject(threadData);
        global->field1 = object.header();
        collectYoung();
        ASSERT_THAT(Alive(threadData), testing::UnorderedElementsAre(global.header(), object.header()));
        ASSERT_TRUE(isOld(object.header()));

        global->field1 = nullptr;
        collectYoung();
        EXPECT_THAT(Alive(threadData), testing::UnorderedElementsAre(global.header(), object.header()));

        collectFull();
        EXPECT_THAT(Alive(threadData), testing::UnorderedElementsAre(global.header()));
    });
}

TEST_F(GenerationalGCTest, OldToYoungFieldStoreKeepsYoungObjectAlive) {
    RunInNewThread([](mm::ThreadData& threadData) {
        GlobalObjectHolder global{threadData};
        collectFull();
        ASSERT_TRUE(isOld(global.header()));

        // Only reachable through the old global object: a young collection must find the edge via the barrier.
        ObjHeader* young;
        ObjHeader* youngChild;
        {
            auto& object = AllocateObject(threadData);
            auto& child = AllocateObject(threadData);
            object->field1 = child.header(); // young -> young
            global->field2 = object.header(); // old -> young
            young = object.header();
            youngChild = child.header();
        }
        auto& garbage = AllocateObject(threadData);

        collectYoung();

        EXPECT_THAT(Alive(threadData), testing::UnorderedElementsAre(global.header(), young, youngChild));
        EXPECT_THAT(Alive(threadData), testing::Not(testing::Contains(garbage.header())));
    });
}

TEST_F(GenerationalGCTest, OldToYoungArrayStoreKeepsYoungObjectAlive) {
    RunInNewThread([](mm::ThreadData& threadData) {
        GlobalObjectArrayHolder array{threadData};
        collectFull();
        ASSERT_TRUE(isOld(array.header()));

        ObjHeader* young;
        {
            auto& object = AllocateObject(threadData);
            array[2] = object.header();
            young = object.header();
        }

        collectYoung();

        EXPECT_THAT(Alive(threadData), testing::UnorderedElementsAre(array.header(), young));
    });
}

TEST_F(GenerationalGCTest, OverwrittenOldToYoungEdgeDoesNotRetain) {
    RunInNewThread([](mm::ThreadData& threadData) {
        GlobalObjectHolder global{threadData};
        collectFull();

        {
            auto& object = AllocateObject(threadData);
            global->field1 = object.header();
        }
        global->field1 = nullptr;

        collectYoung();

        EXPECT_THAT(Alive(threadData), testing::UnorderedElementsAre(global.header()));
    });
}

TEST_F(GenerationalGCTest, StoreIntoDeadYoungObjectDoesNotRetain) {
    RunInNewThread([](mm::ThreadData& threadData) {
        GlobalObjectHolder global{threadData};
        collectFull();

        {
            auto& holder = AllocateObject(threadData);
            auto& object = AllocateObject(threadData);
            holder->field1 = object.header(); // recorded slot, but its owner is young and unreachable
        }

        collectYoung();

        EXPECT_THAT(Alive(threadData), testing::UnorderedElementsAre(global.header()));
    });
}

TEST_F(GenerationalGCTest, HolderAwareBarrierKeepsYoungObjectAlive) {
    RunInNewThread([](mm::ThreadData& threadData) {
        GlobalObjectHolder global{threadData};
        collectFull();

        ObjHeader* young;
        {
            auto& object = AllocateObject(threadData);
            young = object.header();
            // A store that bypasses `beforeHeapRefUpdate` and reports the edge explicitly instead.
            global->field3.direct().store(young);
            gc::afterReferenceStoreInObject(global.header(), young);
        }
        EXPECT_TRUE(alloc::objectDataForObject(global.header()).remembered());

        collectYoung();

        EXPECT_THAT(Alive(threadData), testing::UnorderedElementsAre(global.header(), young));
        EXPECT_FALSE(alloc::objectDataForObject(global.header()).remembered());
    });
}

TEST_F(GenerationalGCTest, WeakReferenceAttachedToOldObject) {
    RunInNewThread([](mm::ThreadData& threadData) {
        GlobalObjectHolder global{threadData};
        collectFull();

        ObjHeader* weak;
        {
            ObjHolder holder;
            weak = test_support::InstallWeakReference(threadData, global.header(), holder.slot()).header();
            // `mm::createRegularWeakReferenceImpl` reports this edge; the test helper links it directly.
            gc::afterReferenceStoreInObject(global.header(), weak);
        }

        collectYoung();

        EXPECT_THAT(Alive(threadData), testing::UnorderedElementsAre(global.header(), weak));
    });
}

TEST_F(GenerationalGCTest, EdgesRecordedByExitedThreadAreKept) {
    RunInNewThread([](mm::ThreadData& threadData) {
        GlobalObjectHolder global{threadData};
        collectFull();

        ObjHeader* young = nullptr;
        RunInNewThread([&](mm::ThreadData& otherThreadData) {
            auto& object = AllocateObject(otherThreadData);
            global->field1 = object.header();
            young = object.header();
            // A real thread exit publishes the thread's objects; the test harness's teardown would instead destroy
            // unpublished ones (legacy allocator).
            otherThreadData.allocator().prepareForGC();
        });

        collectYoung();

        EXPECT_THAT(Alive(threadData), testing::Contains(young));
    });
}

TEST_F(GenerationalGCTest, FullCollectionClearsRememberedSets) {
    RunInNewThread([](mm::ThreadData& threadData) {
        GlobalObjectHolder global{threadData};
        collectFull();

        {
            auto& object = AllocateObject(threadData);
            global->field1 = object.header();
        }
        global->field1 = nullptr;

        collectFull();
        EXPECT_THAT(Alive(threadData), testing::UnorderedElementsAre(global.header()));

        // A stale slot from before the full collection must not be resurrected or dereferenced.
        collectYoung();
        EXPECT_THAT(Alive(threadData), testing::UnorderedElementsAre(global.header()));
    });
}

TEST_F(GenerationalGCTest, StoreInObjectIntoOldHolderLogsHolder) {
    RunInNewThread([](mm::ThreadData& threadData) {
        GlobalObjectArrayHolder array{threadData};
        collectFull();

        ObjHeader* young;
        {
            auto& object = AllocateObject(threadData);
            young = object.header();
            UpdateHeapRefInObject(array.header(), array[1].ptr(), young);
        }
        auto& rememberedSet = threadData.gc().impl().rememberedSet();
        EXPECT_THAT(rememberedSet.objects, testing::ElementsAre(array.header()));
        EXPECT_THAT(rememberedSet.slots, testing::IsEmpty());

        collectYoung();

        EXPECT_THAT(Alive(threadData), testing::UnorderedElementsAre(array.header(), young));
    });
}

TEST_F(GenerationalGCTest, StoreInObjectIntoYoungHolderRecordsNothing) {
    RunInNewThread([](mm::ThreadData& threadData) {
        auto& holder = AllocateObject(threadData);
        auto& object = AllocateObject(threadData);
        UpdateHeapRefInObject(holder.header(), holder->field1.ptr(), object.header());

        auto& rememberedSet = threadData.gc().impl().rememberedSet();
        EXPECT_THAT(rememberedSet.objects, testing::IsEmpty());
        EXPECT_THAT(rememberedSet.slots, testing::IsEmpty());
        EXPECT_THAT(holder->field1.direct().load(), object.header());
    });
}
