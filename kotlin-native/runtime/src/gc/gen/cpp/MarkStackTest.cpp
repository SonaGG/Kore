/*
 * Copyright 2026 Sona Interactive, Inc. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the license/LICENSE.txt file.
 */

#include "MarkStack.hpp"

#include <atomic>
#include <list>
#include <memory>
#include <random>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "concurrent/ScopedThread.hpp"
#include "TestSupport.hpp"
#include "concurrent/ParallelProcessor.hpp"

using namespace kotlin;

namespace {

struct Node {
    std::atomic<bool> marked = false;
    std::atomic<int> visits = 0;
    std::vector<Node*> children;
};

using Stack = gc::ChunkedStack<Node, 512>;

} // namespace

TEST(ChunkedStackTest, PushPopAcrossChunks) {
    std::vector<Node> nodes(3 * 512 + 7);
    Stack stack;
    EXPECT_TRUE(stack.empty());
    EXPECT_EQ(stack.try_pop_front(), nullptr);
    for (auto& node : nodes) EXPECT_TRUE(stack.try_push_front(node));
    EXPECT_FALSE(stack.empty());
    EXPECT_EQ(stack.size(), nodes.size());
    for (size_t i = nodes.size(); i-- > 0;) EXPECT_EQ(stack.try_pop_front(), &nodes[i]);
    EXPECT_TRUE(stack.empty());
    EXPECT_EQ(stack.try_pop_front(), nullptr);
    // Reuse after emptying.
    stack.try_push_front(nodes[0]);
    EXPECT_EQ(stack.try_pop_front(), &nodes[0]);
    EXPECT_TRUE(stack.empty());
}

TEST(ChunkedStackTest, Splice) {
    std::vector<Node> nodes(2000);
    Stack source;
    for (auto& node : nodes) source.try_push_front(node);
    Stack batch;
    // A batch takes at most its size: the partial top chunk (2000 = 3 * 512 + 464) whole, then single elements.
    EXPECT_EQ(batch.splice_after(batch.before_begin(), source.before_begin(), source.end(), 512), 512u);
    EXPECT_EQ(batch.size(), 512u);
    EXPECT_EQ(source.size(), 2000u - 512u);
    Stack batch2;
    EXPECT_EQ(batch2.splice_after(batch2.before_begin(), source.before_begin(), source.end(), 512), 512u);
    // Less than a chunk: element by element.
    Stack small;
    EXPECT_EQ(small.splice_after(small.before_begin(), source.before_begin(), source.end(), 10), 10u);
    // Everything into one stack.
    Stack all;
    all.splice_after(all.before_begin(), batch.before_begin(), batch.end(), std::numeric_limits<size_t>::max());
    all.splice_after(all.before_begin(), batch2.before_begin(), batch2.end(), std::numeric_limits<size_t>::max());
    all.splice_after(all.before_begin(), small.before_begin(), small.end(), std::numeric_limits<size_t>::max());
    all.splice_after(all.before_begin(), source.before_begin(), source.end(), std::numeric_limits<size_t>::max());
    EXPECT_TRUE(batch.empty());
    EXPECT_TRUE(batch2.empty());
    EXPECT_TRUE(small.empty());
    EXPECT_TRUE(source.empty());
    EXPECT_EQ(all.size(), nodes.size());
    std::vector<bool> seen(nodes.size(), false);
    while (Node* node = all.try_pop_front()) {
        size_t index = node - nodes.data();
        EXPECT_FALSE(seen[index]);
        seen[index] = true;
    }
    for (bool s : seen) EXPECT_TRUE(s);
}

TEST(ChunkedStackTest, Move) {
    std::vector<Node> nodes(600);
    Stack a;
    for (auto& node : nodes) a.try_push_front(node);
    Stack b(std::move(a));
    EXPECT_TRUE(a.empty());
    EXPECT_EQ(b.size(), nodes.size());
    Stack c;
    c.try_push_front(nodes[0]);
    c = std::move(b);
    EXPECT_EQ(c.size(), nodes.size());
    c.clear();
    EXPECT_TRUE(c.empty());
}

namespace {

// A parallel graph traversal like the mark: whoever sets a node's mark pushes it. Few shared batches, so that workers
// overflow into their own lists.
template <size_t kBatchSize, size_t kPoolSize>
void traverseInParallel(size_t nodeCount, size_t edgesPerNode, size_t threadCount, uint32_t seed) {
    using Processor = ParallelProcessor<Stack, kBatchSize, kPoolSize>;
    std::vector<Node> nodes(nodeCount);
    std::mt19937 random(seed);
    for (size_t i = 0; i < nodeCount; ++i) {
        // A spine reaches everything from node 0; random edges make the work irregular.
        if (i + 1 < nodeCount) nodes[i].children.push_back(&nodes[i + 1]);
        for (size_t e = 0; e < edgesPerNode; ++e) nodes[i].children.push_back(&nodes[random() % nodeCount]);
    }
    Processor processor;
    auto work = [&](typename Processor::Worker& worker) {
        while (Node* node = worker.tryPop()) {
            node->visits.fetch_add(1);
            for (Node* child : node->children) {
                if (!child->marked.exchange(true)) worker.tryPush(*child);
            }
        }
    };
    std::atomic<bool> start = false;
    std::list<ScopedThread> threads;
    std::vector<std::unique_ptr<typename Processor::Worker>> workers;
    for (size_t i = 0; i < threadCount; ++i) workers.push_back(std::make_unique<typename Processor::Worker>(processor));
    nodes[0].marked = true;
    workers[0]->tryPush(nodes[0]);
    for (size_t i = 0; i < threadCount; ++i) {
        threads.emplace_back([&, i] {
            while (!start.load()) {
            }
            work(*workers[i]);
        });
    }
    start = true;
    for (auto& thread : threads) thread.join();
    for (auto& worker : workers) EXPECT_TRUE(worker->retainsNoWork());
    workers.clear();
    for (auto& node : nodes) {
        EXPECT_TRUE(node.marked.load());
        EXPECT_EQ(node.visits.load(), 1);
    }
}

} // namespace

TEST(ChunkedStackTest, ParallelTraversal) {
    for (uint32_t seed = 1; seed <= 5; ++seed) {
        traverseInParallel<512, 4>(200000, 3, kDefaultThreadCount, seed);
    }
}

TEST(ChunkedStackTest, ParallelTraversalSmallBatches) {
    for (uint32_t seed = 1; seed <= 5; ++seed) {
        traverseInParallel<16, 2>(50000, 2, kDefaultThreadCount, seed);
    }
}
