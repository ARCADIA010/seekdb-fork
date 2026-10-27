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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_rerank.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"
#include "lib/utility/ob_sort.h"
#include <cmath>
#include <limits>
 
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

namespace
{
struct RerankResultEntry
{
  ObJsonObject *object_;
  int64_t index_;
  double score_;
};

// Read untrusted provider data only after checking the JSON node types.
int read_rerank_result(ObJsonNode *node, int64_t document_count,
                       int64_t &index, double &score)
{
  int ret = OB_SUCCESS;
  ObJsonNode *index_node = nullptr;
  ObJsonNode *score_node = nullptr;
  if (OB_ISNULL(node) || node->json_type() != ObJsonNodeType::J_OBJECT) {
    ret = OB_INVALID_DATA;
  } else {
    ObJsonObject *object = static_cast<ObJsonObject *>(node);
    index_node = object->get_value("index");
    score_node = object->get_value("relevance_score");
    if (OB_ISNULL(index_node) || OB_ISNULL(score_node) || document_count <= 0) {
      ret = OB_INVALID_DATA;
    } else if (index_node->json_type() == ObJsonNodeType::J_INT) {
      index = static_cast<ObJsonInt *>(index_node)->value();
      if (index < 0 || index >= document_count) {
        ret = OB_INVALID_DATA;
      }
    } else if (index_node->json_type() == ObJsonNodeType::J_UINT) {
      const uint64_t value = static_cast<ObJsonUint *>(index_node)->value();
      if (value >= static_cast<uint64_t>(document_count)) {
        ret = OB_INVALID_DATA;
      } else {
        index = static_cast<int64_t>(value);
      }
    } else {
      ret = OB_INVALID_DATA;
    }
    if (OB_SUCC(ret)) {
      const ObJsonNodeType score_type = score_node->json_type();
      if (score_type != ObJsonNodeType::J_INT && score_type != ObJsonNodeType::J_UINT
          && score_type != ObJsonNodeType::J_DOUBLE && score_type != ObJsonNodeType::J_DECIMAL) {
        ret = OB_INVALID_DATA;
      } else if (OB_FAIL(score_node->to_double(score))) {
        ret = OB_INVALID_DATA;
      } else if (!std::isfinite(score)) {
        ret = OB_INVALID_DATA;
      }
    }
  }
  return ret;
}

int validate_and_sort_rerank_result(ObIAllocator &allocator, ObJsonArray *response,
                                    int64_t document_count, ObJsonArray *&sorted_response)
{
  int ret = OB_SUCCESS;
  ObArray<bool> seen;
  ObArray<RerankResultEntry> entries;
  ObJsonArray *sorted_array = nullptr;
  if (OB_ISNULL(response) || document_count <= 0
      || response->element_count() != static_cast<uint64_t>(document_count)) {
    ret = OB_INVALID_DATA;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < document_count; ++i) {
    if (OB_FAIL(seen.push_back(false))) {
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < document_count; ++i) {
    int64_t index = -1;
    double score = 0;
    ObJsonNode *node = response->get_value(i);
    if (OB_FAIL(read_rerank_result(node, document_count, index, score))) {
    } else if (seen.at(index)) {
      ret = OB_INVALID_DATA;
    } else {
      seen.at(index) = true;
      const RerankResultEntry entry = {static_cast<ObJsonObject *>(node), index, score};
      if (OB_FAIL(entries.push_back(entry))) {
      }
    }
  }
  // Exact count, range and uniqueness together imply complete input coverage.
  if (OB_SUCC(ret)) {
    lib::ob_sort(entries.begin(), entries.end(),
        [](const RerankResultEntry &left, const RerankResultEntry &right) {
          return left.score_ > right.score_
              || (left.score_ == right.score_ && left.index_ < right.index_);
        });
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, sorted_array))) {
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < entries.count(); ++i) {
    if (OB_FAIL(sorted_array->append(entries.at(i).object_))) {
    }
  }
  if (OB_SUCC(ret)) {
    sorted_response = sorted_array;
  } else if (ret == OB_INVALID_DATA) {
    LOG_WARN("invalid rerank response count, index or relevance_score", K(ret), K(document_count));
  }
  return ret;
}
} // namespace

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_RERANK, 
                    N_AI_RERANK, 
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIRerank::~ObExprAIRerank() 
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_tc(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_tc(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (doc_key_type == ObJsonType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {  
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_query = nullptr;
  ObDatum *arg_documents = nullptr;
  ObDatum *arg_doc_key = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_query, arg_documents, arg_doc_key))) {
  } else if (OB_ISNULL(arg_model_id) || OB_ISNULL(arg_query) || OB_ISNULL(arg_documents)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (arg_model_id->is_null() || arg_query->is_null() || arg_documents->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query or documents is null");
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_RERANK));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo endpoint_info;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString query = arg_query->get_string();
    const bool has_doc_key = OB_NOT_NULL(arg_doc_key) && !arg_doc_key->is_null();
    ObString doc_key;
    ObJsonArray *document_array = nullptr;
    ObIJsonBase *j_base = nullptr;
    bool is_null_result = false;
    if (has_doc_key) {
      if (expr.args_[DOC_KEY_IDX]->datum_meta_.type_ == ObJsonType) {
        ObIJsonBase *key_base = nullptr;
        if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, temp_allocator, DOC_KEY_IDX,
                                                key_base, is_null_result))) {
        } else if (OB_ISNULL(key_base) || key_base->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
        } else {
          doc_key = static_cast<ObJsonString *>(key_base)->value();
        }
      } else {
        doc_key = arg_doc_key->get_string();
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, temp_allocator, DOCUMENTS_IDX,
                                                    j_base, is_null_result))) {
    } else if (OB_ISNULL(j_base) || j_base->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is not array");
    } else {
      document_array = static_cast<ObJsonArray *>(j_base);
      if (document_array->element_count() == 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is empty");
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < document_array->element_count(); ++i) {
        ObJsonNode *doc_node = document_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (has_doc_key && doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else if (!has_doc_key && doc_node->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not string");
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(info) || OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(model_id, temp_allocator, endpoint_info))) {
    } else if (has_doc_key) {
      if (OB_FAIL(eval_ai_rerank_with_doc_key(expr, ctx, temp_allocator, model_id, query,
                                             document_array, doc_key, *info, endpoint_info, res))) {
      }
    } else {
      ObJsonArray *result_array = nullptr;
      ObString raw_str;
      if (OB_FAIL(eval_rerank_batches(temp_allocator, *info, endpoint_info, query,
                                     document_array, result_array))) {
      } else if (OB_FAIL(result_array->get_raw_binary(raw_str, &temp_allocator))) {
      } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
      }
    }
  }
  if (OB_FAIL(ret)) {
    res.set_null();
  }
  return ret;
}

int ObExprAIRerank::eval_rerank_batches(ObIAllocator &allocator,
                                       const ObAIFuncExprInfo &info,
                                       const share::ObAiModelEndpointInfo &endpoint_info,
                                       ObString &query, ObJsonArray *document_array,
                                       ObJsonArray *&result_array)
{
  int ret = OB_SUCCESS;
  const int64_t batch_size = 20;
  ObArray<ObString> headers;
  ObJsonArray *merged_array = nullptr;
  if (OB_ISNULL(document_array) || document_array->element_count() == 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(&info))) {
  } else if (OB_FAIL(ObAIFuncUtils::get_header(allocator, info, endpoint_info, headers))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, merged_array))) {
  } else {
    const int64_t document_count = document_array->element_count();
    ObString score_key(SCORE_KEY);
    for (int64_t start = 0; OB_SUCC(ret) && start < document_count;) {
      const int64_t count = MIN(batch_size, document_count - start);
      ObJsonArray *batch_documents = nullptr;
      ObJsonArray *batch_results = nullptr;
      ObJsonArray *compact_array = nullptr;
      if (OB_FAIL(construct_batch_document_array(allocator, document_array, start, start + count,
                                                 batch_documents))) {
      } else if (OB_FAIL(inner_eval_ai_rerank(allocator, info, endpoint_info, headers, query,
                                             batch_documents, batch_results))) {
      } else if (OB_FAIL(batch_result_add_base(allocator, batch_results, start))) {
      } else if (OB_FAIL(compact_json_array_by_key(allocator, merged_array, batch_results,
                                                  score_key, compact_array))) {
      } else {
        merged_array = compact_array;
        start += count;
      }
    }
    if (OB_SUCC(ret) && merged_array->element_count() != document_array->element_count()) {
      ret = OB_INVALID_DATA;
    }
  }
  if (OB_SUCC(ret)) {
    result_array = merged_array;
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank_with_doc_key(const ObExpr &expr, ObEvalCtx &ctx, ObIAllocator &allocator, 
                                                ObString& model_id, ObString& query, ObJsonArray *document_array,
                                                ObString& doc_key, const ObAIFuncExprInfo &info,
                                                const share::ObAiModelEndpointInfo &endpoint_info, ObDatum &res)
{
  INIT_SUCC(ret);
  ObJsonArray *doc_array = nullptr;
  ObJsonArray *result_array = nullptr;
  ObJsonArray *sorted_document_array = nullptr;
  UNUSED(model_id);
  if (OB_FAIL(get_doc_array_from_documents_array_with_key(allocator, document_array, doc_key, doc_array))) {
  } else {
    if (OB_FAIL(eval_rerank_batches(allocator, info, endpoint_info, query, doc_array, result_array))) {
    } else if (OB_FAIL(sort_document_array_by_model_result(allocator, document_array, result_array, sorted_document_array))) {
    } 
  }
  if (OB_SUCC(ret)) {
    ObString raw_str;
    if (OB_FAIL(sorted_document_array->get_raw_binary(raw_str, &allocator))) {
    } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
    }
  }
  return ret;
}

int ObExprAIRerank::get_doc_array_from_documents_array_with_key(ObIAllocator &allocator, ObJsonArray *document_object_array, ObString &doc_key, ObJsonArray *&doc_array)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(document_object_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (doc_key.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonArray *doc_array_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, doc_array_obj))) {
    } else {
      int64_t count = document_object_array->element_count();
      ObJsonNode *doc_node = nullptr;
      ObJsonNode *doc_value_node = nullptr;
      ObJsonObject *doc_obj = nullptr;
      for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
        doc_node = document_object_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is null");
        } else if (doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else {
          doc_obj = static_cast<ObJsonObject *>(doc_node);
          doc_value_node = doc_obj->get_value(doc_key);
          if (OB_ISNULL(doc_value_node)) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is null");
          } else if (doc_value_node->json_type() != ObJsonNodeType::J_STRING) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is not string");
          } else if (OB_FAIL(doc_array_obj->append(doc_value_node))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      doc_array = doc_array_obj;
    }
  }
  return ret;
}

int ObExprAIRerank::sort_document_array_by_model_result(ObIAllocator &allocator,
                                                        ObJsonArray *document_array,
                                                        ObJsonArray *model_result_array,
                                                        ObJsonArray *&sorted_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *sorted_array = nullptr;
  if (OB_ISNULL(document_array) || OB_ISNULL(model_result_array)
      || document_array->element_count() != model_result_array->element_count()) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, sorted_array))) {
  } else {
    const int64_t count = document_array->element_count();
    for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
      ObJsonNode *result_node = model_result_array->get_value(i);
      int64_t index = -1;
      double score = 0;
      if (OB_FAIL(read_rerank_result(result_node, count, index, score))) {
      } else {
        ObJsonNode *origin = document_array->get_value(index);
        ObJsonObject *new_object = nullptr;
        ObJsonNode *score_node = static_cast<ObJsonObject *>(result_node)->get_value(SCORE_KEY);
        ObJsonNode *new_score = nullptr;
        if (OB_ISNULL(origin) || origin->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_DATA;
        } else if (OB_ISNULL(new_object = static_cast<ObJsonObject *>(origin->clone(&allocator, true)))
                   || OB_ISNULL(new_score = score_node->clone(&allocator, true))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          ObJsonNode *old_score = new_object->get_value(MODEL_SCORE_KEY);
          if (OB_NOT_NULL(old_score)) {
            if (OB_FAIL(new_object->replace(old_score, new_score))) {
            }
          } else if (OB_FAIL(new_object->add(MODEL_SCORE_KEY, new_score))) {
          }
          if (OB_SUCC(ret) && OB_FAIL(sorted_array->append(new_object))) {
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    sorted_document_array = sorted_array;
  }
  return ret;
}

int ObExprAIRerank::construct_batch_document_array(ObIAllocator &allocator,
                                                   ObJsonArray *document_array,
                                                   int64_t start_idx, int64_t end_idx,
                                                   ObJsonArray *&batch_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *result_array = nullptr;
  if (OB_ISNULL(document_array) || start_idx < 0 || end_idx < start_idx
      || static_cast<uint64_t>(end_idx) > document_array->element_count()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    for (int64_t i = start_idx; OB_SUCC(ret) && i < end_idx; ++i) {
      if (OB_FAIL(result_array->append(document_array->get_value(i)))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    batch_document_array = result_array;
  }
  return ret;
}

int ObExprAIRerank::inner_eval_ai_rerank(ObIAllocator &allocator,
                                         const ObAIFuncExprInfo &info,
                                         const share::ObAiModelEndpointInfo &endpoint_info,
                                         ObArray<ObString> &header_array,
                                         ObString &query,
                                         ObJsonArray *document_array,
                                         ObJsonArray *&result_array)
{
  INIT_SUCC(ret);
  ObJsonObject *config_json = nullptr;
  ObJsonObject *body = nullptr;
  ObJsonObject *http_response = nullptr;
  ObIJsonBase *response = nullptr;
  ObAIFuncClient ai_client;
  // AI_RERANK has no top_n argument: every input must appear exactly once.
  if (OB_ISNULL(document_array) || document_array->element_count() == 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(construct_config_json(allocator, document_array->element_count(), 1, config_json))) {
  } else if (OB_FAIL(ObAIFuncUtils::get_rerank_body(allocator, info, endpoint_info, query,
                                                 document_array, config_json, body))) {
  } else if (OB_FAIL(ai_client.send_post(allocator, endpoint_info.get_url(), header_array, body, http_response))) {
  } else if (OB_FAIL(ObAIFuncUtils::parse_rerank_output(allocator, endpoint_info, http_response, response))) {
  } else if (OB_ISNULL(response) || response->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(validate_and_sort_rerank_result(allocator, static_cast<ObJsonArray *>(response),
                                                     document_array->element_count(), result_array))) {
  }
  return ret;
}

int ObExprAIRerank::batch_result_add_base(ObIAllocator &allocator, ObJsonArray *array, int64_t start_idx)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(array) || start_idx < 0) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const int64_t count = array->element_count();
    for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
      ObJsonNode *node = array->get_value(i);
      int64_t index = -1;
      double score = 0;
      if (OB_FAIL(read_rerank_result(node, count, index, score))) {
      } else if (start_idx > std::numeric_limits<int64_t>::max() - index) {
        ret = OB_INVALID_DATA;
      } else {
        ObJsonObject *object = static_cast<ObJsonObject *>(node);
        ObJsonInt *global_index = nullptr;
        // Replace the node so ancestor serialization sizes follow the new value.
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, start_idx + index, global_index))) {
        } else if (OB_FAIL(object->replace(object->get_value(INDEX_KEY), global_index))) {
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::compact_json_array_by_key(ObIAllocator &allocator,
                                              ObJsonArray *array1, ObJsonArray *array2,
                                              ObString &score_key, ObJsonArray *&compact_array)
{
  INIT_SUCC(ret);
  UNUSED(score_key);
  ObJsonArray *result_array = nullptr;
  if (OB_ISNULL(array1) || OB_ISNULL(array2)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    const int64_t count1 = array1->element_count();
    const int64_t count2 = array2->element_count();
    int64_t i = 0;
    int64_t j = 0;
    if (count1 > std::numeric_limits<int64_t>::max() - count2) {
      ret = OB_INVALID_DATA;
    }
    while (OB_SUCC(ret) && i < count1 && j < count2) {
      int64_t index1 = -1;
      int64_t index2 = -1;
      double score1 = 0;
      double score2 = 0;
      if (OB_FAIL(read_rerank_result(array1->get_value(i), count1 + count2, index1, score1))) {
      } else if (OB_FAIL(read_rerank_result(array2->get_value(j), count1 + count2, index2, score2))) {
      } else if (score1 > score2 || (score1 == score2 && index1 < index2)) {
        if (OB_FAIL(result_array->append(array1->get_value(i)))) {
        } else {
          ++i;
        }
      } else if (OB_FAIL(result_array->append(array2->get_value(j)))) {
      } else {
        ++j;
      }
    }
    while (OB_SUCC(ret) && i < count1) {
      if (OB_FAIL(result_array->append(array1->get_value(i)))) {
      } else {
        ++i;
      }
    }
    while (OB_SUCC(ret) && j < count2) {
      if (OB_FAIL(result_array->append(array2->get_value(j)))) {
      } else {
        ++j;
      }
    }
  }
  if (OB_SUCC(ret)) {
    compact_array = result_array;
  }
  return ret;
}

int ObExprAIRerank::construct_config_json(ObIAllocator &allocator, int64_t top_n, int64_t return_doc, ObJsonObject *&config_json)
{
  INIT_SUCC(ret);
  ObJsonObject *config_obj = nullptr;
  ObJsonInt *top_n_obj = nullptr;
  ObJsonBoolean *return_doc_obj = nullptr;
  bool return_doc_value = true;
  if (return_doc <= 0) {
    return_doc_value = false;
  }
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config_obj))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_boolean(allocator, return_doc_value, return_doc_obj))) {
  } else if (OB_FAIL(config_obj->add("return_documents", return_doc_obj))) {
  } else if (top_n > 0) {
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, top_n, top_n_obj))) {
    } else if (OB_FAIL(config_obj->add("top_n", top_n_obj))) {
    }
  }
  if (OB_SUCC(ret)) {
    config_json = config_obj;
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   bool got_data = false;
  //   ObObj const_data;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else {
  //     ObAIFuncExprInfo *info = OB_NEWx(ObAIFuncExprInfo, allocator, *allocator, T_FUN_SYS_AI_RERANK);
  //     if (OB_ISNULL(info)) {
  //       ret = OB_ALLOCATE_MEMORY_FAILED;
  //       LOG_WARN("failed to allocate memory for ai rerank info", K(ret));
  //     } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //       LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //     } else {
  //       rt_expr.extra_info_ = info;
  //     }
  //   }
  // }
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
