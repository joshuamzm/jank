#include <jank/runtime/var.hpp>
#include <jank/runtime/ns.hpp>
#include <jank/runtime/hash.hpp>

namespace jank::runtime
{
  var::var(ns_ptr const &n, obj::symbol_ptr const &s)
    : n{ n }, name{ s }
  { }
  var::var(ns_ptr const &n, obj::symbol_ptr const &s, object_ptr const &o)
    : n{ n }, name{ s }, root{ o }
  { }

  /* TODO: If ns already has var, don't make a new one. */
  detail::box_type<var> var::create(ns_ptr const &n, obj::symbol_ptr const &s)
  { return make_box<var>(n, s); }

  detail::box_type<var> var::create(ns_ptr const &n, obj::symbol_ptr const &s, object_ptr const &root)
  { return make_box<var>(n, s, root); }

  runtime::detail::boolean_type var::equal(object const &o) const
  {
    auto const *v(o.as_var());
    if(!v)
    { return false; }

    return n == v->n && name == v->name;
  }
  runtime::detail::string_type var::to_string() const
  /* TODO: Maybe cache this. */
  { return "#'" + name->to_string(); }
  runtime::detail::integer_type var::to_hash() const
  /* TODO: Cache this. */
  { return detail::hash_combine(n->name->to_hash(), name->to_hash()); }

  var const* var::as_var() const
  { return this; }

  object_ptr var::get_root() const
  { return *root.lock_shared(); }

  var_ptr var::set_root(object_ptr const &r)
  {
    *root.lock() = r;
    return ptr_from_this();
  }
}
