/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Longda on 2021/4/13.
//

#include <string.h>

#include "parse_stage.h"

#include "common/conf/ini.h"
#include "common/io/io.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "sql/parser/parse.h"

using namespace common;

RC ParseStage::handle_request(SQLStageEvent *sql_event)
{
  RC rc = RC::SUCCESS;

  SqlResult         *sql_result = sql_event->session_event()->sql_result();
  const string &sql        = sql_event->sql();

  ParsedSqlResult parsed_sql_result;

  parse(sql.c_str(), &parsed_sql_result);
  if (parsed_sql_result.sql_nodes().empty()) {
    sql_result->set_return_code(RC::SUCCESS);
    sql_result->set_state_string("");
    return RC::INTERNAL;
  }

  if (parsed_sql_result.sql_nodes().size() > 1) {
    LOG_WARN("got multi sql commands but only 1 will be handled");
  }

  // 只看 front() 是不够的：bison 在语法的默认归约上会先把能解析的前缀规约出来
  // （比如 `select ... from t limit 1` 会先产出一个 SELECT 节点），
  // 遇到无法识别的尾随子句再调用 yyerror 追加一个 SCF_ERROR 节点。
  // 只检查 front() 就会把尾随的错误节点丢掉，于是 LIMIT 之类的子句被静默忽略。
  // 因此这里扫描所有节点，只要有一个是 SCF_ERROR 就整体报语法错误。
  for (const unique_ptr<ParsedSqlNode> &node : parsed_sql_result.sql_nodes()) {
    if (node->flag == SCF_ERROR) {
      // set error information to event
      rc = RC::SQL_SYNTAX;
      sql_result->set_return_code(rc);
      sql_result->set_state_string("Failed to parse sql");
      return rc;
    }
  }

  unique_ptr<ParsedSqlNode> sql_node = std::move(parsed_sql_result.sql_nodes().front());

  sql_event->set_sql_node(std::move(sql_node));

  return RC::SUCCESS;
}
