#include <stdlib.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include "internal.h"
#include "history.h"
#include "parser.h"
#include "module.h"
#include "prolog.h"
#include "query.h"
#include "builtins.h"
#include "heap.h"
#include "utf8.h"

static bool is_cyclic_term_internal(query *q, cell *p1, pl_idx_t p1_ctx, reflist *list)
{
	if (!is_structure(p1))
		return false;

	pl_idx_t nbr_cells = p1->nbr_cells - 1;
	p1++;

	while (nbr_cells) {
		if (is_variable(p1)) {
			if (is_in_ref_list(p1, p1_ctx, list))
				return q->cycle_error = true;

			reflist nlist;
			nlist.next = list;
			nlist.var_nbr = p1->var_nbr;
			nlist.ctx = p1_ctx;

			cell *c = deref(q, p1, p1_ctx);
			pl_idx_t c_ctx = q->latest_ctx;

			if (is_cyclic_term_internal(q, c, c_ctx, &nlist)) {
				return true;
			}
		}

		nbr_cells--;
		p1++;
	}

	return false;
}

bool is_cyclic_term(query *q, cell *p1, pl_idx_t p1_ctx)
{
	q->cycle_error = false;
	return is_cyclic_term_internal(q, p1, p1_ctx, NULL);
}

static cell *term_next(query *q, cell *c, pl_idx_t *c_ctx, bool *done)
{
	if (!is_list(c)) {
		*done = true;
		return c;
	}

	LIST_HANDLER(c);
	LIST_HEAD(c);
	c = LIST_TAIL(c);
	c = deref(q, c, *c_ctx);
	*c_ctx = q->latest_ctx;
	return c;
}

// This uses Brent's method...

cell* detect_cycle(query *q, cell *head, pl_idx_t *head_ctx, pl_int_t max, pl_int_t *skip, cell *tmp)
{
	if (is_string(head)) {
		const char *src = GET_STR(q, head);
		size_t len_src = LEN_STR(q, head);
		const char *save_src = src;

		while ((max-- > 0) && (len_src > 0)) {
			size_t len = len_char_utf8(src);
			len_src -= len;
			src += len;
			*skip += 1;
		}

		if (LEN_STR(q, head) == (size_t)(src-save_src)) {
			make_literal(tmp, g_nil_s);
		} else if (src == save_src) {
			tmp = head;
		} else {
			may_error(make_stringn(tmp, src, LEN_STR(q, head) - (src-save_src)));
		}

		return tmp;
	}

	if (!head)
		return NULL;

	if (!max) {
		*skip = max;
		return head;
	}

	cell *slow = head;
	pl_idx_t slow_ctx = *head_ctx, fast_ctx = *head_ctx;
	bool done = false;
	cell *fast = term_next(q, head, &fast_ctx, &done);
	pl_int_t length = 1, cnt = 0;

#define DO_BRENT 1		// If 0 it's basically Floyd

#if DO_BRENT
	int power = 1;
#endif

	while (!g_tpl_interrupt && !done) {
		if ((fast == slow) && (fast_ctx == slow_ctx))
			break;

#if DO_BRENT
		if (length == power) {
			power *= 2;
			length = 0;
			slow = fast;
			slow_ctx = fast_ctx;
		}
#endif

		if (max == ++cnt) {
			*skip = cnt;
			*head_ctx = fast_ctx;
			return fast;
		}

		fast = term_next(q, fast, &fast_ctx, &done);
		++length;
	}

	if (done) {
		*skip = cnt;
		*head_ctx = fast_ctx;
		return fast;
	}

	// length stores actual length of the loop.
	// Now set slow to the beginning
	// and fast to head+length i.e length of the cycle.

	slow = fast = head;
	fast_ctx = slow_ctx = *head_ctx;

	while (length > 0) {
		fast = term_next(q, fast, &fast_ctx, &done);
		--length;

		if (length == max)
			break;
	}

	pl_int_t len = 0;

	while (!g_tpl_interrupt) {
		if ((fast == slow) && (fast_ctx == slow_ctx))
			break;

		fast = term_next(q, fast, &fast_ctx, &done);
		slow = term_next(q, slow, &slow_ctx, &done);
		len++;
	}

	*skip = len;
	*head_ctx = slow_ctx;
	return slow;
}

static void make_var_ref(query *q, cell *tmp, unsigned var_nbr, pl_idx_t ctx)
{
	make_variable(tmp, g_anon_s, create_vars(q, 1));
	cell v;
	make_variable(&v, g_anon_s, var_nbr);
	set_var(q, tmp, q->st.curr_frame, &v, ctx);
}

static void make_cell_ref(query *q, cell *tmp, cell *v, pl_idx_t ctx)
{
	make_variable(tmp, g_anon_s, create_vars(q, 1));
	set_var(q, tmp, q->st.curr_frame, v, ctx);
}

pl_status fn_sys_undo_trail_1(query *q)
{
	GET_FIRST_ARG(p1,variable);
	q->save_e = malloc(sizeof(slot)*(q->undo_hi_tp - q->undo_lo_tp));
	may_ptr_error(q->save_e);
	bool first = true;

	// Unbind our vars

	for (pl_idx_t i = q->undo_lo_tp, j = 0; i < q->undo_hi_tp; i++, j++) {
		const trail *tr = q->trails + i;
		const frame *f = GET_FRAME(tr->ctx);
		slot *e = GET_SLOT(f, tr->var_nbr);
		//printf("*** unbind [%u:%u] hi_tp=%u, ctx=%u, var=%u\n", j, i, q->undo_hi_tp, tr->ctx, tr->var_nbr);
		q->save_e[j] = *e;

		cell lhs, rhs;
		make_var_ref(q, &lhs, tr->var_nbr, tr->ctx);
		make_cell_ref(q, &rhs, &e->c, e->ctx);

		cell tmp[3];
		make_structure(tmp, g_minus_s, NULL, 2, 2);
		SET_OP(&tmp[0], OP_YFX);
		tmp[1] = lhs;
		tmp[2] = rhs;

		if (first) {
			allocate_list(q, tmp);
			first = false;
		} else
			append_list(q, tmp);

		e->c.tag = TAG_EMPTY;
		e->c.attrs = tr->attrs;
		e->c.attrs_ctx = tr->attrs_ctx;
	}

	cell *tmp = end_list(q);
	may_ptr_error(tmp);
	set_var(q, p1, p1_ctx, tmp, q->st.curr_frame);
	return pl_success;
}

pl_status fn_sys_redo_trail_0(query * q)
{
	for (pl_idx_t i = q->undo_lo_tp, j = 0; i < q->undo_hi_tp; i++, j++) {
		const trail *tr = q->trails + i;
		const frame *f = GET_FRAME(tr->ctx);
		slot *e = GET_SLOT(f, tr->var_nbr);
		//printf("*** rebind [%u:%u] hi_tp=%u, ctx=%u, var=%u\n", j, i, q->undo_hi_tp, tr->ctx, tr->var_nbr);
		*e = q->save_e[j];
	}

	free(q->save_e);
	return pl_success;
}

pl_status fn_sys_end_hook_0(query * q)
{
	q->in_hook = false;
	return pl_success;
}

pl_status do_post_unification_hook(query *q)
{
	q->in_hook = true;
	q->has_attrs = false;
	q->undo_lo_tp = q->save_tp;
	q->undo_hi_tp = q->st.tp;
	cell *tmp = alloc_on_heap(q, 3);
	may_ptr_error(tmp);
	// Needed for follow() to work
	*tmp = (cell){0};
	tmp[0].tag = TAG_EMPTY;
	tmp[0].nbr_cells = 1;
	tmp[0].flags = FLAG_BUILTIN;

	tmp[1].tag = TAG_LITERAL;
	tmp[1].nbr_cells = 1;
	tmp[1].arity = 0;
	tmp[1].flags = 0;
	tmp[1].val_off = g_post_unify_hook_s;
	tmp[1].match = search_predicate(q->pl->user_m, tmp+1);

	if (!tmp[1].match)
		return throw_error(q, tmp+1, q->st.curr_frame, "existence_error", "procedure");

	make_call(q, tmp+2);
	q->st.curr_cell = tmp;
	return pl_success;
}

// TODO : change this to make a list of vars as we go...

static void collect_vars_internal(query *q, cell *p1, pl_idx_t p1_ctx, reflist *list)
{
	pl_idx_t nbr_cells = p1->nbr_cells;

	while (nbr_cells) {
		if (is_variable(p1)) {
			if (is_in_ref_list(p1, p1_ctx, list))
				return;

			reflist nlist;
			nlist.next = list;
			nlist.var_nbr = p1->var_nbr;
			nlist.ctx = p1_ctx;
			cell *c = deref(q, p1, p1_ctx);
			pl_idx_t c_ctx = q->latest_ctx;

			if (is_structure(c))
				collect_vars_internal(q, c, c_ctx, &nlist);
		}

		cell *c = deref(q, p1, p1_ctx);
		pl_idx_t c_ctx = q->latest_ctx;

		if (is_variable(c)) {
			bool found = false;

			for (unsigned idx = 0; idx < q->pl->tab_idx; idx++) {
				if ((q->pl->tab1[idx] == c_ctx) && (q->pl->tab2[idx] == c->var_nbr)) {
					q->pl->tab4[idx]++;
					found = true;
					break;
				}
			}

			if (!found) {
				q->pl->tab1[q->pl->tab_idx] = c_ctx;
				q->pl->tab2[q->pl->tab_idx] = c->var_nbr;
				q->pl->tab3[q->pl->tab_idx] = c->val_off;
				q->pl->tab4[q->pl->tab_idx] = 1;
				q->pl->tab5[q->pl->tab_idx] = is_anon(c) ? 1 : 0;
				q->pl->tab_idx++;
			}
		}

		nbr_cells--;
		p1++;
	}
}

bool collect_vars(query *q, cell *p1, pl_idx_t p1_ctx)
{
	collect_vars_internal(q, p1, p1_ctx, NULL);
	return true;
}

static bool unify_structs(query *q, cell *p1, pl_idx_t p1_ctx, cell *p2, pl_idx_t p2_ctx, unsigned depth)
{
	if (!depth)
		q->cycle_error = false;

	if (depth >= MAX_DEPTH) {
		q->cycle_error = true;
		return true;
	}

	if (p1->arity != p2->arity)
		return false;

	if (p1->val_off != p2->val_off)
		return false;

	unsigned arity = p1->arity;
	p1++; p2++;

	while (arity-- && !g_tpl_interrupt) {
		cell *c1 = deref(q, p1, p1_ctx);
		pl_idx_t c1_ctx = q->latest_ctx;
		cell *c2 = deref(q, p2, p2_ctx);
		pl_idx_t c2_ctx = q->latest_ctx;
		reflist r1 = {0}, r2 = {0};

		if (q->info1) {
			if (is_variable(p1)) {
				if (is_in_ref_list(p1, p1_ctx, q->info1->r1)) {
					c1 = p1;
					c1_ctx = p1_ctx;
				} else {
					r1.next = q->info1->r1;
					r1.var_nbr = p1->var_nbr;
					r1.ctx = p1_ctx;
					q->info1->r1 = &r1;
				}
			}
		}

		if (q->info2) {
			if (is_variable(p2)) {
				if (is_in_ref_list(p2, p2_ctx, q->info2->r2)) {
					c2 = p2;
					c2_ctx = p2_ctx;
				} else {
					r2.next = q->info2->r2;
					r2.var_nbr = p2->var_nbr;
					r2.ctx = p2_ctx;
					q->info2->r2 = &r2;
				}
			}
		}

		if (!unify_internal(q, c1, c1_ctx, c2, c2_ctx, depth+1))
			return false;

		if (q->cycle_error)
			return true;

		if (q->info1) {
			if (is_variable(p1))
				q->info1->r1 = r1.next;		// restore
		}

		if (q->info2) {
			if (is_variable(p2))
				q->info2->r2 = r2.next;		// restore
		}

		p1 += p1->nbr_cells;
		p2 += p2->nbr_cells;
	}

	return true;
}

// This is for when one arg is a string & the other an iso-list...

static bool unify_string_to_list(query *q, cell *p1, pl_idx_t p1_ctx, cell *p2, pl_idx_t p2_ctx, unsigned depth)
{
	if (p1->arity != p2->arity)
		return false;

	unsigned save_depth = depth;
	LIST_HANDLER(p1);
	LIST_HANDLER(p2);

	while (is_list(p1) && is_list(p2) && !g_tpl_interrupt) {
		if (depth >= MAX_DEPTH) {
			q->cycle_error = true;
			return true;
		}

		cell *c1 = LIST_HEAD(p1);
		cell *c2 = LIST_HEAD(p2);

		c1 = deref(q, c1, p1_ctx);
		pl_idx_t c1_ctx = q->latest_ctx;
		c2 = deref(q, c2, p2_ctx);
		pl_idx_t c2_ctx = q->latest_ctx;

		if (!unify_internal(q, c1, c1_ctx, c2, c2_ctx, depth+1)) {
			if (q->cycle_error)
				return true;

			return false;
		}

		if (q->cycle_error)
			return true;

		c1 = LIST_TAIL(p1);
		c2 = LIST_TAIL(p2);

		p1 = deref(q, c1, p1_ctx);
		p1_ctx = q->latest_ctx;
		p2 = deref(q, c2, p2_ctx);
		p2_ctx = q->latest_ctx;
		depth++;
	}

	return unify_internal(q, p1, p1_ctx, p2, p2_ctx, save_depth+1);
}

static bool unify_integers(__attribute__((unused)) query *q, cell *p1, cell *p2)
{
	if (is_bigint(p1) && is_bigint(p2))
		return !mp_int_compare(&p1->val_bigint->ival, &p2->val_bigint->ival);

	if (is_bigint(p1) && is_integer(p2))
		return !mp_int_compare_value(&p1->val_bigint->ival, p2->val_int);

	if (is_bigint(p2) && is_integer(p1))
		return !mp_int_compare_value(&p2->val_bigint->ival, p1->val_int);

	if (is_integer(p2))
		return (get_int(p1) == get_int(p2));

	return false;
}

static bool unify_reals(__attribute__((unused)) query *q, cell *p1, cell *p2)
{
	if (is_real(p2))
		return get_real(p1) == get_real(p2);

	return false;
}

static bool unify_literals(query *q, cell *p1, cell *p2)
{
	if (is_literal(p2))
		return p1->val_off == p2->val_off;

	if (is_cstring(p2) && (LEN_STR(q, p1) == LEN_STR(q, p2)))
		return !memcmp(GET_STR(q, p2), GET_POOL(q, p1->val_off), LEN_STR(q, p1));

	return false;
}

static bool unify_cstrings(query *q, cell *p1, cell *p2)
{
	if (is_cstring(p2) && (LEN_STR(q, p1) == LEN_STR(q, p2)))
		return !memcmp(GET_STR(q, p1), GET_STR(q, p2), LEN_STR(q, p1));

	if (is_literal(p2) && (LEN_STR(q, p1) == LEN_STR(q, p2)))
		return !memcmp(GET_STR(q, p1), GET_POOL(q, p2->val_off), LEN_STR(q, p1));

	return false;
}

struct dispatch {
	uint8_t tag;
	bool (*fn)(query*, cell*, cell*);
};

static const struct dispatch g_disp[] =
{
	{TAG_EMPTY, NULL},
	{TAG_VAR, NULL},
	{TAG_LITERAL, unify_literals},
	{TAG_CSTR, unify_cstrings},
	{TAG_INT, unify_integers},
	{TAG_REAL, unify_reals},
	{0}
};

bool unify_internal(query *q, cell *p1, pl_idx_t p1_ctx, cell *p2, pl_idx_t p2_ctx, unsigned depth)
{
	if (depth >= MAX_DEPTH) {
		q->cycle_error = true;
		return true;
	}

	if (p1_ctx == q->st.curr_frame)
		q->no_tco = true;

	if (is_variable(p1) && !is_variable(p2))
		q->has_vars = true;

	if (is_variable(p1) && is_variable(p2)) {
		if (p2_ctx > p1_ctx)
			set_var(q, p2, p2_ctx, p1, p1_ctx);
		else if (p2_ctx < p1_ctx)
			set_var(q, p1, p1_ctx, p2, p2_ctx);
		else if (p2->var_nbr != p1->var_nbr)
			set_var(q, p2, p2_ctx, p1, p1_ctx);

		return true;
	}

	if (is_variable(p1)) {
		set_var(q, p1, p1_ctx, p2, p2_ctx);
		return true;
	}

	if (is_variable(p2)) {
		set_var(q, p2, p2_ctx, p1, p1_ctx);
		return true;
	}

	q->check_unique = true;

	if (is_string(p1) && is_string(p2))
		return unify_cstrings(q, p1, p2);

	if (is_string(p1) && is_list(p2))
		return unify_string_to_list(q, p1, p1_ctx, p2, p2_ctx, depth+1);

	if (is_string(p2) && is_list(p1))
		return unify_string_to_list(q, p2, p2_ctx, p1, p1_ctx, depth+1);

	if (p1->arity || p2->arity)
		return unify_structs(q, p1, p1_ctx, p2, p2_ctx, depth+1);

	return g_disp[p1->tag].fn(q, p1, p2);
}
