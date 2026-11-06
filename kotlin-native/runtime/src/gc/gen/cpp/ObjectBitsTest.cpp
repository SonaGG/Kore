/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#include "ObjectData.hpp"

#include <atomic>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "ExtraObjectData.hpp"
#include "GlobalData.hpp"
#include "ObjectOps.hpp"
#include "ObjectTestSupport.hpp"
#include "TestSupport.hpp"
#include "concurrent/ScopedThread.hpp"

using namespace kotlin;

namespace {

struct Payload {
    mm::RefField field;

    static constexpr std::array kFields = {&Payload::field};
};

test_support::TypeInfoHolder typeHolder{test_support::TypeInfoHolder::ObjectBuilder<Payload>()};

} // namespace

// Kore: meta objects are installed by mutators (`ExtraObjectData::Install`) while the GC updates the bits of the same
// word (with the custom allocator, see `ObjectBits`): neither may lose the other's update.
TEST(ObjectBitsTest, MetaObjectInstallRacesWithBitUpdates) {
    constexpr size_t kCount = 100000;
    RunInNewThread([](mm::ThreadData& threadData) {
        // No collection runs: the objects need no roots.
        std::vector<ObjHeader*> objects(kCount);
        for (size_t i = 0; i < kCount; ++i) {
            mm::AllocateObject(&threadData, typeHolder.typeInfo(), &objects[i]);
        }
        std::atomic<bool> stop = false;
        std::atomic<size_t> rounds = 0;
        // Like markers, sweepers and barriers, on every object, until the meta objects are all installed.
        ScopedThread bitsThread([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                for (ObjHeader* object : objects) {
                    auto bits = gc::objectBits(object);
                    bits.tryMark();
                    bits.sweep(/* fullCollection = */ false);
                    bits.tryRemember();
                    bits.forget();
                    bits.reset(gc::ObjectBits::kOld);
                }
                rounds.fetch_add(1, std::memory_order_relaxed);
            }
        });
        while (rounds.load(std::memory_order_relaxed) == 0) {
        }
        for (ObjHeader* object : objects) {
            auto& extraObject = mm::ExtraObjectData::GetOrInstall(object);
            EXPECT_EQ(extraObject.GetBaseObject(), object);
        }
        stop = true;
        bitsThread.join();
        for (ObjHeader* object : objects) {
            EXPECT_TRUE(object->has_meta_object());
            EXPECT_EQ(object->type_info(), typeHolder.typeInfo());
            auto bits = gc::objectBits(object);
            EXPECT_EQ(bits.bits(), gc::ObjectBits::kOld);
            if (gc::ObjectBits::inHeader()) {
                EXPECT_EQ(bits.load() & ~gc::ObjectBits::kAll, reinterpret_cast<uintptr_t>(object->meta_object_or_null()));
            }
            // Unlinking keeps the bits too. (The legacy allocator uninstalls when destroying, the custom one does not.)
            auto& extraObject = *mm::ExtraObjectData::Get(object);
            if (gc::ObjectBits::inHeader()) extraObject.Uninstall();
            alloc::destroyExtraObjectData(extraObject);
            EXPECT_FALSE(object->has_meta_object());
            EXPECT_EQ(object->type_info(), typeHolder.typeInfo());
            EXPECT_EQ(bits.bits(), gc::ObjectBits::kOld);
        }
    });
    mm::GlobalData::Instance().allocator().clearForTests();
}

TEST(ObjectBitsTest, BitsKeepTheTypeInfo) {
    RunInNewThread([](mm::ThreadData& threadData) {
        ObjHeader* object = nullptr;
        mm::AllocateObject(&threadData, typeHolder.typeInfo(), &object);
        auto bits = gc::objectBits(object);
        EXPECT_EQ(bits.bits(), 0u);
        EXPECT_TRUE(bits.tryMark());
        EXPECT_FALSE(bits.tryMark());
        EXPECT_TRUE(bits.marked());
        EXPECT_TRUE(object->heap());
        EXPECT_EQ(object->type_info(), typeHolder.typeInfo());
        EXPECT_TRUE(bits.oldOrSurviving());
        EXPECT_TRUE(bits.tryRemember());
        EXPECT_FALSE(bits.tryRemember());
        EXPECT_TRUE(bits.sweep(/* fullCollection = */ true));
        EXPECT_EQ(bits.bits(), gc::ObjectBits::kOld | gc::ObjectBits::kRemembered);
        EXPECT_EQ(object->type_info(), typeHolder.typeInfo());
        EXPECT_FALSE(bits.sweep(/* fullCollection = */ true));
        EXPECT_TRUE(bits.sweep(/* fullCollection = */ false));
        bits.forget();
        bits.reset(0);
        EXPECT_EQ(bits.bits(), 0u);
        EXPECT_EQ(object->type_info(), typeHolder.typeInfo());
    });
    mm::GlobalData::Instance().allocator().clearForTests();
}
