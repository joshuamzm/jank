#include <iostream>
#include <atomic>

#include <fmt/core.h>

#include <jank/runtime/obj/vector.hpp>
#include <jank/runtime/obj/map.hpp>
#include <jank/analyze/processor.hpp>
#include <jank/analyze/expr/primitive_literal.hpp>
#include <jank/result.hpp>

namespace jank::analyze
{
  context::context(runtime::context &rt_ctx)
    : rt_ctx{ rt_ctx }
  { }

  void context::dump() const
  {
    std::cout << "analysis ctx dump begin" << std::endl;
    for(auto const &v : vars)
    { std::cout << "  var " << *v.first << std::endl; }
    std::cout << "analysis ctx dump end" << std::endl;
  }

  option<std::pair<runtime::obj::symbol_ptr, option<expression_ptr>>> context::find_var
  (runtime::obj::symbol_ptr const &sym) const
  {
    runtime::obj::symbol_ptr qualified_sym{ sym };
    if(qualified_sym->ns.empty())
    {
      auto const t_state(rt_ctx.get_thread_state());
      auto const * const current_ns(t_state.current_ns->get_root()->as_ns());
      qualified_sym = runtime::obj::symbol::create(current_ns->name->name, sym->name);
    }

    auto const found(vars.find(qualified_sym));
    if(found == vars.end())
    { return none; }
    return some(*found);
  }

  runtime::obj::symbol context::unique_name()
  { return unique_name("gen"); }
  runtime::obj::symbol context::unique_name(std::string_view const &prefix)
  {
    static std::atomic_size_t index{ 1 };
    return { "", prefix.data() + std::to_string(index++) };
  }

  processor::processor
  (
    runtime::context &rt_ctx,
    read::parse::processor::iterator const &b,
    read::parse::processor::iterator const &e
  )
    : rt_ctx{ rt_ctx }
    , root_frame{ std::make_shared<local_frame>(local_frame::frame_type::root, rt_ctx, none) }
    , parse_current{ b }, parse_end{ e }
  {
    using runtime::obj::symbol;
    auto const make_fn = [this](auto const fn) -> decltype(specials)::mapped_type
    {
      return [this, fn](auto const &list, auto &current_frame, auto &ctx)
      { return (this->*fn)(list, current_frame, ctx); };
    };
    specials =
    {
      { symbol::create("def"), make_fn(&processor::analyze_def) },
      { symbol::create("fn*"), make_fn(&processor::analyze_fn) },
      { symbol::create("let*"), make_fn(&processor::analyze_let) },
      { symbol::create("if"), make_fn(&processor::analyze_if) },
      { symbol::create("quote"), make_fn(&processor::analyze_quote) },
      { symbol::create("native/raw"), make_fn(&processor::analyze_native_raw) },
    };
  }

  processor::expression_result processor::result(context &ctx)
  {
    if(parse_current == parse_end)
    { return ok(none); }

    /* We wrap all of the expressions we get in an anonymous fn so that we can call it easily.
     * This also simplifies codegen, since we only ever codegen a single fn, even if that fn
     * represents a ns, a single REPL expression, or an actual source fn. */
    runtime::detail::vector_transient_type fn_body;
    fn_body.push_back(runtime::obj::symbol::create("fn*"));
    fn_body.push_back(runtime::obj::vector::create());
    for(; parse_current != parse_end; ++parse_current)
    {
      if(parse_current->is_err())
      { return err(parse_current->expect_err_move()); }
      fn_body.push_back(parse_current->expect_ok());
    }
    auto fn(runtime::obj::list::create(fn_body.rbegin(), fn_body.rend()));
    return analyze(std::move(fn), ctx);
  }

  processor::expression_result processor::analyze_def
  (runtime::obj::list_ptr const &l, local_frame_ptr &current_frame, context &ctx)
  {
    auto const length(l->count());
    if(length != 2 && length != 3)
    {
      /* TODO: Error handling. */
      return err(error{ "invalid def" });
    }

    auto const sym_obj(l->data.rest().first().unwrap());
    auto const * const sym(sym_obj->as_symbol());
    if(sym == nullptr)
    {
      /* TODO: Error handling. */
      return err(error{ "invalid def: name must be a symbol" });
    }
    else if(!sym->ns.empty())
    {
      /* TODO: Error handling. */
      return err(error{ "invalid def: name must not be qualified" });
    }

    bool has_value{ true };
    auto const value_opt(l->data.rest().rest().first());
    if(value_opt.is_none())
    { has_value = false; }

    option<std::shared_ptr<expression>> value_expr;

    if(has_value)
    {
      auto value_result(analyze(value_opt.unwrap(), current_frame, ctx));
      if(value_result.is_err())
      { return value_result; }
      value_expr = some(std::make_shared<expression>(value_result.expect_ok().unwrap()));
    }

    auto const qualified_sym(current_frame->lift_var(boost::static_pointer_cast<runtime::obj::symbol>(sym_obj)));
    /* TODO: Should this use find_var? */
    auto const existing_var(ctx.vars.find(qualified_sym));
    if(existing_var != ctx.vars.end())
    {
      /* TODO: If backward checking is enabled, type check new value type with old one. */
      existing_var->second = value_expr;
    }
    else
    { ctx.vars.emplace(qualified_sym, value_expr); }

    return
    {
      expr::def<expression>
      {
        qualified_sym,
        value_expr,
        current_frame
      }
    };
  }

  processor::expression_result processor::analyze_symbol
  (runtime::obj::symbol_ptr const &sym, local_frame_ptr &current_frame, context &ctx)
  {
    /* TODO: Assert it doesn't start with __. */
    auto const found_local(current_frame->find_capture(sym));
    if(found_local.is_some())
    {
      local_frame::register_captures(found_local.unwrap());
      return { expr::local_reference{ sym, found_local.unwrap().binding } };
    }

    auto const var(ctx.find_var(sym));
    if(var.is_none())
    { return err(error{ "unbound symbol: " + *sym->to_string().data }); }

    current_frame->lift_var(var.unwrap().first);
    return { expr::var_deref<expression>{ var.unwrap().first, current_frame } };
  }

  result<expr::function_arity<expression>, error> processor::analyze_fn_arity
  (runtime::obj::list_ptr const &list, local_frame_ptr &current_frame, context &ctx)
  {
    auto const params_obj(list->data.first().unwrap());
    auto const * const params(params_obj->as_vector());
    if(params == nullptr)
    { return err(error{ "invalid fn parameter vector" }); }

    local_frame_ptr frame
    { std::make_shared<local_frame>(local_frame::frame_type::fn, current_frame->rt_ctx, current_frame) };
    std::vector<runtime::obj::symbol_ptr> param_symbols;
    param_symbols.reserve(params->data.size());

    bool is_variadic{};
    for(auto it(params->data.begin()); it != params->data.end(); ++it)
    {
      auto const &p(*it);
      auto const * const sym(p->as_symbol());
      if(sym == nullptr)
      { return err(error{ "invalid parameter; must be a symbol" }); }
      else if(!sym->ns.empty())
      { return err(error{ "invalid parameter; must be unqualified" }); }
      else if(sym->name == "&")
      {
        if(is_variadic)
        { return err(error{ "invalid function; parameters contain mutliple &" }); }
        else if(it + 1 == params->data.end())
        { return err(error{ "invalid function; missing symbol after &" }); }

        is_variadic = true;
        continue;
      }

      auto const sym_ptr(boost::static_pointer_cast<runtime::obj::symbol>(p));
      frame->locals.emplace(sym_ptr, local_binding{ sym_ptr, none });
      param_symbols.emplace_back(sym_ptr);
    }

    /* We do this after building the symbols vector, since the & symbol isn't a param
     * and would cause an off-by-one error. */
    if(param_symbols.size() > 10)
    { return err(error{ "invalid parameter count; must be <= 10; use & args to capture the rest" }); }

    expr::do_<expression> body_do;
    for(auto const &item : list->data.rest())
    {
      auto form(analyze(item, frame, ctx));
      if(form.is_err())
      { return form.expect_err_move(); }
      else if(form.expect_ok().is_none())
      { return err(read::error{ "expected fn body value" }); }
      body_do.body.emplace_back(std::move(form.expect_ok_move().unwrap()));
    }

    return
    {
      expr::function_arity<expression>
      { std::move(param_symbols), is_variadic, std::move(body_do), std::move(frame) }
    };
  }

  processor::expression_result processor::analyze_fn
  (runtime::obj::list_ptr const &list, local_frame_ptr &current_frame, context &ctx)
  {
    auto const length(list->count());
    if(length < 2)
    { return err(error{ "fn missing forms" }); }

    std::vector<expr::function_arity<expression>> arities;

    auto const first_elem(list->data.rest().first().unwrap());
    if(first_elem->as_vector() != nullptr)
    {
      auto result
      (
        analyze_fn_arity
        (runtime::make_box<runtime::obj::list>(list->data.rest()), current_frame, ctx)
      );
      if(result.is_err())
      { return result.expect_err_move(); }
      arities.emplace_back(result.expect_ok_move());
    }
    else if(first_elem->as_list() != nullptr)
    {
      for(auto it(list->data.rest()); it.size() > 0; it = it.rest())
      {
        auto arity_list(it.first().unwrap());
        if(arity_list->as_list() == nullptr)
        { return err(error{ "invalid fn: expected arity list" }); }

        auto result
        (
          analyze_fn_arity
          (boost::static_pointer_cast<runtime::obj::list>(arity_list), current_frame, ctx)
        );
        if(result.is_err())
        { return result.expect_err_move(); }
        arities.emplace_back(result.expect_ok_move());
      }
    }
    else
    { return err(error{ "invalid fn syntax" }); }

    /* There can only be one variadic arity. Clojure requires this. */
    size_t found_variadic{};
    size_t variadic_arity{};
    for(auto const &arity : arities)
    {
      found_variadic += static_cast<int>(arity.is_variadic);
      variadic_arity = arity.params.size();
    }
    if(found_variadic > 1)
    { return err(error{ "invalid fn: has more than one variadic arity" }); }

    /* The variadic arity, if present, must have at least as many fixed params as the
     * highest non-variadic arity. Clojure requires this. */
    if(found_variadic > 0)
    {
      for(auto const &arity : arities)
      {
        if(!arity.is_variadic && arity.params.size() >= variadic_arity)
        { return err(error{ "invalid fn: fixed arity has >= params than variadic arity" }); }
      }
    }

    /* Assert that arities are unique. Lazy implementation, but N is small anyway. */
    for(auto base(arities.begin()); base != arities.end(); ++base)
    {
      if(base + 1 == arities.end())
      { break; }

      for(auto other(base + 1); other != arities.end(); ++other)
      {
        if
        (
          base->params.size() == other->params.size()
          && base->is_variadic == other->is_variadic
        )
        { return err(error{ "invalid fn: duplicate arity definition" }); }
      }
    }

    return { expr::function<expression> { none, std::move(arities) } };
  }

  processor::expression_result processor::analyze_let
  (
    runtime::obj::list_ptr const &o,
    local_frame_ptr &current_frame,
    context &ctx
  )
  {
    if(o->count() < 2)
    { return err(error{ "invalid let: expects bindings" }); }

    auto const bindings_obj(o->data.rest().first().unwrap());
    auto const * const bindings(bindings_obj->as_vector());
    if(bindings == nullptr)
    { return err(error{ "invalid let* bindings: must be a vector" }); }

    auto const binding_parts(bindings->data.size());
    if(binding_parts % 2 == 1)
    { return err(error{ "invalid let* bindings: must be an even number" }); }

    expr::let<expression> ret
    {
      {},
      {},
      std::make_shared<local_frame>
      (local_frame::frame_type::let, current_frame->rt_ctx, current_frame)
    };
    for(size_t i{}; i < binding_parts; i += 2)
    {
      auto const &sym_obj(bindings->data[i]);
      auto const &val(bindings->data[i + 1]);

      auto const &sym(sym_obj->as_symbol());
      if(sym == nullptr || !sym->ns.empty())
      { return err(error{ "invalid let* binding: left hand must be an unqualified symbol" }); }

      auto const sym_ptr(boost::static_pointer_cast<runtime::obj::symbol>(sym_obj));
      /* TODO: Return errors. */
      auto it(ret.pairs.emplace_back(sym_ptr, analyze(val, ret.frame, ctx).expect_ok().unwrap()));
      ret.frame->locals.emplace(sym_ptr, local_binding{ sym_ptr, some(std::ref(it.second)) });
      /* TODO: Rename shadowed bindings? */
    }

    for(auto const &item : o->data.rest().rest())
    /* TODO: Return errors. */
    { ret.body.body.emplace_back(analyze(item, ret.frame, ctx).expect_ok().unwrap()); }

    return { std::move(ret) };
  }

  processor::expression_result processor::analyze_if
  (
    runtime::obj::list_ptr const &o,
    local_frame_ptr &current_frame,
    context &ctx
  )
  {
    auto const form_count(o->count());
    if(form_count < 3)
    { return err(error{ "invalid if: expects at least two forms" }); }
    else if(form_count > 4)
    { return err(error{ "invalid if: expects at most three forms" }); }

    auto const condition(o->data.rest().first().unwrap());
    auto condition_expr(analyze(condition, current_frame, ctx));
    if(condition_expr.is_err())
    { return condition_expr.expect_err_move(); }

    auto const then(o->data.rest().rest().first().unwrap());
    auto then_expr(analyze(then, current_frame, ctx));
    if(then_expr.is_err())
    { return then_expr.expect_err_move(); }

    option<expression_ptr> else_expr_opt;
    if(form_count == 4)
    {
      auto const else_(o->data.rest().rest().rest().first().unwrap());
      auto else_expr(analyze(else_, current_frame, ctx));
      if(else_expr.is_err())
      { return else_expr.expect_err_move(); }
      else_expr_opt = std::make_shared<expression>(else_expr.expect_ok_move().unwrap());
    }

    return
    {
      expr::if_<expression>
      {
        std::make_shared<expression>(condition_expr.expect_ok_move().unwrap()),
        std::make_shared<expression>(then_expr.expect_ok_move().unwrap()),
        else_expr_opt
      }
    };
  }

  processor::expression_result processor::analyze_quote
  (runtime::obj::list_ptr const &o, local_frame_ptr &current_frame, context &ctx)
  {
    if(o->count() != 2)
    { return err(error{ "invalid quote: expects one argument" }); }

    return analyze_primitive_literal(o->data.rest().first().unwrap(), current_frame, ctx);
  }

  processor::expression_result processor::analyze_native_raw
  (runtime::obj::list_ptr const &o, local_frame_ptr &current_frame, context &ctx)
  {
    if(o->count() != 2)
    { return err(error{ "invalid native/raw: expects one argument" }); }

    auto const &code(o->data.rest().first().unwrap());
    auto const * const code_str(code->as_string());
    if(code_str == nullptr)
    { return err(error{ "invalid native/raw: expects string of C++ code" }); }
    if(code_str->data.empty())
    { return { expr::native_raw<expression>{ } }; }

    /* native/raw expressions are broken up into chunks of either literal C++ code or
     * interpolated jank code, the latter needing to also be analyzed. */
    decltype(expr::native_raw<expression>::chunks) chunks;
    /* TODO: Just use } for end and rely on token parsing info for when that is.
     * This requires storing line/col start/end meta in each object. */
    constexpr std::string_view interp_start{ "#{" }, interp_end{ "}#" };
    for(size_t it{}; it != std::string::npos; )
    {
      auto const next_start(code_str->data.data->find(interp_start, it));
      if(next_start == std::string::npos)
      {
        /* This is the final chunk. */
        chunks.emplace_back(std::string_view{ code_str->data.data->data() + it });
        break;
      }
      auto const next_end(code_str->data.data->find(interp_end, next_start));
      if(next_end == std::string::npos)
      { return err(error{ "no matching }$ found for native/raw interpolation" }); }

      read::lex::processor l_prc
      {
        {
          code_str->data.data->data() + next_start + interp_start.size(),
          next_end - next_start - interp_end.size()
        }
      };
      read::parse::processor p_prc{ l_prc.begin(), l_prc.end() };
      auto parsed_it(p_prc.begin());
      if(parsed_it->is_err())
      { return parsed_it->expect_err_move(); }
      auto result(analyze(parsed_it->expect_ok(), current_frame, ctx));
      if(result.is_err())
      { return result.expect_err_move(); }

      if(next_start - it > 0)
      { chunks.emplace_back(std::string_view{ code_str->data.data->data() + it, next_start - it }); }
      chunks.emplace_back(result.expect_ok_move().unwrap());
      it = next_end + interp_end.size();

      if(++parsed_it != p_prc.end())
      { return err(error{ "invalid native/raw: only one expression per interpolation" }); }
    }

    return { expr::native_raw<expression>{ std::move(chunks) } };
  }

  processor::expression_result processor::analyze_primitive_literal
  (runtime::object_ptr const &o, local_frame_ptr &current_frame, context &)
  {
    current_frame->lift_constant(o);
    return { expr::primitive_literal<expression>{ o, current_frame } };
  }

  /* TODO: Test for this. */
  processor::expression_result processor::analyze_vector
  (runtime::obj::vector_ptr const &o, local_frame_ptr &current_frame, context &ctx)
  {
    /* TODO: Detect literal and act accordingly. */
    std::vector<expression> exprs;
    exprs.reserve(o->count());
    for(auto d = o->seq(); d != nullptr; d = d->next())
    /* TODO: Return errors. */
    { exprs.emplace_back(analyze(d->first(), current_frame, ctx).expect_ok().unwrap()); }
    return { expr::vector<expression>{ std::move(exprs) } };
  }

  processor::expression_result processor::analyze_map
  (runtime::obj::map_ptr const &o, local_frame_ptr &current_frame, context &ctx)
  {
    /* TODO: Detect literal and act accordingly. */
    std::vector<std::pair<expression, expression>> exprs;
    exprs.reserve(o->data.size());
    for(auto const &kv : o->data)
    /* TODO: Return errors. */
    {
      auto const k_expr(analyze(kv.first, current_frame, ctx).expect_ok().unwrap());
      auto const v_expr(analyze(kv.second, current_frame, ctx).expect_ok().unwrap());
      exprs.emplace_back(k_expr, v_expr);
    }

    /* TODO: Uniqueness check. */
    return { expr::map<expression>{ std::move(exprs) } };
  }

  /* This is largely a hack until we have type information for these things. */
  result<option<size_t>, error> match_fn_call
  (expression_ptr const &source, std::vector<expression> const &args, context &ctx)
  {
    expr::function<expression> const *fn{};
    boost::apply_visitor
    (
      [&fn, &ctx](auto const &arg)
      {
        using T = std::decay_t<decltype(arg)>;
        if constexpr(std::is_same_v<T, expr::var_deref<expression>>)
        {
          auto const found_var(ctx.find_var(arg.qualified_name));
          auto const &var_expr(found_var.unwrap().second);
          if(var_expr.is_none())
          { return; }
          fn = boost::get<expr::function<expression>>(&var_expr.unwrap()->data);
        }
        else if constexpr(std::is_same_v<T, expr::local_reference>)
        {
          if(arg.binding.value_expr.is_none())
          { return; }
          fn = boost::get<expr::function<expression>>(&arg.binding.value_expr.unwrap().get().data);
        }
        else if constexpr(std::is_same_v<T, expr::function<expression>>)
        { fn = &arg; }
      },
      source->data
    );

    /* TODO: Support other callables. */
    if(!fn)
    { return none; }

    /* TODO: Put this into fn expr. */
    std::unordered_map<expr::arity_key, none_t> arity_map;
    for(auto const &arity : fn->arities)
    { arity_map.emplace(expr::arity_key{ arity.params.size(), arity.is_variadic }, none); }

    size_t const arg_count{ args.size() };
    expr::arity_key lookup{ arg_count, false };
    option<size_t> required_packed_args{};
    auto const found_concrete(arity_map.find(lookup));
    if(found_concrete != arity_map.end())
    { required_packed_args = 0; }
    else
    {
      for(size_t packed_args{}; packed_args <= arg_count; ++packed_args)
      {
        lookup.arg_count = arg_count - packed_args;
        lookup.is_variadic = true;
        auto const found_variadic(arity_map.find(lookup));
        if(found_variadic != arity_map.end())
        {
          required_packed_args = packed_args + 1;
          break;
        }
      }
    }

    if(required_packed_args.is_none())
    { return err(error{ "invalid call to fn: unmatched arity" }); }

    return required_packed_args;
  }

  processor::expression_result processor::analyze_call
  (runtime::obj::list_ptr const &o, local_frame_ptr &current_frame, context &ctx)
  {
    /* An empty list evaluates to a list, not a call. */
    auto const count(o->count());
    if(count == 0)
    { return analyze_primitive_literal(o, current_frame, ctx); }

    auto const first(o->data.first().unwrap());
    expression_ptr source;
    if(first->as_symbol())
    {
      auto const sym(boost::static_pointer_cast<runtime::obj::symbol>(first));
      auto const found_special(specials.find(sym));
      if(found_special != specials.end())
      { return found_special->second(o, current_frame, ctx); }

      auto sym_result(analyze_symbol(sym, current_frame, ctx));
      if(sym_result.is_err())
      { return sym_result; }

      source = std::make_shared<expression>(sym_result.expect_ok().unwrap());
    }
    else
    {
      auto callable_expr(analyze(first, current_frame, ctx));
      if(callable_expr.is_err())
      { return callable_expr; }
      source = std::make_shared<expression>(callable_expr.expect_ok().unwrap());
    }

    /* TODO: Verify source is callable. */

    std::vector<expression> arg_exprs;
    arg_exprs.reserve(count - 1);
    for(auto const &s : o->data.rest())
    {
      auto arg_expr(analyze(s, current_frame, ctx));
      if(arg_expr.is_err())
      { return arg_expr; }
      arg_exprs.emplace_back(arg_expr.expect_ok().unwrap());
    }

    auto const match_result(match_fn_call(source, arg_exprs, ctx));
    if(match_result.is_err())
    { return match_result.expect_err(); }

    return
    {
      expr::call<expression>
      {
        source,
        runtime::obj::list::create(o->data.rest()),
        arg_exprs,
        match_result.expect_ok()
      }
    };
  }

  processor::expression_result processor::analyze(runtime::object_ptr const &o, context &ctx)
  { return analyze(o, root_frame, ctx); }

  processor::expression_result processor::analyze
  (runtime::object_ptr const &o, local_frame_ptr &current_frame, context &ctx)
  {
    assert(o);

    if(o->as_list())
    { return analyze_call(boost::static_pointer_cast<runtime::obj::list>(o), current_frame, ctx); }
    else if(o->as_vector())
    { return analyze_vector(boost::static_pointer_cast<runtime::obj::vector>(o), current_frame, ctx); }
    else if(o->as_map())
    { return analyze_map(boost::static_pointer_cast<runtime::obj::map>(o), current_frame, ctx); }
    else if(o->as_set())
    { return err(error{ "unimplemented analysis: set" }); }
    else if(o->as_number() || o->as_boolean() || o->as_keyword() || o->as_nil())
    { return analyze_primitive_literal(o, current_frame, ctx); }
    else if(o->as_string())
    { return err(error{ "unimplemented analysis: string" }); }
    else if(o->as_symbol())
    { return analyze_symbol(boost::static_pointer_cast<runtime::obj::symbol>(o), current_frame, ctx); }
    else
    {
      std::cerr << "unsupported analysis of " << o->to_string() << std::endl;
      return err(error{ "unimplemented analysis" });
    }
  }
}
