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
#include "ob_expr_ai_embed.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (OB_FAIL(expr.args_[CONTENT_IDX]->eval(ctx, arg_content))) {
  } else if (arg_content->is_null()) {
    // Match batch evaluation: NULL content also skips model/dimension expressions.
    res.set_null();
  } else if (OB_FAIL(expr.args_[MODEL_IDX]->eval(ctx, arg_model_id))) {
  } else if (expr.arg_cnt_ == 3 && OB_FAIL(expr.args_[DIM_IDX]->eval(ctx, arg_dim))) {
  } else if (arg_model_id->is_null() || (OB_NOT_NULL(arg_dim) && arg_dim->is_null())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or dimension is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content = arg_content->get_string();
    if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim)) {
      dim = arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

namespace
{
struct EmbedBatchGroup
{
  EmbedBatchGroup() : info_(nullptr), dimension_(0) {}
  ObString model_id_;
  ObAIFuncExprInfo *info_;
  share::ObAiModelEndpointInfo endpoint_;
  int64_t dimension_;
  ObArray<int64_t> rows_;
  ObArray<ObString> contents_;
  ObArray<ObString> results_;

  bool matches(const ObString &model_id, const ObAIFuncExprInfo &info,
               const share::ObAiModelEndpointInfo &endpoint, const int64_t dimension) const
  {
    // Conservatively keep model aliases separate, and compare every endpoint
    // field that can affect routing, credentials, transforms or request content.
    return model_id_ == model_id && dimension_ == dimension
        && info_->type_ == info.type_ && info_->model_ == info.model_
        && endpoint_.get_endpoint_id() == endpoint.get_endpoint_id()
        && endpoint_.get_name() == endpoint.get_name()
        && endpoint_.get_scope() == endpoint.get_scope()
        && endpoint_.get_ai_model_name() == endpoint.get_ai_model_name()
        && endpoint_.get_url() == endpoint.get_url()
        && endpoint_.get_provider() == endpoint.get_provider()
        && endpoint_.get_request_model_name() == endpoint.get_request_model_name()
        && endpoint_.get_encrypted_access_key() == endpoint.get_encrypted_access_key()
        && endpoint_.get_parameters() == endpoint.get_parameters()
        && endpoint_.get_request_transform_fn() == endpoint.get_request_transform_fn()
        && endpoint_.get_response_transform_fn() == endpoint.get_response_transform_fn();
  }
};
} // namespace

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                      const ObBitVector &skip, const int64_t size)
{
  int ret = OB_SUCCESS;
  if (size <= 0) {
    return ret;
  } else if (expr.arg_cnt_ < 2 || expr.arg_cnt_ > 3 || OB_ISNULL(expr.args_)) {
    return OB_ERR_UNEXPECTED;
  }
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  ObArray<int64_t> pending_rows;
  for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
    if (!skip.at(i) && !eval_flags.at(i) && OB_FAIL(pending_rows.push_back(i))) {
    }
  }
  if (OB_FAIL(ret) || pending_rows.empty()) {
    return ret; // Do not evaluate even constant arguments for an all-skip batch.
  }
  ObEvalCtx::TempAllocGuard alloc_guard(ctx);
  MultimodeAlloctor allocator(alloc_guard.get_allocator());
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
  ObEvalCtx::BatchInfoScopeGuard batch_guard(ctx);
  batch_guard.set_batch_size(size);
  ObArray<EmbedBatchGroup *> groups;
  ObArray<ObDatum> staged;
  ObBitVector *arg_skip = to_bit_vector(allocator.alloc(ObBitVector::memory_size(size)));
  if (OB_ISNULL(arg_skip)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(staged.prepare_allocate(size))) {
  } else {
    arg_skip->set_all(size);
    for (int64_t i = 0; i < pending_rows.count(); ++i) {
      const int64_t row = pending_rows.at(i);
      arg_skip->unset(row);
      staged.at(row).set_null();
    }
  }
  // Evaluate content first so NULL rows also skip model/dimension evaluation.
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(expr.args_[CONTENT_IDX]->eval_batch(ctx, *arg_skip, size))) {
  } else {
    ObDatumVector contents = expr.args_[CONTENT_IDX]->locate_expr_datumvector(ctx);
    int64_t valid_count = 0;
    for (int64_t i = 0; i < pending_rows.count(); ++i) {
      const int64_t row = pending_rows.at(i);
      if (contents.at(row)->is_null()) {
        arg_skip->set(row);
      } else {
        ++valid_count;
      }
    }
    if (valid_count == 0) {
      // All pending results stay NULL; no resolver/provider call is needed.
    } else if (OB_FAIL(expr.args_[MODEL_IDX]->eval_batch(ctx, *arg_skip, size))) {
    } else if (expr.arg_cnt_ == 3 && OB_FAIL(expr.args_[DIM_IDX]->eval_batch(ctx, *arg_skip, size))) {
    } else {
      ObDatumVector models = expr.args_[MODEL_IDX]->locate_expr_datumvector(ctx);
      ObDatumVector dimensions = expr.arg_cnt_ == 3
          ? expr.args_[DIM_IDX]->locate_expr_datumvector(ctx) : ObDatumVector();
      query::ObIAiEndpointResolver *resolver =
          ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
      for (int64_t i = 0; OB_SUCC(ret) && i < pending_rows.count(); ++i) {
        const int64_t row = pending_rows.at(i);
        if (arg_skip->at(row)) {
          continue;
        }
        batch_guard.set_batch_idx(row);
        ObDatum *model_datum = models.at(row);
        ObDatum *dimension_datum = expr.arg_cnt_ == 3 ? dimensions.at(row) : nullptr;
        ObString model_id;
        ObString content;
        int64_t dimension = 0;
        ObAIFuncExprInfo *info = nullptr;
        share::ObAiModelEndpointInfo endpoint;
        if (model_datum->is_null() || (OB_NOT_NULL(dimension_datum) && dimension_datum->is_null())) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or dimension is null");
        } else {
          model_id = model_datum->get_string();
          content = contents.at(row)->get_string();
          if (model_id.empty() || content.empty()) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
          } else if (OB_NOT_NULL(dimension_datum)) {
            dimension = dimension_datum->get_int();
            if (dimension <= 0) {
              ret = OB_INVALID_ARGUMENT;
              LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
            }
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_id, info))) {
        } else if (OB_ISNULL(info) || info->type_ != share::EndpointType::DENSE_EMBEDDING) {
          ret = OB_INVALID_ARGUMENT;
        } else if (OB_ISNULL(resolver)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(resolver->resolve_by_model_name(model_id, allocator, endpoint))) {
        } else {
          EmbedBatchGroup *group = nullptr;
          for (int64_t j = 0; OB_ISNULL(group) && j < groups.count(); ++j) {
            if (groups.at(j)->matches(model_id, *info, endpoint, dimension)) {
              group = groups.at(j);
            }
          }
          if (OB_ISNULL(group)) {
            if (OB_ISNULL(group = OB_NEWx(EmbedBatchGroup, &allocator))) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else if (OB_FAIL(groups.push_back(group))) {
              OB_DELETEx(EmbedBatchGroup, &allocator, group);
              group = nullptr;
            } else if (OB_FAIL(group->endpoint_.deep_copy(allocator, endpoint))) {
            } else if (OB_FAIL(ob_write_string(allocator, model_id, group->model_id_))) {
            } else {
              group->info_ = info;
              group->dimension_ = dimension;
            }
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(group->rows_.push_back(row))) {
          } else if (OB_FAIL(group->contents_.push_back(content))) {
          }
        }
      }
    }
  }
  // Each compatible group becomes one array-input request, not N scalar calls.
  for (int64_t i = 0; OB_SUCC(ret) && i < groups.count(); ++i) {
    EmbedBatchGroup &group = *groups.at(i);
    ObJsonObject *config = nullptr;
    ObJsonInt *dimension = nullptr;
    if (group.dimension_ > 0) {
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, group.dimension_, dimension))) {
      } else if (OB_FAIL(config->add("dimensions", dimension))) {
      }
    }
    if (OB_SUCC(ret)) {
      ObAIFuncModel model(allocator, *group.info_, group.endpoint_);
      if (OB_FAIL(model.call_dense_embedding_vector_v2(group.contents_, config, group.results_))) {
      } else if (group.results_.count() != group.rows_.count()) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
  }
  // Keep every JSON/result allocation alive until all persistent output copies
  // succeed. Stage datum descriptors; neither frame datums nor flags change yet.
  for (int64_t i = 0; OB_SUCC(ret) && i < groups.count(); ++i) {
    EmbedBatchGroup &group = *groups.at(i);
    for (int64_t j = 0; OB_SUCC(ret) && j < group.rows_.count(); ++j) {
      const int64_t row = group.rows_.at(j);
      batch_guard.set_batch_idx(row);
      ObString &value = group.results_.at(j);
      ObTextStringDatumResult output(expr.datum_meta_.type_, &expr, &ctx, &staged.at(row));
      if (OB_FAIL(output.init_with_batch_idx(value.length(), row))) {
      } else if (OB_FAIL(output.append(value))) {
      } else {
        output.set_result();
      }
    }
  }
  if (OB_SUCC(ret)) {
    ObDatum *results = expr.locate_batch_datums(ctx);
    for (int64_t i = 0; i < pending_rows.count(); ++i) {
      const int64_t row = pending_rows.at(i);
      results[row] = staged.at(row);
      eval_flags.set(row);
      if (results[row].is_null()) {
        expr.get_eval_info(ctx).notnull_ = false;
      }
    }
  }
  // On error this callback leaves all frame datums and existing flags untouched.
  // The generic do_eval_batch error cleanup is coordinated separately.
  for (int64_t i = 0; i < groups.count(); ++i) {
    OB_DELETEx(EmbedBatchGroup, &allocator, groups.at(i));
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
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
  //   ObObj const_data;
  //   bool got_data = false;
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
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
