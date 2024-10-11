#include <sqlite3.h>

/* https://sqlite.org */
namespace jank::data::sqlite::impl
{
  using namespace jank;
  using namespace jank::runtime;

  object_ptr open(object_ptr const filename_str_obj)
  {
    return obj::nil::nil_const();
  }

  /* TODO: Make base class for ns which does this_object_ptr */
  struct __ns : behavior::callable
  {
    /* TODO: Remove const from all of these. */
    object_ptr call() const override
    {
      auto const ns(__rt_ctx->intern_ns("jank.data.sqlite.impl"));
      ns->intern_var("open")->bind_root(make_box<obj::native_function_wrapper>(&open));
      return obj::nil::nil_const();
    }

    object_ptr this_object_ptr() const override
    {
      return obj::nil::nil_const();
    }
  };
}
