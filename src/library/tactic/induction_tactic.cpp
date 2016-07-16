/*
Copyright (c) 2016 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#include "util/list_fn.h"
#include "kernel/instantiate.h"
#include "library/trace.h"
#include "library/user_recursors.h"
#include "library/locals.h"
#include "library/app_builder.h"
#include "library/vm/vm_expr.h"
#include "library/vm/vm_name.h"
#include "library/vm/vm_list.h"
#include "library/tactic/tactic_state.h"
#include "library/tactic/revert_tactic.h"
#include "library/tactic/intro_tactic.h"
#include "library/tactic/clear_tactic.h"
#include "library/tactic/induction_tactic.h"

namespace lean {
[[ noreturn ]] static void throw_ill_formed_recursor_exception(recursor_info const & rec_info) {
    throw exception(sstream() << "induction tactic failed, recursor '" << rec_info.get_name() << "' is ill-formed");
}

static void set_intron(expr & R, type_context & ctx, expr const & M, unsigned n, list<name> & ns, buffer<name> & new_names) {
    if (n == 0) {
        R = M;
    } else {
        metavar_context mctx = ctx.mctx();
        if (auto M1 = intron(ctx.env(), ctx.get_options(), mctx, M, n, ns, new_names)) {
            R = *M1;
            ctx.set_mctx(mctx);
        } else {
            throw exception("induction tactic failed, failed to create new goal");
        }
    }
}

static void set_intron(expr & R, type_context & ctx, expr const & M, unsigned n, buffer<name> & new_names) {
    list<name> tmp;
    set_intron(R, ctx, M, n, tmp, new_names);
}

static void set_clear(expr & R, type_context & ctx, expr const & M, expr const & H) {
    metavar_context mctx = ctx.mctx();
    R = clear(mctx, M, H);
    ctx.set_mctx(mctx);
}

/* Helper function for computing the number of nested Pi-expressions.
   It uses head_beta_reduce on intermediate terms. */
static unsigned get_expr_arity(expr type) {
    unsigned r = 0;
    type = head_beta_reduce(type);
    while (is_pi(type)) {
        type = head_beta_reduce(binding_body(type));
        r++;
    }
    return r;
}

list<expr> induction(environment const & env, options const & opts, transparency_mode const & m, metavar_context & mctx,
                     expr const & mvar, expr const & H, name const & rec_name, list<name> & ns,
                     intros_list * ilist, renaming_list * rlist) {
    lean_assert(is_metavar(mvar));
    lean_assert(is_local(H));
    optional<metavar_decl> g = mctx.get_metavar_decl(mvar);
    lean_assert(g);
    type_context ctx1 = mk_type_context_for(env, opts, mctx, g->get_context(), m);
    expr H_type = ctx1.infer(H);
    recursor_info rec_info = get_recursor_info(env, rec_name);
    buffer<expr> H_type_args;
    get_app_args(H_type, H_type_args);
    for (optional<unsigned> const & pos : rec_info.get_params_pos()) {
        if (*pos >= H_type_args.size()) {
            throw exception("induction tactic failed, major premise type is ill-formed");
        }
    }
    buffer<expr> indices;
    list<unsigned> const & idx_pos = rec_info.get_indices_pos();
    for (unsigned pos : idx_pos) {
        if (pos >= H_type_args.size()) {
            throw exception("induction tactic failed, major premise type is ill-formed");
        }
        expr const & idx = H_type_args[pos];
        if (!is_local(idx)) {
            throw exception(sstream() << "induction tactic failed, argument #"
                            << pos+1 << " of major premise '" << H << "' type is not a variable");
        }
        for (unsigned i = 0; i < H_type_args.size(); i++) {
            if (i != pos && is_local(H_type_args[i]) && mlocal_name(H_type_args[i]) == mlocal_name(idx)) {
                throw exception(sstream() << "induction tactic failed, argument #"
                                << pos+1 << " of major premise '" << H << "' type is an index, "
                                << "but it occurs more than once");
            }
            if (i < pos && depends_on(H_type_args[i], idx)) {
                throw exception(sstream() << "induction tactic failed, argument #"
                                << pos+1 << " of major premise '" << H << "' type is an index, "
                                << "but it occurs in previous arguments");
            }
            if (i > pos && // occurs after idx
                std::find(idx_pos.begin(), idx_pos.end(), i) != idx_pos.end() && // it is also an index
                is_local(H_type_args[i]) && // if it is not an index, it will fail anyway.
                depends_on(mlocal_type(idx), H_type_args[i])) {
                throw exception(sstream() << "induction tactic failed, argument #"
                                << pos+1 << " of major premise '" << H << "' type is an index, "
                                << "but its type depends on the index at position #" << i+1);
            }
        }
        indices.push_back(idx);
    }
    if (!rec_info.has_dep_elim() && depends_on(g->get_type(), H)) {
        throw exception(sstream() << "induction tactic failed, recursor '" << rec_name
                        << "' does not support dependent elimination, but conclusion "
                        << "depends on major premise '" << H << "'");
    }
    /* Revert indices and major premise */
    buffer<expr> to_revert;
    to_revert.append(indices);
    to_revert.push_back(H);
    expr mvar1 = revert(env, opts, mctx, mvar, to_revert);
    lean_assert(to_revert.size() >= indices.size() + 1);
    /* Re-introduce indices and major. */
    buffer<name> indices_H;
    optional<expr> mvar2 = intron(env, opts, mctx, mvar1, indices.size() + 1, indices_H);
    if (!mvar2)
        throw exception("induction tactic failed, failed to reintroduce major premise");
    name_map<name> base_renames; /* renames for all branches */
    if (rlist) {
        /* store old index name -> new index name */
        for (unsigned i = 0; i < indices.size(); i++) {
            base_renames.insert(mlocal_name(indices[i]), indices_H[i]);
        }
    }
    optional<metavar_decl> g2 = mctx.get_metavar_decl(*mvar2);
    lean_assert(g2);
    type_context ctx2   = mk_type_context_for(env, opts, mctx, g2->get_context(), m);
    local_context lctx2 = g2->get_context();
    indices.clear(); /* update indices buffer */
    for (unsigned i = 0; i < indices_H.size() - 1; i++)
        indices.push_back(lctx2.get_local_decl(indices_H[i])->mk_ref());
    level g_lvl         = get_level(ctx2, g2->get_type());
    local_decl H2_decl  = *lctx2.get_last_local_decl();
    expr H2             = H2_decl.mk_ref();
    expr H2_type        = H2_decl.get_type();
    buffer<expr> H2_type_args;
    expr const & I     = get_app_args(H2_type, H2_type_args);
    if (!is_constant(I))
        throw exception("induction tactic failed, major premise is not of the form (C ...)");
    /* Compute recursor universe levels */
    buffer<level> I_lvls;
    to_buffer(const_levels(I), I_lvls);
    bool found_g_lvl = false;
    buffer<level> rec_lvls;
    for (unsigned idx : rec_info.get_universe_pos()) {
        if (idx == recursor_info::get_motive_univ_idx()) {
            rec_lvls.push_back(g_lvl);
            found_g_lvl = true;
        } else {
            if (idx >= I_lvls.size())
                throw_ill_formed_recursor_exception(rec_info);
            rec_lvls.push_back(I_lvls[idx]);
        }
    }
    if (!found_g_lvl && !is_zero(g_lvl)) {
        throw exception(sstream() << "induction tactic failed, recursor '" << rec_info.get_name()
                        << "' can only eliminate into Prop");
    }
    expr rec = mk_constant(rec_info.get_name(), to_list(rec_lvls));
    /* Compute recursor parameters */
    unsigned nparams = 0;
    for (optional<unsigned> const & pos : rec_info.get_params_pos()) {
        if (pos) {
            /* See test at induction_tactic_core */
            lean_assert(*pos < H2_type_args.size());
            rec = mk_app(rec, H2_type_args[*pos]);
        } else {
            expr rec_type = ctx2.relaxed_whnf(ctx2.infer(rec));
            if (!is_pi(rec_type))
                throw_ill_formed_recursor_exception(rec_info);
            optional<expr> inst = ctx2.mk_class_instance(binding_domain(rec_type));
            if (!inst)
                throw exception(sstream() << "induction tactic failed, failed to generate "
                                "type class instance parameter");
            rec = mk_app(rec, *inst);
        }
        nparams++;
    }
    /* Save initial arity */
    unsigned initial_arity = get_expr_arity(g2->get_type());
    /* Compute motive */
    expr motive = g2->get_type();
    if (rec_info.has_dep_elim())
        motive  = ctx2.mk_lambda(H2, motive);
    motive   = ctx2.mk_lambda(indices, motive);
    rec      = mk_app(rec, motive);
    /* Process remaining arguments and create new goals */
    unsigned curr_pos      = nparams + 1 /* motive */;
    expr rec_type          = ctx2.relaxed_try_to_pi(ctx2.infer(rec));
    unsigned first_idx_pos = rec_info.get_first_index_pos();
    bool consumed_major    = false;
    buffer<expr> new_goals;
    buffer<list<name>>      param_names_buffer;
    buffer<name_map<name>>  rename_buffer;
    list<bool> produce_motive = rec_info.get_produce_motive();
    while (is_pi(rec_type) && curr_pos < rec_info.get_num_args()) {
        if (first_idx_pos == curr_pos) {
            for (expr const & idx : indices) {
                rec      = mk_app(rec, idx);
                rec_type = ctx2.relaxed_whnf(instantiate(binding_body(rec_type), idx));
                if (!is_pi(rec_type)) {
                    throw_ill_formed_recursor_exception(rec_info);
                }
                curr_pos++;
            }
            rec      = mk_app(rec, H2);
            rec_type = ctx2.relaxed_try_to_pi(instantiate(binding_body(rec_type), H2));
            consumed_major = true;
            curr_pos++;
        } else {
            if (!produce_motive)
                throw_ill_formed_recursor_exception(rec_info);
            expr new_type  = head_beta_reduce(binding_domain(rec_type));
            expr rec_arg;
            if (binding_info(rec_type).is_inst_implicit()) {
                if (optional<expr> inst = ctx2.mk_class_instance(new_type)) {
                    rec_arg = *inst;
                } else {
                    /* Add new goal if type class inference failed */
                    expr new_goal = ctx2.mk_metavar_decl(lctx2, new_type);
                    new_goals.push_back(new_goal);
                    rec_arg = new_goal;
                }
            } else {
                unsigned arity = get_expr_arity(new_type);
                lean_assert(arity >= initial_arity);
                unsigned nparams = arity - initial_arity; /* number of hypotheses due to minor premise */
                unsigned nextra  = to_revert.size() - indices.size() - 1; /* extra dependencies that have been reverted */
                expr new_M       = ctx2.mk_metavar_decl(ctx2.lctx(), head_beta_reduce(new_type));
                expr aux_M;
                buffer<name> param_names; buffer<name> extra_names;
                /* Introduce constructor parameter for new goal associated with minor premise. */
                set_intron(aux_M, ctx2, new_M, nparams, ns, param_names);
                /* Introduce hypothesis that had to be reverted because they depended on indices and/or major premise. */
                set_intron(aux_M, ctx2, aux_M, nextra, extra_names);
                if (ilist) {
                    /* Save name of constructor parameters that have been introduced for new goal. */
                    param_names_buffer.push_back(to_list(param_names));
                }
                if (rlist) {
                    /* Save renaming for hypothesis that depend on indices and/or major premise. */
                    name_map<name> rmap = base_renames;
                    lean_assert(extra_names.size() == nextra);
                    for (unsigned i = indices.size() + 1, j = 0; i < to_revert.size(); i++, j++) {
                        rmap.insert(mlocal_name(to_revert[i]), extra_names[j]);
                    }
                    rename_buffer.push_back(rmap);
                }
                /* Clear hypothesis in the new goal. */
                set_clear(aux_M, ctx2, aux_M, H2);
                new_goals.push_back(aux_M);
                rec_arg = new_M;
            }
            produce_motive = tail(produce_motive);
            rec            = mk_app(rec, rec_arg);
            rec_type       = ctx2.relaxed_try_to_pi(instantiate(binding_body(rec_type), rec_arg));
            curr_pos++;
        }
    }
    if (!consumed_major) {
        throw_ill_formed_recursor_exception(rec_info);
    }
    mctx = ctx2.mctx();
    mctx.assign(*mvar2, rec);
    if (ilist) *ilist = to_list(param_names_buffer);
    if (rlist) *rlist = to_list(rename_buffer);
    return to_list(new_goals);
}

vm_obj induction_tactic_core(transparency_mode const & m, expr const & H, name const & rec_name, list<name> const & ns,
                             tactic_state const & s) {
    if (!s.goals()) return mk_no_goals_exception(s);
    if (!is_local(H)) return mk_tactic_exception("induction tactic failed, argument is not a hypothesis", s);
    try {
        metavar_context mctx = s.mctx();
        list<name> tmp_ns = ns;
        list<expr> new_goals = induction(s.env(), s.get_options(), m, mctx, head(s.goals()), H, rec_name, tmp_ns,
                                         nullptr, nullptr);
        tactic_state new_s   = set_mctx_goals(s, mctx, append(new_goals, tail(s.goals())));
        return mk_tactic_success(new_s);
    } catch (exception & ex) {
        return mk_tactic_exception(ex, s);
    }
}

vm_obj tactic_induction_core(vm_obj const & m, vm_obj const & H, vm_obj const & rec, vm_obj const & ns, vm_obj const & s) {
    return induction_tactic_core(to_transparency_mode(m), to_expr(H), to_name(rec), to_list_name(ns), to_tactic_state(s));
}

void initialize_induction_tactic() {
    DECLARE_VM_BUILTIN(name({"tactic", "induction_core"}), tactic_induction_core);
}

void finalize_induction_tactic() {
}
}