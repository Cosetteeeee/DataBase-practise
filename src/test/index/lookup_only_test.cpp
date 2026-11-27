// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

/**
 * @file lookup_only_test.cpp
 * @brief 第 7 次实验课「Lab2 任务 1（B+树查找）」的独立验收测试。
 *
 * 为什么需要这个文件：b_plus_tree_insert_test.cpp 两个用例都先调
 * insert_entry 再查，b_plus_tree_delete_test.cpp 又是先插后删；任务 1 完成时
 * 学生手上没有任何"只测查找"的测试，跑不通也不知道是查找写错还是插入写错。
 * index_node_test.cpp 测的是 IndexNode 的 RAII 解 pin，不是查找算法。
 *
 * 设计要点：**完全不调用 insert_entry**。通过框架已实现的 fetch_node() 拿到初始根页
 * （同时也是空树唯一的叶页），用 BPlusTreeNode 的 public setter 直接写入键值对，
 * 再检验任务 1 的四组 Todo：get_value / 树层 lower_bound、upper_bound /
 * 结点内 lower_bound、upper_bound、leaf_lookup，以及 IndexScan 的区间遍历。
 * 这样任务 1 一做完就能单独验收，不必等任务 2。
 *
 * 覆盖的教材章节：6.3.1节B+树叶结点结构、非叶结点结构，6.3.2节B+树的查找；
 * docs/RUCBase-Lab2[索引管理].md 任务 1(1)(2)、前言「普通索引与唯一索引（UNIQUE）」；
 * src/index/index_types.h、b_plus_tree.h、index_scan.h/.cpp。
 *
 * 使用方式见同目录 README-接入说明.md。
 */

#include <unistd.h>  // for chdir

#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "common/errors.h"
#include "index/b_plus_tree.h"
#include "index/index_manager.h"
#include "index/index_scan.h"
#include "index/index_types.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "system/sm_meta.h"

namespace {

const std::string TEST_DB_NAME = "LookupOnlyTest_db";
const std::string TEST_TABLE = "lookup_table";
const std::vector<std::string> TEST_COL = {"col1"};
const std::vector<ColMeta> TEST_COL_META = {
    {.tab_name = TEST_TABLE, .name = "col1", .type = TYPE_INT, .len = sizeof(int), .offset = 0, .index = true}};

/** 把 int 键转成骨架要求的 char* 键指针（TYPE_INT 列长度为 4）。 */
class IntKey {
public:
    explicit IntKey(int value) : value_(value) {}
    const char* data() const { return reinterpret_cast<const char*>(&value_); }

private:
    int value_;
};

class LookupOnlyTest : public ::testing::Test {
public:
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> buffer_pool_manager_;
    std::unique_ptr<IndexManager> index_manager_;
    std::unique_ptr<BPlusTree> tree_;
    PinSnapshot pin_baseline_;

    void SetUp() override {
        ::testing::Test::SetUp();
        disk_manager_ = std::make_unique<DiskManager>();
        buffer_pool_manager_ = std::make_unique<BufferPoolManager>(200, disk_manager_.get());
        index_manager_ = std::make_unique<IndexManager>(disk_manager_.get(), buffer_pool_manager_.get());

        if (!disk_manager_->is_dir(TEST_DB_NAME)) {
            disk_manager_->create_dir(TEST_DB_NAME);
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
        if (chdir(TEST_DB_NAME.c_str()) < 0) {
            throw UnixError();
        }
        // 清掉上次运行留下的索引文件，保证每次从空树开始。
        if (index_manager_->exists(TEST_TABLE, TEST_COL)) {
            index_manager_->destroy_index(TEST_TABLE, TEST_COL);
        }
        index_manager_->create_index(IndexMeta::make(TEST_TABLE, TEST_COL_META));
        tree_ = index_manager_->open_index(TEST_TABLE, TEST_COL);
        assert(tree_ != nullptr);
        pin_baseline_ = buffer_pool_manager_->get_pin_snapshot();
    }

    void TearDown() override {
        // 所有 IndexNode 都是局部对象，离开作用域应自动解 pin。
        EXPECT_EQ(buffer_pool_manager_->get_pin_snapshot(), pin_baseline_) << "buffer pool pin state changed";
        index_manager_->close_index(tree_.get());
        tree_.reset();
        index_manager_.reset();
        buffer_pool_manager_.reset();
        disk_manager_.reset();
        if (chdir("..") < 0) {
            throw UnixError();
        }
    }

    /**
     * @brief 不经过 insert_entry，直接把键值对写进初始根页（页 2）。
     *        只用 BPlusTreeNode 的 public setter，不访问任何私有成员。
     */
    void seed_root_leaf(const std::vector<int>& keys) {
        IndexNode node = tree_->fetch_node(INDEX_INITIAL_ROOT_PAGE);
        ASSERT_TRUE(node.valid());
        ASSERT_TRUE(node->is_leaf_page());
        for (size_t i = 0; i < keys.size(); ++i) {
            IntKey key(keys[i]);
            node->set_key(static_cast<int>(i), key.data());
            node->set_rid(static_cast<int>(i), Rid{.page_no = 100 + static_cast<int>(i), .slot_no = static_cast<int>(i)});
        }
        node->set_size(static_cast<int>(keys.size()));
        node.mark_dirty();
    }
};

/**
 * @brief 空树：位置规范的两个端点，以及"查不到"必须是明确的失败而不是位置越界。
 *
 * @note lab2，仅任务一 计分：10 points
 */
TEST_F(LookupOnlyTest, EmptyTreePositions) {
    // 空树时首叶 = 尾叶 = 初始根页，且该页 key_count = 0，故 begin == end。
    EXPECT_EQ(tree_->leaf_begin(), (IndexPosition{.page_no = INDEX_INITIAL_ROOT_PAGE, .slot_no = 0}));
    EXPECT_EQ(tree_->leaf_end(), (IndexPosition{.page_no = INDEX_INITIAL_ROOT_PAGE, .slot_no = 0}));
    EXPECT_EQ(tree_->leaf_begin(), tree_->leaf_end());

    IntKey key(7);
    std::vector<Rid> result;
    EXPECT_FALSE(tree_->get_value(key.data(), &result, nullptr));
    EXPECT_TRUE(result.empty());

    // 所有 key 都小于目标 key 时，lower/upper_bound 都应返回 leaf_end()。
    EXPECT_EQ(tree_->lower_bound(key.data()), tree_->leaf_end());
    EXPECT_EQ(tree_->upper_bound(key.data()), tree_->leaf_end());

    IndexScan scan(tree_.get(), tree_->leaf_begin(), tree_->leaf_end());
    EXPECT_TRUE(scan.is_end());
}

/**
 * @brief 单叶、含重复 key：验证 get_value 要取全、结点内/树层 lower-upper 语义一致，
 *        以及 [lower_bound(k), upper_bound(k)) 可直接交给 IndexScan 遍历。
 *        这正是文档 key=7 例子（跨叶子）在单叶内的最小版本。
 * 
 * @note lab2，仅任务一 计分：15 points
 */
TEST_F(LookupOnlyTest, SingleLeafDuplicateKeys) {
    // 下标:      0  1  2  3  4
    // key:       5  7  7  7  9
    // rid.slot:  0  1  2  3  4
    seed_root_leaf({5, 7, 7, 7, 9});

    IntKey k7(7);
    IntKey k5(5);
    IntKey k6(6);
    IntKey k9(9);
    IntKey k100(100);

    // (1) get_value 返回同一 key 的全部 Rid，且顺序与索引项顺序一致。
    std::vector<Rid> result;
    EXPECT_TRUE(tree_->get_value(k7.data(), &result, nullptr));
    ASSERT_EQ(result.size(), 3u);
    for (size_t i = 0; i < result.size(); ++i) {
        EXPECT_EQ(result[i], (Rid{.page_no = 101 + static_cast<int>(i), .slot_no = 1 + static_cast<int>(i)}));
    }

    // (2) 查不到的 key 必须返回 false 且不污染 result。
    std::vector<Rid> missing;
    EXPECT_FALSE(tree_->get_value(k6.data(), &missing, nullptr));
    EXPECT_TRUE(missing.empty());

    // (3) 树层 lower_bound / upper_bound 的半开区间。
    const IndexPosition lower7 = tree_->lower_bound(k7.data());
    const IndexPosition upper7 = tree_->upper_bound(k7.data());
    EXPECT_EQ(lower7, (IndexPosition{.page_no = INDEX_INITIAL_ROOT_PAGE, .slot_no = 1}));
    EXPECT_EQ(upper7, (IndexPosition{.page_no = INDEX_INITIAL_ROOT_PAGE, .slot_no = 4}));
    EXPECT_EQ(tree_->lower_bound(k5.data()), tree_->leaf_begin());
    // 目标比所有 key 都大时，upper_bound 落在最后一个叶子的末尾，即 leaf_end()。
    EXPECT_EQ(tree_->upper_bound(k9.data()), tree_->leaf_end());
    EXPECT_EQ(tree_->lower_bound(k100.data()), tree_->leaf_end());
    EXPECT_EQ(tree_->upper_bound(k100.data()), tree_->leaf_end());

    // (4) 结点内查找：lower_bound 是第一个 >=，upper_bound 是第一个 >，
    //     leaf_lookup 必须额外验证相等（返回位置不等于查找成功）。
    {
        IndexNode node = tree_->fetch_node(INDEX_INITIAL_ROOT_PAGE);
        ASSERT_TRUE(node.valid());
        EXPECT_EQ(node->lower_bound(k7.data()), 1);
        EXPECT_EQ(node->upper_bound(k7.data()), 4);
        EXPECT_EQ(node->lower_bound(k6.data()), 1) << "lower_bound(6) 应指向第一个 >6 的位置";
        Rid* value = nullptr;
        EXPECT_TRUE(node->leaf_lookup(k7.data(), &value));
        ASSERT_NE(value, nullptr);
        EXPECT_EQ(*value, (Rid{.page_no = 101, .slot_no = 1}));
        EXPECT_FALSE(node->leaf_lookup(k6.data(), &value));
    }

    // (5) IndexScan 沿 [lower7, upper7) 恰好扫出 3 条记录。
    IndexScan scan(tree_.get(), lower7, upper7);
    for (int i = 1; i <= 3; ++i) {
        ASSERT_FALSE(scan.is_end());
        EXPECT_EQ(scan.rid(), (Rid{.page_no = 100 + i, .slot_no = i}));
        scan.next();
    }
    EXPECT_TRUE(scan.is_end());

    // (6) IndexPosition 与 Rid 字段同名但语义不同：get_rid 完成地址空间转换。
    EXPECT_EQ(tree_->get_rid(lower7), (Rid{.page_no = 101, .slot_no = 1}));
}

}  // namespace
