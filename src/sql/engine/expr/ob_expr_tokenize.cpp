/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sql/engine/expr/ob_expr_tokenize.h"

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/page_arena.h"
#include "lib/atomic/ob_atomic.h"
#include "lib/charset/ob_charset.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "object/ob_object.h"
#include "sql/resolver/ddl/ob_fts_index_builder_util.h"
#include "share/ob_json_access_utils.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "data_plane/fts/dict/ob_gen_dic_loader.h"
#include "data_plane/fts/ob_fts_parser_property.h"

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/expr/ob_expr_json_func_helper.h" // file not self-contained, there're logs inside.

namespace oceanbase
{
namespace sql
{

namespace
{
/**
 * TOKENIZE() rebuilds its fulltext parser configuration on every call: the parser name
 * is parsed and re-serialized ("ik" -> "ik.1"), the parser properties JSON is parsed,
 * rebuilt for the parser type and serialized again, and the result is parsed a second
 * time to build the parser property object. None of that depends on the document, only
 * on the SQL visible inputs (parser name, parser properties, collation), so this small
 * fixed size cache takes the work off the hot path after the first call.
 *
 * A published entry is immutable and lives for the process lifetime, which is what makes
 * it safe for a caller to borrow pointers into it: readers scan the slots without any
 * lock (a slot is published with a release store of state_ and inspected after an acquire
 * load of it) and nothing is ever evicted or freed. The slot count is bounded; once it is
 * exhausted the per call work is simply still done and the miss is counted once.
 */
struct ObTokenizeParserConfigSlot final
{
  static constexpr int64_t MAX_RAW_NAME_LEN = 128;
  static constexpr int64_t MAX_RAW_PROPERTIES_LEN = 512;
  static constexpr int64_t MAX_INNER_NAME_LEN = storage::OB_FT_PARSER_NAME_LENGTH;
  static constexpr int64_t MAX_FORMATTED_PROPERTIES_LEN = 512;

  int64_t state_;              // 0 while being filled, 1 once fully published
  int64_t dic_ready_;          // 1 once the builtin dictionary is loaded in this process
  int32_t raw_name_len_;
  int32_t raw_properties_len_;
  int32_t coll_type_;
  int32_t inner_name_len_;
  int32_t formatted_properties_len_;
  char raw_name_[MAX_RAW_NAME_LEN];
  char raw_properties_[MAX_RAW_PROPERTIES_LEN];
  char inner_name_[MAX_INNER_NAME_LEN];
  char formatted_properties_[MAX_FORMATTED_PROPERTIES_LEN];
  storage::ObFTParser parser_;
  storage::ObFTParserProperty property_;
};

constexpr int64_t MAX_TOKENIZE_PARSER_CONFIGS = 8;
ObTokenizeParserConfigSlot g_tokenize_parser_configs[MAX_TOKENIZE_PARSER_CONFIGS];
int64_t g_tokenize_parser_config_cnt = 0;
int64_t g_tokenize_parser_config_overflow_cnt = 0;

bool same_input(const char *cached, const int32_t cached_len, const ObString &input)
{
  return cached_len == input.length()
         && (0 == cached_len || 0 == MEMCMP(cached, input.ptr(), cached_len));
}

ObTokenizeParserConfigSlot *find_parser_config(const ObString &raw_parser_name,
                                               const ObString &raw_properties,
                                               const ObCollationType coll_type)
{
  ObTokenizeParserConfigSlot *slot = nullptr;
  const int64_t slot_cnt =
      MIN(ATOMIC_LOAD(&g_tokenize_parser_config_cnt), MAX_TOKENIZE_PARSER_CONFIGS);
  for (int64_t i = 0; nullptr == slot && i < slot_cnt; ++i) {
    ObTokenizeParserConfigSlot &candidate = g_tokenize_parser_configs[i];
    if (1 == ATOMIC_LOAD_ACQ(&candidate.state_)
        && static_cast<int32_t>(coll_type) == candidate.coll_type_
        && same_input(candidate.raw_name_, candidate.raw_name_len_, raw_parser_name)
        && same_input(candidate.raw_properties_, candidate.raw_properties_len_, raw_properties)) {
      slot = &candidate;
    }
  }
  return slot;
}

/**
 * Best effort: a configuration that cannot be cached (a key too long to keep a copy of,
 * or no free slot left) only keeps paying the per call cost, it must never fail the
 * statement.
 */
void publish_parser_config(const ObString &raw_parser_name,
                           const ObString &raw_properties,
                           const ObCollationType coll_type,
                           const ObString &inner_name,
                           const ObString &formatted_properties,
                           const storage::ObFTParser &parser,
                           const storage::ObFTParserProperty &property)
{
  int64_t slot_cnt = ATOMIC_LOAD(&g_tokenize_parser_config_cnt);
  if (raw_parser_name.length() > ObTokenizeParserConfigSlot::MAX_RAW_NAME_LEN
      || raw_properties.length() > ObTokenizeParserConfigSlot::MAX_RAW_PROPERTIES_LEN
      || inner_name.length() > ObTokenizeParserConfigSlot::MAX_INNER_NAME_LEN
      || formatted_properties.length()
             > ObTokenizeParserConfigSlot::MAX_FORMATTED_PROPERTIES_LEN) {
    // Too long to keep a copy of, keep doing the work per call.
  } else if (slot_cnt >= MAX_TOKENIZE_PARSER_CONFIGS) {
    if (1 == ATOMIC_AAF(&g_tokenize_parser_config_overflow_cnt, 1)) {
      LOG_INFO("no free tokenize parser config slot, do not cache", K(slot_cnt));
    }
  } else if (!ATOMIC_BCAS(&g_tokenize_parser_config_cnt, slot_cnt, slot_cnt + 1)) {
    // Another thread claimed this slot first, caching is skipped for now.
  } else {
    ObTokenizeParserConfigSlot &slot = g_tokenize_parser_configs[slot_cnt];
    slot.raw_name_len_ = static_cast<int32_t>(raw_parser_name.length());
    slot.raw_properties_len_ = static_cast<int32_t>(raw_properties.length());
    slot.coll_type_ = static_cast<int32_t>(coll_type);
    slot.inner_name_len_ = static_cast<int32_t>(inner_name.length());
    slot.formatted_properties_len_ = static_cast<int32_t>(formatted_properties.length());
    slot.dic_ready_ = 0;
    if (slot.raw_name_len_ > 0) {
      MEMCPY(slot.raw_name_, raw_parser_name.ptr(), slot.raw_name_len_);
    }
    if (slot.raw_properties_len_ > 0) {
      MEMCPY(slot.raw_properties_, raw_properties.ptr(), slot.raw_properties_len_);
    }
    if (slot.inner_name_len_ > 0) {
      MEMCPY(slot.inner_name_, inner_name.ptr(), slot.inner_name_len_);
    }
    if (slot.formatted_properties_len_ > 0) {
      MEMCPY(slot.formatted_properties_,
             formatted_properties.ptr(),
             slot.formatted_properties_len_);
    }
    slot.parser_ = parser;
    slot.property_ = property;
    // The fields above must be visible before the slot is published to the readers.
    ATOMIC_STORE_REL(&slot.state_, 1);
  }
}
} // namespace

ObExprTokenize::ObExprTokenize(common::ObIAllocator &alloc)
    : ObStringExprOperator(alloc,
                           T_FUN_TOKENIZE,
                           N_TOKENIZE,
                           MORE_THAN_ZERO,
                           VALID_FOR_GENERATED_COL)
{
}

ObExprTokenize::~ObExprTokenize() {}

int ObExprTokenize::eval_tokenize(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  common::ObArenaAllocator &temp_allocator = tmp_alloc_g.get_allocator();

  ObIJsonBase *json_result = nullptr;
  TokenizeParam param;

  // check param num, which is checked in ObExprOperator::calc_result_typeN.
  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_param(expr, ctx, temp_allocator, param))) {
  } else if (OB_FAIL(tokenize_fulltext(param, param.output_mode_, temp_allocator, json_result))) {
  } else if (OB_FAIL(ObJsonExprHelper::pack_json_res(expr,
                                                     ctx,
                                                     temp_allocator,
                                                     json_result,
                                                     expr_datum))) {
  }

  return ret;
}

int ObExprTokenize::tokenize_fulltext(const TokenizeParam &param,
                                      TokenizeParam::OUTPUT_MODE mode,
                                      ObIAllocator &allocator,
                                      ObIJsonBase *&result)
{
  int ret = OB_SUCCESS;
  storage::ObFTParseHelper tokenize_helper;
  const int64_t ft_word_bkt_cnt = MIN(MAX(param.fulltext_.length() / 2, 2), 997);
  int64_t doc_len = 0;
  storage::ObFTWordMap token_map;

  ObArenaAllocator tmp_parse_alloc(ObMemAttr("Tmp buffer"));

  if (TokenizeParam::OUTPUT_MODE::DEFAULT != mode && TokenizeParam::OUTPUT_MODE::ALL != mode) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    if (nullptr != param.cached_parser_ && nullptr != param.cached_property_) {
      // The parser description of this configuration was built before, reuse it instead
      // of parsing the parser name and the properties JSON again.
      ret = tokenize_helper.init(&allocator, *param.cached_parser_, *param.cached_property_);
    } else {
      ret = tokenize_helper.init(&allocator, param.parser_name_, param.properties_);
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(token_map.create(ft_word_bkt_cnt, common::ObMemAttr("FTWordMap")))) {
    } else if (
        (0 != param.fulltext_.length())
        && OB_FAIL(tokenize_helper.segment(
                       param.meta_,
                       param.fulltext_.ptr(),
                       param.fulltext_.length(),
                       doc_len,
                       token_map))) {
    } else {
      switch (param.output_mode_) {
      case TokenizeParam::OUTPUT_MODE::DEFAULT: {
        if (OB_FAIL(tokenize_helper.make_token_array_json(token_map, result))) {
        } else {
          // pass
        }
        break;
      }
      case TokenizeParam::OUTPUT_MODE::ALL: {
        if (OB_FAIL(tokenize_helper.make_detail_json(token_map, doc_len, result))) {
        } else {
          // pass
        }
        break;
      }
      default:
        ret = OB_INVALID_ARGUMENT;
      }
    }
  }
  return ret;
}

ObExprTokenize::TokenizeParam ::TokenizeParam()
  : allocator_(ObMemAttr("TokenizeParam")),
    parser_name_(ObString(OB_DEFAULT_FULLTEXT_PARSER_NAME)),
    meta_(),
    fulltext_(),
    cached_parser_(nullptr),
    cached_property_(nullptr),
    dic_ready_(nullptr),
    output_mode_(OUTPUT_MODE::DEFAULT)
{
}

int ObExprTokenize::TokenizeParam::parse_json_param(const ObIJsonBase *obj)
{
  int ret = OB_SUCCESS;
  ObString str;
  ObIJsonBase *val;

  if (OB_UNLIKELY(nullptr == obj)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ObJsonNodeType::J_OBJECT != obj->json_type()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (obj->element_count() == 0) {
    // no data
  } else if (OB_FAIL(obj->get_object_value(0, str, val))) {
  } else if (0 == str.case_compare(CASE_INDICATOR_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("UPPER")) {
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("LOWER")) {
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "case indentifier");
  } else if (0 == str.case_compare(OUTPUT_MODE_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("DEFAULT")) {
      output_mode_ = DEFAULT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("ALL")) {
      output_mode_ = ALL;
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    }
  } else if (0 == str.case_compare(STOPWORDS_LIST_STR)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "stopwords");
  } else if (0 == str.case_compare(ADDITIONAL_ARGS_STR)) {
    if (ObJsonNodeType::J_ARRAY != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser arguments");
    } else {
      ObString json_str;
      if (OB_FAIL(storage::ObFTParserJsonProps::tokenize_array_to_props_json(allocator_, val, json_str))) {
        ObSqlString message;
        message.append_fmt("format in %s form", ADDITIONAL_ARGS_STR);
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
      } else {
        properties_ = json_str;
      }
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    ObSqlString message;
    message.append_fmt("config: %s", str.ptr());
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
  }
  return ret;
}

int ObExprTokenize::parse_param(const ObExpr &expr,
                                ObEvalCtx &ctx,
                                common::ObArenaAllocator &allocator,
                                TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObString raw_parser_name;
  ObString raw_properties;

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());

  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_fulltext(expr, ctx, param))) {
  } else if (OB_FAIL(eval_parser_name(expr, ctx, raw_parser_name))) {
  } else if (OB_FAIL(parse_parser_properties(expr, ctx, temp_allocator, param))) {
  } else {
    raw_properties = param.properties_;
    ObTokenizeParserConfigSlot *slot = find_parser_config(
        raw_parser_name, raw_properties, param.meta_.get_collation_type());
    if (nullptr != slot) {
      // Built before for exactly these inputs: borrow the parser description and the
      // reformed properties instead of building them again.
      param.parser_name_ = ObString(slot->inner_name_len_, slot->inner_name_);
      param.properties_ = ObString(slot->formatted_properties_len_, slot->formatted_properties_);
      param.cached_parser_ = &slot->parser_;
      param.cached_property_ = &slot->property_;
      param.dic_ready_ = &slot->dic_ready_;
    } else if (OB_FAIL(construct_ft_parser_inner_name(raw_parser_name, param))) {
    } else if (OB_FAIL(param.reform_parser_properties(param.properties_))) {
    } else {
      storage::ObFTParser parser;
      storage::ObFTParserProperty property;
      // Preparing the cached description is best effort: a failure here only loses the
      // caching opportunity and must not fail the statement.
      if (OB_SUCCESS == parser.parse_from_str(param.parser_name_.ptr(), param.parser_name_.length())
          && OB_SUCCESS == property.parse_for_parser_helper(parser, param.properties_)) {
        publish_parser_config(raw_parser_name,
                              raw_properties,
                              param.meta_.get_collation_type(),
                              param.parser_name_,
                              param.properties_,
                              parser,
                              property);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(param.try_load_dictionary_for_ik())) {
    }
  }
  return ret;
}

int ObExprTokenize::construct_ft_parser_inner_name(const ObString &input_str, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  storage::ObFTParser parser;

  char *parser_name_buf = nullptr;
  if (OB_ISNULL(parser_name_buf
                = static_cast<char *>(param.allocator_.alloc(storage::OB_FT_PARSER_NAME_LENGTH)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(parser.init(input_str))) {
  } else if (OB_FAIL(parser.serialize_to_str(parser_name_buf, storage::OB_FT_PARSER_NAME_LENGTH))) {
  } else {
    param.parser_name_ = ObString::make_string(parser_name_buf);
  }
  return ret;
}

int ObExprTokenize::calc_result_typeN(ObExprResType &type,
                                      ObExprResType *types,
                                      int64_t param_num,
                                      common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(param_num < 1 || param_num > 3)) {
    ret = OB_ERR_PARAM_SIZE;
    ObString expr_name(N_TOKENIZE);
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, expr_name.length(), expr_name.ptr());
  } else {
    // just okay
  }

  ObLength length = ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType].get_length();

  if (OB_SUCC(ret)) {
    // set res type
    type.set_json();
    type.set_length(length); // keep consistent with other json expr, maybe calc it later.

    // param type set, skip charset after first param
    for (int64_t i = 1; OB_SUCC(ret) && i < param_num; ++i) {
      if (ob_is_string_type(types[i].get_type())) {
        if (types[i].get_charset_type() != CHARSET_UTF8MB4) {
          types[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
        }
      }
    }

    // handle param
    if (param_num >= 2) {
      types[1].set_varchar();
    }

    if (param_num >= 3) {
      if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types, 2, N_TOKENIZE))) {
      }
    }
  }

  return ret;
}

int ObExprTokenize::cg_expr(ObExprCGCtx &op_cg_ctx,
                            const ObRawExpr &raw_expr,
                            ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  CK((rt_expr.arg_cnt_ >= 1 && rt_expr.arg_cnt_ <= 3));
  if (OB_SUCC(ret)) {
    // do register
    rt_expr.eval_func_ = eval_tokenize;
  }
  return ret;
}

int ObExprTokenize::parse_fulltext(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *fulltext_datum;

  if (OB_FAIL(expr.args_[0]->eval(ctx, fulltext_datum))) {
  } else {
    if (fulltext_datum->is_null()) {
      // do nothing, return empty result
      param.fulltext_ = ObString::make_empty_string();
    } else {
      param.fulltext_ = fulltext_datum->get_string();
    }
    param.meta_.set_varchar(); // as we hardcoded in fts_index
    param.meta_.set_collation_type(expr.args_[0]->obj_meta_.get_collation_type());
  }
  return ret;
}

int ObExprTokenize::eval_parser_name(const ObExpr &expr,
                                     ObEvalCtx &ctx,
                                     ObString &raw_parser_name)
{
  int ret = OB_SUCCESS;
  ObDatum *parser_datum = nullptr;

  if (expr.arg_cnt_ < 2) {
    raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
  } else if (OB_FAIL(expr.args_[1]->eval(ctx, parser_datum))) {
  } else if (parser_datum->is_null()) {
    raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
  } else {
    raw_parser_name = parser_datum->get_string().trim();
  }

  return ret;
}

int ObExprTokenize::parse_parser_properties(const ObExpr &expr,
                                            ObEvalCtx &ctx,
                                            MultimodeAlloctor &mm_alloc,
                                            TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;

  if (expr.arg_cnt_ < 3) {
    // do nothing
  } else {
    bool is_null = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, mm_alloc, 2, base, is_null))) {
    } else {
      if (ObJsonNodeType::J_ARRAY != base->json_type()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser args should be in an array.");
      } else {
        for (uint64_t i = 0; OB_SUCC(ret) && i < base->element_count(); ++i) {
          ObIJsonBase *node = nullptr;
          if (OB_FAIL(base->get_array_element(i, node))) {
          } else if (ObJsonNodeType::J_OBJECT != (node->json_type())) {
            ret = OB_INVALID_ARGUMENT;
          } else if (OB_FAIL(param.parse_json_param(node))) {
          }
        } // for
      }
    }
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::reform_parser_properties(const ObString &properties)
{
  int ret = OB_SUCCESS;
  storage::ObFTParserJsonProps parser_properties;

  if (OB_FAIL(parser_properties.init())) {
  } else if (OB_FAIL(parser_properties.parse_from_valid_str(properties))) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser properties invalid.");
  } else if (OB_FAIL(parser_properties.rebuild_props_for_ddl(parser_name_,
                                                             ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                                             true))) {
  } else if (OB_FAIL(parser_properties.to_format_json(allocator_, properties_))) {
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::try_load_dictionary_for_ik()
{
  int ret = OB_SUCCESS;
  bool need_to_load_dic = false;
  storage::ObDicLoaderHandle dic_loader_handle;
  if (nullptr != dic_ready_ && 1 == ATOMIC_LOAD(dic_ready_)) {
    // The builtin dictionary of this parser is already loaded in this process: the loader
    // keeps that state and never clears it, so resolving and loading it again is a no-op.
  } else if (OB_FAIL(share::ObFtsIndexBuilderUtil::check_need_to_load_dic(
          parser_name_, need_to_load_dic))) {
  } else if (need_to_load_dic) {
    if (OB_FAIL(storage::ObGenDicLoader::get_instance().get_dic_loader(
                    ObString::make_string(storage::ObFTSLiteral::PARSER_NAME_IK), // currently only ik, use parser_name_ without version suffix
                    ObCharset::charset_type_by_coll(meta_.get_collation_type()),
                    dic_loader_handle))) {
    } else if (OB_UNLIKELY(!dic_loader_handle.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dic_loader_handle.get_loader()->try_load_dictionary_in_trans())) {
    } else if (nullptr != dic_ready_) {
      ATOMIC_STORE(dic_ready_, 1);
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
