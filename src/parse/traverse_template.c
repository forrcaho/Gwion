#include "gwion_util.h"
#include "gwion_ast.h"
#include "oo.h"
#include "env.h"
#include "nspc.h"
#include "traverse.h"
#include "template.h"
#include "vm.h"
#include "gwion.h"

ANN m_bool traverse_func_template(const Env env, const Func_Def def, const Type_List types) {
  CHECK_BB(template_push_types(env, def->tmpl->list, types))
  return traverse_func_def(env, def);
}
