/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once

// Thread safety
// -------------
//
// Writes require external synchronization, most likely a mutex.
// Reads require a guarantee that the ObSkipList will not be destroyed
// while the read is in progress. Apart from that, reads progress
// without any internal locking or synchronization.
//
// Invariants:
//
// (1) Allocated nodes are never deleted until the ObSkipList is
// destroyed.  This is trivially guaranteed by the code since we
// never delete any skip list nodes.
//
// (2) The contents of a Node except for the next/prev pointers are
// immutable after the Node has been linked into the ObSkipList.
// Only insert() modifies the list, and it is careful to initialize
// a node and use release-stores to publish the nodes in one or
// more lists.
//
// ... prev vs. next pointer ordering ...

#include "common/math/random_generator.h"
#include "common/lang/atomic.h"
#include "common/lang/vector.h"
#include "common/log/log.h"

namespace oceanbase {
// Key：存储的键类型；
// ObComparator：自定义比较器，实现 Key 大小对比
// 模板类实现有序跳表，用于内存有序存储，支持单线程插入、并发无锁插入、范围迭代；MemTable 底层就是这个结构。
template <typename Key, class ObComparator>
class ObSkipList
{
private:
  // 私有前向声明
  struct Node;

public:
  /**
   * @brief Create a new ObSkipList object that will use "cmp" for comparing keys.
   */
  // explicit避免隐式转换
  explicit ObSkipList(ObComparator cmp);
  // 调表自己管理堆内存， 禁止拷贝构造
  ObSkipList(const ObSkipList &)            = delete;
  ObSkipList &operator=(const ObSkipList &) = delete;
  ~ObSkipList();

  /**
   * @brief Insert key into the list.
   * REQUIRES: nothing that compares equal to key is currently in the list
   */
  void insert(const Key &key);

  void insert_concurrently(const Key &key);

  /**
   * @brief Returns true if an entry that compares equal to key is in the list.
   *  @param [in] key
   *  @return true if found, false otherwise
   */
  bool contains(const Key &key) const;

  /**
   * @brief Iteration over the contents of a skip list
   */
  class Iterator
  {
  public:
    /**
     * @brief Initialize an iterator over the specified list.
     * @return The returned iterator is not valid.
     */
    // 初始化 迭代器是无效的
    explicit Iterator(const ObSkipList *list);

    /**
     * @brief Returns true iff the iterator is positioned at a valid node.
     */
    bool valid() const;

    /**
     * @brief Returns the key at the current position.
     * REQUIRES: valid()
     */
    const Key &key() const;

    /**
     * @brief Advance to the next entry in the list.
     * REQUIRES: valid()
     */
    void next();

    /**
     * @brief Advances to the previous position.
     * REQUIRES: valid()
     */
    void prev();

    /**
     * @brief Advance to the first entry with a key >= target
     */
    // 定位第一个键 >= target的节点
    void seek(const Key &target);

    /**
     * @brief Position at the first entry in list.
     * @note Final state of iterator is valid() iff list is not empty.
     */
    // 迭代器指向跳表中最小的键所在的节点
    void seek_to_first();

    /**
     * @brief Position at the last entry in list.
     * @note Final state of iterator is valid() iff list is not empty.
     */
    void seek_to_last();

  private:
    const ObSkipList *list_;
    Node             *node_;
  };

private:
  enum
  {
    kMaxHeight = 12
  };

  inline int get_max_height() const
  {
    // 宽松内存序，只保证原子本身读写，不做跨线程内存屏障。
    return max_height_.load(std::memory_order_relaxed);
  }
  // 新建跳表节点
  Node *new_node(const Key &key, int height);
  // 生成随机节点高度
  int  random_height();
  bool equal(const Key &a, const Key &b) const { return (compare_(a, b) == 0); }

  // Return the earliest node that comes at or after key.
  // Return nullptr if there is no such node.
  //
  // If prev is non-null, fills prev[level] with pointer to previous
  // node at "level" for every level in [0..max_height_-1].
  // 返回第一个 key ≥ 目标 key 的节点；无则返回 nullptr
  Node *find_greater_or_equal(const Key &key, Node **prev) const;

  // Return the latest node with a key < key.
  // Return head_ if there is no such node.
  // 返回最大的、key < 目标 key 的节点
  Node *find_less_than(const Key &key) const;

  // Return the last node in the list.
  // Return head_ if list is empty.
  Node *find_last() const;

  // Immutable after construction
  ObComparator const compare_;

  Node *const head_;

  // Modified only by insert().  Read racily by readers, but stale
  // values are ok.
  atomic<int> max_height_;  // Height of the entire list
};

// Implementation details follow
template <typename Key, class ObComparator>
struct ObSkipList<Key, ObComparator>::Node
{
  explicit Node(const Key &k) : key(k) {}

  Key const key;

  // Accessors/mutators for links.  Wrapped in methods so we can
  // add the appropriate barriers as necessary.
  Node *next(int n)
  {
    ASSERT(n >= 0, "n >= 0");
    // Use an 'acquire load' so that we observe a fully initialized
    // version of the returned Node.
    // acquire是读屏障
    return next_[n].load(std::memory_order_acquire);
  }
  void set_next(int n, Node *x)
  {
    ASSERT(n >= 0, "n >= 0");
    // Use a 'release store' so that anybody who reads through this
    // pointer observes a fully initialized version of the inserted node.
    // release写屏障
    next_[n].store(x, std::memory_order_release);
  }

  // No-barrier variants that can be safely used in a few locations.
  Node *nobarrier_next(int n)
  {
    ASSERT(n >= 0, "n >= 0");
    return next_[n].load(std::memory_order_relaxed);
  }
  void nobarrier_set_next(int n, Node *x)
  {
    ASSERT(n >= 0, "n >= 0");
    next_[n].store(x, std::memory_order_relaxed);
  }
  // 无锁修改指针
  bool cas_next(int n, Node *expected, Node *x)
  {
    ASSERT(n >= 0, "n >= 0");
    return next_[n].compare_exchange_strong(expected, x);
  }

private:
  // Array of length equal to the node height.  next_[0] is lowest level link.
  // 存储每一层的后继指针
  atomic<Node *> next_[1];
};

template <typename Key, class ObComparator>
typename ObSkipList<Key, ObComparator>::Node *ObSkipList<Key, ObComparator>::new_node(const Key &key, int height)
{
  char *const node_memory = reinterpret_cast<char *>(malloc(sizeof(Node) + sizeof(atomic<Node *>) * (height - 1)));
  return new (node_memory) Node(key);
}

template <typename Key, class ObComparator>
inline ObSkipList<Key, ObComparator>::Iterator::Iterator(const ObSkipList *list)
{
  list_ = list;
  node_ = nullptr;
}

template <typename Key, class ObComparator>
inline bool ObSkipList<Key, ObComparator>::Iterator::valid() const
{
  return node_ != nullptr;
}

template <typename Key, class ObComparator>
inline const Key &ObSkipList<Key, ObComparator>::Iterator::key() const
{
  ASSERT(valid(), "valid");
  return node_->key;
}

template <typename Key, class ObComparator>
inline void ObSkipList<Key, ObComparator>::Iterator::next()
{
  ASSERT(valid(), "valid");
  node_ = node_->next(0);
}

template <typename Key, class ObComparator>
inline void ObSkipList<Key, ObComparator>::Iterator::prev()
{
  // Instead of using explicit "prev" links, we just search for the
  // last node that falls before key.
  ASSERT(valid(), "valid");
  node_ = list_->find_less_than(node_->key);
  if (node_ == list_->head_) {
    node_ = nullptr;
  }
}

template <typename Key, class ObComparator>
inline void ObSkipList<Key, ObComparator>::Iterator::seek(const Key &target)
{
  node_ = list_->find_greater_or_equal(target, nullptr);
}

template <typename Key, class ObComparator>
inline void ObSkipList<Key, ObComparator>::Iterator::seek_to_first()
{
  node_ = list_->head_->next(0);
}

template <typename Key, class ObComparator>
inline void ObSkipList<Key, ObComparator>::Iterator::seek_to_last()
{
  node_ = list_->find_last();
  if (node_ == list_->head_) {
    node_ = nullptr;
  }
}

template <typename Key, class ObComparator>
int ObSkipList<Key, ObComparator>::random_height()
{
  // Increase height with probability 1 in kBranching
  static const unsigned int kBranching = 4;
  // 每个线程使用独立的随机数生成器，避免并发 insert 时对 mt19937 产生数据竞争
  static thread_local common::RandomGenerator tls_rnd;
  int                                         height = 1;
  while (height < this->kMaxHeight && tls_rnd.next(kBranching) == 0) {
    height++;
  }
  ASSERT(height > 0, "height > 0");
  ASSERT(height <= this->kMaxHeight, "height <= kMaxHeight");
  return height;
}

template <typename Key, class ObComparator>
typename ObSkipList<Key, ObComparator>::Node *ObSkipList<Key, ObComparator>::find_greater_or_equal(
    const Key &key, Node **prev) const
{
  Node *cur = this->head_;
  // 从最高层往下遍历。始终从 kMaxHeight 开始，保证 prev[0..kMaxHeight) 全部被填充：
  // 高于当前 max_height_ 的层，前驱自然落在 head_ 上（head_->next[level] 为 nullptr），
  // 避免并发插入时 prev 数组出现未初始化元素。
  for (int level = this->kMaxHeight - 1; level >= 0; level--) {
    // 每层向右走到最后一个 < key 的节点
    while (true) {
      Node *next_node = cur->next(level);
      // next_node 为空或 next_node->key >= key 时停止，此时 next_node 就是“第一个 >= key”的节点
      if (next_node == nullptr || this->compare_(next_node->key, key) >= 0) {
        break;
      }
      cur = next_node;
    }
    // 需要记录前驱则存入prev数组
    if (prev != nullptr) {
      prev[level] = cur;
    }
  }
  // 到第0层，下一个就是 >= key 的节点
  Node *succ = cur->next(0);
  return succ;
}

template <typename Key, class ObComparator>
typename ObSkipList<Key, ObComparator>::Node *ObSkipList<Key, ObComparator>::find_less_than(const Key &key) const
{
  Node *x     = this->head_;
  int   level = this->get_max_height() - 1;
  while (true) {
    ASSERT(x == this->head_ || this->compare_(x->key, key) < 0, "x == head_ || compare_(x->key, key) < 0");
    Node *next = x->next(level);
    if (next == nullptr || this->compare_(next->key, key) >= 0) {
      if (level == 0) {
        return x;
      } else {
        // Switch to next list
        level--;
      }
    } else {
      x = next;
    }
  }
}

template <typename Key, class ObComparator>
typename ObSkipList<Key, ObComparator>::Node *ObSkipList<Key, ObComparator>::find_last() const
{
  Node *x     = this->head_;
  int   level = this->get_max_height() - 1;
  while (true) {
    Node *next = x->next(level);
    if (next == nullptr) {
      if (level == 0) {
        return x;
      } else {
        // Switch to next list
        level--;
      }
    } else {
      x = next;
    }
  }
}

template <typename Key, class ObComparator>
ObSkipList<Key, ObComparator>::ObSkipList(ObComparator cmp)
    : compare_(cmp), head_(new_node(0 /* any key will do */, this->kMaxHeight)), max_height_(1)
{
  for (int i = 0; i < this->kMaxHeight; i++) {
    head_->set_next(i, nullptr);
  }
}

template <typename Key, class ObComparator>
ObSkipList<Key, ObComparator>::~ObSkipList()
{
  using Node = typename ObSkipList<Key, ObComparator>::Node;
  typename std::vector<Node *> nodes;
  nodes.reserve(this->get_max_height());
  for (Node *x = this->head_; x != nullptr; x = x->next(0)) {
    nodes.push_back(x);
  }
  // malloc拿到一块裸内存， 没有构造对象。
  // 释放要 先析构在free
  for (auto node : nodes) {
    node->~Node();
    free(node);
  }
}

template <typename Key, class ObComparator>
void ObSkipList<Key, ObComparator>::insert(const Key &key)
{
  using Node = typename ObSkipList<Key, ObComparator>::Node;
  Node *prev[this->kMaxHeight];
  // 1. 查找每层前驱，prev填充每层前置节点
  Node *target = this->find_greater_or_equal(key, prev);
  // 实验要求：不存在相等key才能插入
  ASSERT(target == nullptr || !this->equal(key, target->key), "key duplicated");

  // 2. 随机生成节点高度
  int h = this->random_height();
  // find_greater_or_equal 已填充 [0, kMaxHeight) 全部层的前驱，
  // 高于当前层高的层前驱自然落在 head_ 上，无需额外处理。
  // 3. 新建节点
  Node *new_nd = this->new_node(key, h);

  // 4. 逐层挂载到prev后面（单线程无竞争，直接set_next）
  for (int level = 0; level < h; level++) {
    Node *p = prev[level];
    Node *s = p->nobarrier_next(level);
    new_nd->nobarrier_set_next(level, s);
    p->nobarrier_set_next(level, new_nd);
  }

  // 5. 更新全局max_height，如果新节点更高
  // 获取当前记录的最大层高 (一次原子读)
  // release写操作， acquire读操作
  int old_max = this->max_height_.load(std::memory_order_relaxed);
  // while + CAS 尝试把max_height_修改成更高的值h
  while (h > old_max) {
    if (this->max_height_.compare_exchange_weak(old_max, h, std::memory_order_relaxed)) {
      break;
    }
  }
}
// 无锁并发插入：底层 level0 先 CAS 成功，节点才对读者可见；再逐层向上 CAS 挂载上层索引
template <typename Key, class ObComparator>
void ObSkipList<Key, ObComparator>::insert_concurrently(const Key &key)
{
  using Node = typename ObSkipList<Key, ObComparator>::Node;
  // 1. 预先创建节点，此时节点对其他线程不可见
  int   node_h = this->random_height();
  Node *new_nd = this->new_node(key, node_h);

  // 循环重试直到插入成功
  while (true) {
    Node *prev[this->kMaxHeight];
    Node *succ[this->kMaxHeight];
    // 步骤1：查找每层前驱
    Node *target = this->find_greater_or_equal(key, prev);
    // 重复key直接返回
    if (target != nullptr && this->equal(key, target->key)) {
      free(new_nd);
      return;
    }

    // 收集每层后继：succ[lv] = prev[lv] 的下一个节点。
    // find_greater_or_equal 已把 prev[0..kMaxHeight) 全部填好（高层前驱是 head_），直接取 next 即可。
    for (int lv = 0; lv < node_h; lv++) {
      succ[lv] = prev[lv]->next(lv);
    }

    // 步骤2：先把新节点所有层的 next 指针都设好（此时节点尚未发布，读线程看不到）
    for (int lv = 0; lv < node_h; lv++) {
      new_nd->nobarrier_set_next(lv, succ[lv]);
    }

    // 步骤3：先 CAS 第0层（底层链表）。成功后节点才对读者可见，必须成功才能继续上层
    if (!prev[0]->cas_next(0, succ[0], new_nd)) {
      // 底层CAS失败，其他线程抢先插入，全部重来
      continue;
    }

    // 步骤4：逐层向上 CAS 挂载上层索引
    for (int lv = 1; lv < node_h; lv++) {
      while (true) {
        // 重新查找当前层 prev/succ（中间可能被其他线程修改）
        this->find_greater_or_equal(key, prev);
        succ[lv] = prev[lv]->next(lv);
        new_nd->nobarrier_set_next(lv, succ[lv]);
        if (prev[lv]->cas_next(lv, succ[lv], new_nd)) {
          break;  // 当前层插入成功，去上一层
        }
        // 当前层CAS失败，重新查找重试本层
      }
    }

    // 步骤5：尝试更新全局最大层高max_height_
    int old_max = this->max_height_.load(std::memory_order_relaxed);
    while (node_h > old_max) {
      // relaxed只保证原子性，没有同步，没有指令屏障
      if (this->max_height_.compare_exchange_weak(old_max, node_h, std::memory_order_relaxed)) {
        break;
      }
    }

    // 全部层插入完成，退出循环
    break;
  }
}

template <typename Key, class ObComparator>
bool ObSkipList<Key, ObComparator>::contains(const Key &key) const
{
  Node *x = this->find_greater_or_equal(key, nullptr);
  if (x != nullptr && this->equal(key, x->key)) {
    return true;
  } else {
    return false;
  }
}

}  // namespace oceanbase