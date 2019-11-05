#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "gwion_util.h"
#include "gwion_ast.h"
#include "oo.h"
#include "vm.h"
#include "env.h"
#include "type.h"
#include "value.h"
#include "traverse.h"
#include "instr.h"
#include "object.h"
#include "emit.h"
#include "func.h"
#include "nspc.h"
#include "gwion.h"
#include "operator.h"
#include "import.h"
#include "gwi.h"
#include "mpool.h"
#include "specialid.h"
#include "template.h"

ANN static m_bool mk_xtor(MemPool p, const Type type, const m_uint d, const ae_flag e) {
  VM_Code* code = e == ae_flag_ctor ? &type->nspc->pre_ctor : &type->nspc->dtor;
  const m_str name = type->name;
  *code = new_vm_code(p, NULL, SZ_INT, e | ae_flag_member | ae_flag_builtin, name);
  (*code)->native_func = (m_uint)d;
  type->flag |= e;
  return GW_OK;
}

ANN2(1,2) static void import_class_ini(const Env env, const Type t) {
  t->nspc = new_nspc(env->gwion->mp, t->name);
  t->nspc->parent = env->curr;
  if(t->e->parent && t->e->parent->nspc) {
    t->nspc->info->offset = t->e->parent->nspc->info->offset;
    if(t->e->parent->nspc->info->vtable.ptr)
      vector_copy2(&t->e->parent->nspc->info->vtable, &t->nspc->info->vtable);
  }
  t->e->owner = env->curr;
  SET_FLAG(t, checked);
  env_push_type(env, t);
}

ANN2(1) void gwi_class_xtor(const Gwi gwi, const f_xtor ctor, const f_xtor dtor) {
  const Type t = gwi->gwion->env->class_def;
  if(ctor)
    mk_xtor(gwi->gwion->mp, t, (m_uint)ctor, ae_flag_ctor);
  if(dtor)
    mk_xtor(gwi->gwion->mp, t, (m_uint)dtor, ae_flag_dtor);
}

ANN static Type type_finish(const Gwi gwi, const Type t) {
  SET_FLAG(t, scan1 | ae_flag_scan2 | ae_flag_check | ae_flag_emit);
  gwi_add_type(gwi, t);
  import_class_ini(gwi->gwion->env, t);
  return t;
}

ANN2(1,2) Type gwi_class_ini(const Gwi gwi, const m_str name, const m_str parent) {
  struct ImportCK ck = { .name=name };
  CHECK_BO(check_typename_def(gwi, &ck))
  Type_Decl *td = str2decl(gwi, parent ?: "Object"); // check
  Tmpl* tmpl = ck.tmpl ? new_tmpl(gwi->gwion->mp, ck.tmpl, -1) : NULL;
  if(tmpl)
    template_push_types(gwi->gwion->env, tmpl);
  const Type p = known_type(gwi->gwion->env, td); // check
  if(tmpl)
    nspc_pop_type(gwi->gwion->mp, gwi->gwion->env->curr);
  const Type t = new_type(gwi->gwion->mp, ++gwi->gwion->env->scope->type_xid, s_name(ck.sym), p);
  t->e->def = new_class_def(gwi->gwion->mp, 0, ck.sym, td, NULL, loc(gwi));
  t->e->def->base.tmpl = tmpl;
  t->e->def->base.type = t;
  t->e->parent = p;
  if(td->array)
    SET_FLAG(t, typedef);
  if(ck.tmpl)
    SET_FLAG(t, template);
  return type_finish(gwi, t);
}

ANN Type gwi_class_spe(const Gwi gwi, const m_str name, const m_uint size) {
  CHECK_OO(str2sym(gwi, name))
  const Type t = new_type(gwi->gwion->mp, ++gwi->gwion->env->scope->type_xid, name, NULL);
  t->size = size;
  return type_finish(gwi, t);
}

ANN m_int gwi_class_end(const Gwi gwi) {
  if(!gwi->gwion->env->class_def)
    GWI_ERR_B(_("import: too many class_end called."))
  nspc_allocdata(gwi->gwion->mp, gwi->gwion->env->class_def->nspc);
  env_pop(gwi->gwion->env, 0);
  return GW_OK;
}
