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
// Created by Ping Xu(haibarapink@gmail.com)
//
#pragma once

#include <cstdlib>
#include <iostream>
#include <fstream>
#include <map>
#include <ctime>

#include "common/sys/rc.h"
#include "common/lang/string.h"
#include "common/lang/string_view.h"
#include "oblsm/client/cliutil/defs.h"

#define MAX_MEM_BUFFER_SIZE 8192
namespace oceanbase {

inline const string LINE_HISTORY_FILE = "./.oblsm_cli.history";

enum class TokenType
{
  COMMAND,  // 原生指令：open / set / get / delete 等
  STRING,   // 字符串参数（被引号包裹的 key/value）
  BOUND,    // 边界符 `-`，对应 scan 范围查询的起止标记
  INVALID   // 非法字符/令牌
};

class ObLsmCliCmdTokenizer
{
public:
  TokenType       token_type;  // 当前解析出的令牌类型
  ObLsmCliCmdType cmd;         // 如果是COMMAND，对应命令枚举
  string          str;         // 字符串内容（参数/边界符文本）

  ObLsmCliCmdTokenizer()
  {
    //{"open" → OPEN}, {"set" → SET}, {"get" → GET} ...
#define MAP_COMMAND(cmd) token_map_[string{ObLsmCliUtil::strcmd(ObLsmCliCmdType::cmd)}] = ObLsmCliCmdType::cmd
    MAP_COMMAND(OPEN);
    MAP_COMMAND(CLOSE);
    MAP_COMMAND(SET);
    MAP_COMMAND(DELETE);
    MAP_COMMAND(SCAN);
    MAP_COMMAND(HELP);
    MAP_COMMAND(EXIT);
    MAP_COMMAND(GET);
#undef MAP_COMMAND
  }

  void init(string_view command)
  {
    command_ = command;  // 绑定待解析的整行输入
    p_       = 0;        // 解析指针置0，从头开始遍历
  }

  RC next();  // 向后读取一个完整 Token

private:
  bool out_of_range() { return p_ >= command_.size(); }
  // 判断游标是否走到文本末尾（解析完毕）

  void skip_blank_space();
  // 跳过空格、制表符等空白字符，分词必备

  RC parse_string(string &res);
  // 解析被双引号包裹的字符串参数（key / value），处理引号内内容

  std::map<string, ObLsmCliCmdType> token_map_;

  string_view command_;
  size_t      p_;
};

// Semantic checks
class ObLsmCliCmdParser
{
public:
  struct Result
  {
    ObLsmCliCmdType cmd;    // 最终识别的命令枚举
    string          error;  // 语法错误信息，为空则合法

    string args[2];                     // 最多2个字符串参数（适配各类指令）
    bool   bounds[2] = {false, false};  // 标记两个位置是否是边界符 `-`
  };

  Result result;
  RC     parse(string_view command);

private:
  // 词法解析器
  ObLsmCliCmdTokenizer tokenizer_;
};

}  // namespace oceanbase