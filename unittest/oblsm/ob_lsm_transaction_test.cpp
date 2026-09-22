/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "gtest/gtest.h"

#include "common/lang/filesystem.h"
#include "common/lang/thread.h"
#include "common/lang/utility.h"
#include "oblsm/include/ob_lsm.h"
#include "oblsm/ob_lsm_define.h"
#include "unittest/oblsm/ob_lsm_test_base.h"
#include "oblsm/include/ob_lsm_transaction.h"

using namespace oceanbase;

class ObLsmTransactionTest : public ObLsmTestBase
{};

bool check_lsm_scan_result_by_value(ObLsmIterator *iter, const std::vector<std::string> &values)
{
  int i = 0;
  iter->seek_to_first();
  while (iter->valid()) {
    if (values[i] != iter->value()) {
      return false;
    }
    ++i;
    iter->next();
  }
  return true;
}

TEST_F(ObLsmTransactionTest, oblsm_test_basic1)
{
  // 1. 全局直接写3条数据（非事务put），写入memtable
  db->put("key1", "value1");
  db->put("key2", "value2");
  db->put("key3", "value3");
  auto iter = db->new_iterator(ObLsmReadOptions());
  ASSERT_TRUE(check_lsm_scan_result_by_value(iter, {"value1", "value2", "value3"}));
  delete iter;

  // 2. 开启事务 txn1，拿到快照
  auto txn1 = db->begin_transaction();
  // txn1读快照：看到3条原始数据
  auto iter_txn1 = txn1->new_iterator(ObLsmReadOptions());
  ASSERT_TRUE(check_lsm_scan_result_by_value(iter_txn1, {"value1", "value2", "value3"}));
  delete iter_txn1;

  // 3. txn1 在本地WriteBatch修改key1，新增txnkeyx（**只在事务内存缓存，全局不可见**）
  txn1->put("key1", "valuetxn1");
  txn1->put("txnkeyx", "txnvaluex");
  // ✅ 事务内迭代器：优先读自己本地WriteBatch，所以key1变成valuetxn1，并且能看到txnkeyx
  auto iter2_txn1 = txn1->new_iterator(ObLsmReadOptions());
  ASSERT_TRUE(check_lsm_scan_result_by_value(iter2_txn1, {"valuetxn1", "value2", "value3", "txnvaluex"}));
  delete iter2_txn1;

  // 4. 开启txn2，txn2拿到属于自己的快照（此时txn1还没commit）
  auto txn2 = db->begin_transaction();
  txn2->put("key1", "valuetxn2");  // txn2本地修改key1，独立WriteBatch
  // txn2迭代器：看到自己本地修改的key1=valuetxn2，看不到txn1的未提交数据
  auto iter_txn2 = txn2->new_iterator(ObLsmReadOptions());
  ASSERT_TRUE(check_lsm_scan_result_by_value(iter_txn2, {"valuetxn2", "value2", "value3"}));
  delete iter_txn2;

  // 5. txn1 commit！txn1的修改写入LSM全局memtable，全局可见
  txn1->commit();

  // 🔥重点：txn2**快照是begin的时候生成的**！虽然txn1已经提交了，txn2仍然使用旧快照
  // txn2 仍然看不到txn1提交后的key1=valuetxn1，txn2看到的还是自己本地的valuetxn2
  auto iter2_txn2 = txn2->new_iterator(ObLsmReadOptions());
  ASSERT_TRUE(check_lsm_scan_result_by_value(iter2_txn2, {"valuetxn2", "value2", "value3"}));
  delete iter2_txn2;

  // 6. 新开txn3，begin拿到新快照，可以读到txn1已经提交的所有修改
  auto txn3      = db->begin_transaction();
  auto iter_txn3 = txn3->new_iterator(ObLsmReadOptions());
  ASSERT_TRUE(check_lsm_scan_result_by_value(iter_txn3, {"valuetxn1", "value2", "value3", "txnvaluex"}));
  delete iter_txn3;

  delete txn1;
  delete txn2;
  delete txn3;
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}