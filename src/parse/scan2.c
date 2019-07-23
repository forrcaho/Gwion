#include <string.h>
#include "gwion_util.h"
#include "gwion_ast.h"
#include "oo.h"
#include "vm.h"
#include "env.h"
#include "type.h"
#include "value.h"
#include "func.h"
#include "template.h"
#include "traverse.h"
#include "optim.h"
#include "parse.h"
#include "nspc.h"
#include "operator.h"

ANN static m_bool scan2_stmt(const Env, const Stmt);
ANN static m_bool scan2_stmt_list(const Env, Stmt_List);

ANN static m_bool scan2_exp_decl_template(const Env env, const Exp_Decl* decl) {
  CHECK_BB(scan1_cdef(env, decl->type->e->def))
  return scan2_cdef(env, decl->type->e->def);
}

ANN m_bool scan2_exp_decl(const Env env, const Exp_Decl* decl) {
  const m_bool global = GET_FLAG(decl->td, global);
  const m_uint scope = !global ? env->scope->depth : env_push_global(env);
  const Type type = decl->type;
  if(GET_FLAG(type, template) && !GET_FLAG(type, scan2))
    CHECK_BB(scan2_exp_decl_template(env, decl))
  Var_Decl_List list = decl->list;
  do {
    const Var_Decl var = list->self;
    const Exp array = var->array ? var->array->exp : NULL;
    if(array)
      CHECK_BB(scan2_exp(env, array))
    nspc_add_value(env->curr, var->xid, var->value);
  } while((list = list->next));
  if(global)
    env_pop(env, scope);
  return GW_OK;
}

ANN static Value arg_value(MemPool p, const Arg_List list) {
  const Var_Decl var = list->var_decl;
  if(!var->value) {
    const Value v = new_value(p, list->type, var->xid ? s_name(var->xid) : (m_str)__func__);
    if(list->td)
      v->flag = list->td->flag | ae_flag_arg;
    return v;
  }
  var->value->type = list->type;
  return var->value;
}

ANN static m_bool scan2_args(const Env env, const Func_Def f) {
  Arg_List list = f->base->args;
  do {
    const Var_Decl var = list->var_decl;
    if(var->array)
      list->type = array_type(env, list->type, var->array->depth);
    var->value = arg_value(env->gwion->mp, list);
    var->value->offset = f->stack_depth;
    f->stack_depth += list->type->size;
  } while((list = list->next));
  return GW_OK;
}

ANN static Value scan2_func_assign(const Env env, const Func_Def d,
    const Func f, const Value v) {
  v->owner = env->curr;
  SET_FLAG(v, func | ae_flag_const);
  if(!(v->owner_class = env->class_def))
    SET_FLAG(v, global);
  else {
    if(GET_FLAG(f, member))
      SET_FLAG(v, member);
    else SET_FLAG(v, static);
    SET_ACCESS(d, v)
  }
  d->base->func = v->d.func_ref = f;
  return f->value_ref = v;
}


ANN void fptr_assign(const Env env, const Stmt_Fptr ptr) {
  const Func_Def def = ptr->type->e->d.func->def;
  if(GET_FLAG(ptr->base->td, global)) {
    SET_FLAG(ptr->value, global);
    SET_FLAG(ptr->base->func, global);
    SET_FLAG(def, global);
  } else if(!GET_FLAG(ptr->base->td, static)) {
    SET_FLAG(ptr->value, member);
    SET_FLAG(ptr->base->func, member);
    SET_FLAG(def, member);
    def->stack_depth += SZ_INT;
  } else {
    SET_FLAG(ptr->value, static);
    SET_FLAG(ptr->base->func, static);
    SET_FLAG(def, static);
  }
  if(GET_FLAG(def, variadic))
    def->stack_depth += SZ_INT;
  ptr->value->owner_class = env->class_def;
}

ANN m_bool scan2_stmt_fptr(const Env env, const Stmt_Fptr ptr) {
  const Func_Def def = ptr->type->e->d.func->def;
  if(!tmpl_base(ptr->base->tmpl)) {
    def->base->ret_type = ptr->base->ret_type;
    if(ptr->base->args)
      CHECK_BB(scan2_args(env, def))
  } else
    SET_FLAG(ptr->type, func);
  nspc_add_func(ptr->type->e->owner, ptr->base->xid, ptr->base->func);
  return GW_OK;
}

ANN m_bool scan2_stmt_type(const Env env, const Stmt_Type stmt) {
  return stmt->type->e->def ? scan2_class_def(env, stmt->type->e->def) : GW_OK;
}

ANN static inline Value prim_value(const Env env, const Symbol s) {
  const Value value = nspc_lookup_value1(env->curr, s);
  if(env->class_def)
    return value ?: find_value(env->class_def, s);
  return value;
}

ANN static inline m_bool scan2_exp_primary(const Env env, const Exp_Primary* prim) {
  if(prim->primary_type == ae_primary_hack) {
    CHECK_BB(scan2_exp(env, prim->d.exp))
    Exp e = prim->d.exp;
    do {
      if(e->exp_type == ae_exp_decl) {
        Var_Decl_List l = e->d.exp_decl.list;
        do {
           const Value v = l->self->value;
           SET_FLAG(v, used);
        }while ((l = l->next));
      }
    } while((e = e->next));
  } else if(prim->primary_type == ae_primary_id) {
    const Value v = prim_value(env, prim->d.var);
    if(v)
      SET_FLAG(v, used);
  } else if(prim->primary_type == ae_primary_array && prim->d.array->exp)
    return scan2_exp(env, prim->d.array->exp);
  return GW_OK;
}

ANN static inline m_bool scan2_exp_array(const Env env, const Exp_Array* array) {
  CHECK_BB(scan2_exp(env, array->base))
  return scan2_exp(env, array->array->exp);
}


ANN static m_bool multi_decl(const Env env, const Exp e, const Symbol op) {
  if(e->exp_type == ae_exp_decl) {
    if(e->d.exp_decl.list->next)
      ERR_B(e->pos, _("cant '%s' from/to a multi-variable declaration."), s_name(op))
    SET_FLAG(e->d.exp_decl.list->self->value, used);
  }
  return GW_OK;
}

ANN static inline m_bool scan2_exp_binary(const Env env, const Exp_Binary* bin) {
  CHECK_BB(scan2_exp(env, bin->lhs))
  CHECK_BB(scan2_exp(env, bin->rhs))
  CHECK_BB(multi_decl(env, bin->lhs, bin->op))
  return multi_decl(env, bin->rhs, bin->op);
}

ANN static inline m_bool scan2_exp_cast(const Env env, const Exp_Cast* cast) {
  return scan2_exp(env, cast->exp);
}

ANN static inline m_bool scan2_exp_post(const Env env, const Exp_Postfix* post) {
  return scan2_exp(env, post->exp);
}

ANN2(1,2) static inline m_bool scan2_exp_call1(const Env env, const restrict Exp func,
    const restrict Exp args) {
  CHECK_BB(scan2_exp(env, func))
  return args ? scan2_exp(env, args) : 1;
}

ANN static inline m_bool scan2_exp_call(const Env env, const Exp_Call* exp_call) {
    return scan2_exp_call1(env, exp_call->func, exp_call->args);
}

ANN static inline m_bool scan2_exp_dot(const Env env, const Exp_Dot* member) {
  return scan2_exp(env, member->base);
}

ANN static inline m_bool scan2_exp_if(const Env env, const Exp_If* exp_if) {
  CHECK_BB(scan2_exp(env, exp_if->cond))
  CHECK_BB(scan2_exp(env, exp_if->if_exp ?: exp_if->cond))
  return scan2_exp(env, exp_if->else_exp);
}

ANN static m_bool scan2_exp_unary(const Env env, const Exp_Unary * unary) {
  if((unary->op == insert_symbol("spork") || unary->op == insert_symbol("fork")) && unary->code) {
    RET_NSPC(scan2_stmt(env, unary->code))
  } else if(unary->exp)
    return scan2_exp(env, unary->exp);
  return GW_OK;
}

ANN static inline m_bool scan2_exp_typeof(const restrict Env env, const Exp_Typeof *exp) {
  return scan2_exp(env, exp->exp);
}

#define scan2_exp_lambda dummy_func
HANDLE_EXP_FUNC(scan2, m_bool, 1)

#define scan2_stmt_func(name, type, prolog, exp) describe_stmt_func(scan2, name, type, prolog, exp)
scan2_stmt_func(flow, Stmt_Flow,, !(scan2_exp(env, stmt->cond) < 0 ||
    scan2_stmt(env, stmt->body) < 0) ? 1 : -1)
scan2_stmt_func(for, Stmt_For,, !(scan2_stmt(env, stmt->c1) < 0 ||
    scan2_stmt(env, stmt->c2) < 0 ||
    (stmt->c3 && scan2_exp(env, stmt->c3) < 0) ||
    scan2_stmt(env, stmt->body) < 0) ? 1 : -1)
scan2_stmt_func(auto, Stmt_Auto,, !(scan2_exp(env, stmt->exp) < 0 ||
    scan2_stmt(env, stmt->body) < 0) ? 1 : -1)
scan2_stmt_func(loop, Stmt_Loop,, !(scan2_exp(env, stmt->cond) < 0 ||
    scan2_stmt(env, stmt->body) < 0) ? 1 : -1)
scan2_stmt_func(switch, Stmt_Switch,, !(scan2_exp(env, stmt->val) < 0 ||
    scan2_stmt(env, stmt->stmt) < 0) ? 1 : -1)
scan2_stmt_func(if, Stmt_If,, !(scan2_exp(env, stmt->cond) < 0 ||
    scan2_stmt(env, stmt->if_body) < 0 ||
    (stmt->else_body && scan2_stmt(env, stmt->else_body) < 0)) ? 1 : -1)

ANN static inline m_bool scan2_stmt_code(const Env env, const Stmt_Code stmt) {
  if(stmt->stmt_list)
    { RET_NSPC(scan2_stmt_list(env, stmt->stmt_list)) }
  return GW_OK;
}

ANN static inline m_bool scan2_stmt_exp(const Env env, const Stmt_Exp stmt) {
  return stmt->val ? scan2_exp(env, stmt->val) : 1;
}

ANN static inline m_bool scan2_stmt_case(const Env env, const Stmt_Exp stmt) {
  return scan2_exp(env, stmt->val);
}

__attribute__((returns_nonnull))
ANN static Map scan2_label_map(const Env env) {
  Map m, label = env_label(env);
  const m_uint* key = env->class_def && !env->func ?
    (m_uint*)env->class_def : (m_uint*)env->func;
  if(!label->ptr)
    map_init(label);
  if(!(m = (Map)map_get(label, (vtype)key))) {
    m = new_map(env->gwion->mp);
    map_set(label, (vtype)key, (vtype)m);
  }
  return m;
}

ANN static m_bool scan2_stmt_jump(const Env env, const Stmt_Jump stmt) {
  if(stmt->is_label) {
    const Map m = scan2_label_map(env);
    if(map_get(m, (vtype)stmt->name))
      ERR_B(stmt_self(stmt)->pos, _("label '%s' already defined"), s_name(stmt->name))
    map_set(m, (vtype)stmt->name, (vtype)stmt);
    vector_init(&stmt->data.v);
  }
  return GW_OK;
}

ANN m_bool scan2_union_def(const Env env, const Union_Def udef) {
  if(tmpl_base(udef->tmpl))
    return GW_OK;
  const m_uint scope = union_push(env, udef);
  Decl_List l = udef->l;
  do CHECK_BB(scan2_exp(env, l->self))
  while((l = l->next));
  union_pop(env, udef, scope);
  return GW_OK;
}

static const _exp_func stmt_func[] = {
  (_exp_func)scan2_stmt_exp,  (_exp_func)scan2_stmt_flow, (_exp_func)scan2_stmt_flow,
  (_exp_func)scan2_stmt_for,  (_exp_func)scan2_stmt_auto, (_exp_func)scan2_stmt_loop,
  (_exp_func)scan2_stmt_if,   (_exp_func)scan2_stmt_code, (_exp_func)scan2_stmt_switch,
  (_exp_func)dummy_func,      (_exp_func)dummy_func,      (_exp_func)scan2_stmt_exp,
  (_exp_func)scan2_stmt_case, (_exp_func)scan2_stmt_jump,
  (_exp_func)scan2_stmt_fptr, (_exp_func)scan2_stmt_type,
};

ANN static m_bool scan2_stmt(const Env env, const Stmt stmt) {
  return stmt_func[stmt->stmt_type](env, &stmt->d);
}

ANN static m_bool scan2_stmt_list(const Env env, Stmt_List list) {
  do CHECK_BB(scan2_stmt(env, list->stmt))
  while((list = list->next));
  return GW_OK;
}

ANN static m_bool scan2_func_def_overload(const Env env, const Func_Def f, const Value overload) {
  const m_bool base = tmpl_base(f->base->tmpl);
  const m_bool tmpl = GET_FLAG(overload, template);
  if(isa(overload->type, t_function) < 0 || isa(overload->type, t_fptr) > 0) {
  if(isa(actual_type(overload->type), t_function) < 0)
    ERR_B(f->pos, _("function name '%s' is already used by another value"), overload->name)
}
  if((!tmpl && base) || (tmpl && !base && !GET_FLAG(f, template)))
    ERR_B(f->pos, _("must overload template function with template"))
  return GW_OK;
}

ANN static Func scan_new_func(const Env env, const Func_Def f, const m_str name) {
  const Func func = new_func(env->gwion->mp, name, f);
  if(env->class_def) {
    if(GET_FLAG(env->class_def, template))
      SET_FLAG(func, ref);
    if(!GET_FLAG(f, static))
      SET_FLAG(func, member);
  }
  return func;
}

ANN static Type func_type(const Env env, const Func func) {
  const Type t = type_copy(env->gwion->mp, func->def->base->td ? t_function : t_lambda);
  t->name = func->name;
  t->e->owner = env->curr;
  if(GET_FLAG(func, member))
    t->size += SZ_INT;
  t->e->d.func = func;
  return t;
}

ANN2(1,2) static Value func_value(const Env env, const Func f,
    const Value overload) {
  const Type  t = func_type(env, f);
  const Value v = new_value(env->gwion->mp, t, t->name);
  CHECK_OO(scan2_func_assign(env, f->def, f, v))
  if(!overload) {
    ADD_REF(v);
    nspc_add_value(env->curr, f->def->base->xid, v);
  } else {
    if(overload->d.func_ref) {
      f->next = overload->d.func_ref->next;
      overload->d.func_ref->next = f;
    } else
      overload->d.func_ref = f;
  }
  return v;
}

ANN2(1, 2) static m_bool scan2_func_def_template(const Env env, const Func_Def f, const Value overload) {
  const m_str name = s_name(f->base->xid);
  const Func func = scan_new_func(env, f, name);
  const Value value = func_value(env, func, overload);
  SET_FLAG(value, checked | ae_flag_template);
  SET_FLAG(value->type, func); // the only types with func flag, name could be better
  Type type = env->class_def;
  Nspc nspc = env->curr;
  uint i = 0;
  do {
    const Value v = nspc_lookup_value1(nspc, f->base->xid);
    if(v) {
      Func ff = v->d.func_ref;
      do {
        if(ff->def == f) {
          ++i;
          continue;
        }
        m_bool ret = compat_func(ff->def, f);
        if(ret > 0) {
          const Symbol sym = func_symbol(env, env->curr->name, name,
            "template", ff->vt_index);
          nspc_add_value(env->curr, sym, value);
          if(!overload) {
            ADD_REF(value)
            nspc_add_value(env->curr, f->base->xid, value);
          }
          func->vt_index = ff->vt_index;
          return GW_OK;
        }
      } while((ff = ff->next) && ++i);
   }
  } while(type && (type = type->e->parent) && (nspc = type->nspc));
  --i;
  const Symbol sym = func_symbol(env, env->curr->name, name, "template", i);
  nspc_add_value(env->curr, sym, value);
  if(!overload) {
    func->vt_index = i;
    ADD_REF(value)
    nspc_add_value(env->curr, f->base->xid, value);
    nspc_add_func(env->curr, f->base->xid, func);
  } else
    func->vt_index = ++overload->offset;
  return GW_OK;
}

ANN static m_bool scan2_func_def_builtin(MemPool p, const Func func, const m_str name) {
  SET_FLAG(func, builtin);
  func->code = new_vm_code(p, NULL, func->def->stack_depth, func->flag, name);
  func->code->native_func = (m_uint)func->def->d.dl_func_ptr;
  return GW_OK;
}

ANN static m_bool scan2_func_def_op(const Env env, const Func_Def f) {
  assert(f->base->args);
  const Type l = GET_FLAG(f, unary) ? NULL :
    f->base->args->var_decl->value->type;
  const Type r = GET_FLAG(f, unary) ? f->base->args->var_decl->value->type :
    f->base->args->next ? f->base->args->next->var_decl->value->type : NULL;
  struct Op_Import opi = { .op=f->base->xid, .lhs=l, .rhs=r, .ret=f->base->ret_type, .pos=f->pos };
  CHECK_BB(add_op(env->gwion, &opi))
  return GW_OK;
}

ANN static m_bool scan2_func_def_code(const Env env, const Func_Def f) {
  const Func former = env->func;
  env->func = f->base->func;
  CHECK_BB(scan2_stmt_code(env, &f->d.code->d.stmt_code))
  env->func = former;
  return GW_OK;
}

ANN static void scan2_func_def_flag(const Env env, const Func_Def f) {
  if(!GET_FLAG(f, builtin))
    SET_FLAG(f->base->func, pure);
  if(GET_FLAG(f, dtor)) {
    SET_FLAG(env->class_def, dtor);
    SET_FLAG(f->base->func, dtor);
  }
}

ANN static m_str func_tmpl_name(const Env env, const Func_Def f) {
  const m_str name = s_name(f->base->xid);
  struct Vector_ v;
  ID_List id = f->base->tmpl->list;
  m_uint tlen = 0;
  vector_init(&v);
  do {
    const Type t = nspc_lookup_type0(env->curr, id->xid);
    if(!t) {
      vector_release(&v);
      return NULL;
    }
    vector_add(&v, (vtype)t);
    tlen += strlen(t->name);
  } while((id = id->next) && ++tlen);
  char tmpl_name[tlen + 2];
  m_str str = tmpl_name;
  for(m_uint i = 0; i < vector_size(&v); ++i) {
    const m_str s = ((Type)vector_at(&v, i))->name;
    strcpy(str, s);
    str += strlen(s);
    if(i + 1 < vector_size(&v))
      *str++ = ',';
  }
  tmpl_name[tlen+1] = '\0';
  vector_release(&v);
  const Symbol sym = func_symbol(env, env->curr->name, name, tmpl_name, (m_uint)f->base->tmpl->base);
  return s_name(sym);
}


ANN2(1,2,4) /*static */Value func_create(const Env env, const Func_Def f,
     const Value overload, const m_str name) {
  const Func func = scan_new_func(env, f, name);
  nspc_add_func(env->curr, insert_symbol(func->name), func);
  const Value v = func_value(env, func, overload);
  scan2_func_def_flag(env, f);
  if(GET_FLAG(f, builtin))
    CHECK_BO(scan2_func_def_builtin(env->gwion->mp, func, func->name))
  nspc_add_value(env->curr, insert_symbol(func->name), v);
  return v;
}


ANN static m_str template_helper(const Env env, const Func_Def f) {
  const m_str name = f->base->func ?  f->base->func->name : func_tmpl_name(env, f);
  if(!name)
    return(m_str)GW_ERROR;
  const Func func = nspc_lookup_func1(env->curr, insert_symbol(name));
  if(func) {
    f->base->ret_type = known_type(env, f->base->td);
    return (m_str)(m_uint)((f->base->args && f->base->args->type) ? scan2_args(env, f) : GW_OK);
  }
  return name;
}

ANN2(1,2) static m_str func_name(const Env env, const Func_Def f, const Value v) {
  if(!f->base->tmpl) {
    const Symbol sym  = func_symbol(env, env->curr->name, s_name(f->base->xid), NULL, v ? ++v->offset : 0);
    return s_name(sym);
  }
  return template_helper(env, f);
}


ANN2(1,2) m_bool scan2_fdef(const Env env, const Func_Def f, const Value overload) {
  const m_str name = func_name(env, f, overload ?: NULL);
  if((m_int)name <= GW_OK)
    return (m_bool)(m_uint)name;
  const Func base = get_func(env, f);
  if(!base)
    CHECK_OB(func_create(env, f, overload, name))
  else
    f->base->func = base;
  if(f->base->args)
    CHECK_BB(scan2_args(env, f))
  if(!GET_FLAG(f, builtin) && f->d.code)
    CHECK_BB(scan2_func_def_code(env, f))
  if(!base) {
    if(GET_FLAG(f, op))
      CHECK_BB(scan2_func_def_op(env, f))
    SET_FLAG(f->base->func->value_ref, checked);
  }
  return GW_OK;
}

ANN m_bool scan2_func_def(const Env env, const Func_Def f) {
  const m_uint scope = !GET_FLAG(f, global) ? env->scope->depth : env_push_global(env);
  const Value overload = nspc_lookup_value0(env->curr, f->base->xid);
//  const Value res = nspc_lookup_value1(env->global_nspc, f->base->xid);
//  if(res)
//    ERR_B(f->pos, _("'%s' already declared as type"), s_name(f->base->xid))
  f->stack_depth = (env->class_def && !GET_FLAG(f, static) && !GET_FLAG(f, global)) ? SZ_INT : 0;
  if(GET_FLAG(f, variadic))
    f->stack_depth += SZ_INT;
  if(overload)
    CHECK_BB(scan2_func_def_overload(env, f, overload))
  if(f->base->tmpl)
    CHECK_BB(template_push_types(env, f->base->tmpl))
  const m_bool ret = (!tmpl_base(f->base->tmpl) ? scan2_fdef : scan2_func_def_template)(env, f, overload);
  if(f->base->tmpl)
    nspc_pop_type(env->gwion->mp, env->curr);
  if(GET_FLAG(f, global))
    env_pop(env, scope);
  return ret;
}

#define scan2_enum_def dummy_func
DECL_SECTION_FUNC(scan2)

ANN static m_bool scan2_class_parent(const Env env, const Class_Def cdef) {
  const Type parent = cdef->base.type->e->parent;
  if(parent->e->def && (!GET_FLAG(parent, scan2) || GET_FLAG(parent, template)))
    CHECK_BB(scanx_parent(parent, scan2_cdef, env))
  if(cdef->base.ext->array)
    CHECK_BB(scan2_exp(env, cdef->base.ext->array->exp))
  return GW_OK;
}

ANN m_bool scan2_class_def(const Env env, const Class_Def cdef) {
  if(tmpl_base(cdef->base.tmpl))
    return GW_OK;
  SET_FLAG(cdef->base.type, scan2);
  if(cdef->base.ext)
    CHECK_BB(env_ext(env, cdef, scan2_class_parent))
  if(cdef->body)
    CHECK_BB(env_body(env, cdef, scan2_section))
  return GW_OK;
}

ANN m_bool scan2_ast(const Env env, Ast ast) {
  do CHECK_BB(scan2_section(env, ast->section))
  while((ast = ast->next));
  return GW_OK;
}
