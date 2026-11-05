/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#include "GCImpl.hpp"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "GlobalData.hpp"
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

// The shared conformance suite. Every collection it requests is a full one (`GC::Schedule()`).
REGISTER_TYPED_TEST_SUITE_WITH_LISTS(TracingGCTest, TRACING_GC_TEST_LIST);
INSTANTIATE_TYPED_TEST_SUITE_P(GEN, TracingGCTest, GenerationalMarkAndSweepTest);

REGISTER_TYPED_TEST_SUITE_WITH_LISTS(STWMarkGCTest, STW_MARK_GC_TEST_LIST);
INSTANTIATE_TYPED_TEST_SUITE_P(GEN, STWMarkGCTest, GenerationalMarkAndSweepTest);

// ---------------------------------------------------------------------------------------------------------------------
// Generational behaviour.
// ---------------------------------------------------------------------------------------------------------------------

namespace {

class GenerationalGCTest : public testing::Test {
public:
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
        EXPECT_TRUE(gc::isMarked(global.header())) << "survivors are promoted";
    });
}

TEST_F(GenerationalGCTest, OldGarbageSurvivesYoungCollectionsUntilFullCollection) {
    RunInNewThread([](mm::ThreadData& threadData) {
        GlobalObjectHolder global{threadData};
        auto& object = AllocateObject(threadData);
        global->field1 = object.header();
        collectYoung();
        ASSERT_THAT(Alive(threadData), testing::UnorderedElementsAre(global.header(), object.header()));
        ASSERT_TRUE(gc::isMarked(object.header()));

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
        ASSERT_TRUE(gc::isMarked(global.header()));

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
        ASSERT_TRUE(gc::isMarked(array.header()));

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
