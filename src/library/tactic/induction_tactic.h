/*
Copyright (c) 2016 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#pragma once
#include "library/tactic/tactic_state.h"

namespace lean {
typedef list<list<name>>     intros_list;
typedef list<name_map<name>> renaming_list;

/** \brief Apply the induction lemma \c rec_name on the hypothesis \c H at goal \c mvar.
    The tactic uses ns (if available) to name parameters associated with minor premises.

    If ilist != nullptr, then tactic stores the (internal) names for parameters introduced.
    If rlist != nullptr, then tactic stores renamed hypotheses for each new goal.

    The result is a list of goals (new metavariables).

    \pre is_metavar(mvar)
    \pre is_local(H)
    \pre mctx.get_metavar_decl(mvar) != none

    \post ilist != nullptr ==> length(*ilist) == length(result)
    \post rlist != nullptr ==> length(*rlist) == length(result) */
list<expr> induction(environment const & env, options const & opts, transparency_mode const & m, metavar_context & mctx,
                     expr const & mvar, expr const & H, name const & rec_name, list<name> & ns,
                     intros_list * ilist, renaming_list * rlist);

void initialize_induction_tactic();
void finalize_induction_tactic();
}