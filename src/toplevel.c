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
#include "heap.h"
#include "utf8.h"

#ifdef _WIN32
#include <windows.h>
#define msleep Sleep
#else
static void msleep(int ms)
{
	struct timespec tv;
	tv.tv_sec = (ms) / 1000;
	tv.tv_nsec = ((ms) % 1000) * 1000 * 1000;
	nanosleep(&tv, &tv);
}
#endif

typedef struct item_ item;

struct item_ {
	cell *c;
	pl_idx_t c_ctx;
	int nbr;
	item *next;
};

static item *g_items = NULL;

static void	clear_results()
{
	while (g_items) {
		item *save = g_items;
		g_items = g_items->next;
		free(save);
	}
}

static void add_result(int nbr, cell *c, pl_idx_t c_ctx)
{
	item *ptr = malloc(sizeof(item));
	ptr->c = c;
	ptr->c_ctx = c_ctx;
	ptr->nbr = nbr;
	ptr->next = g_items;
	g_items = ptr;
}

static int check_duplicate_result(query *q, int nbr, cell *c, pl_idx_t c_ctx)
{
	const item *ptr = g_items;

	while (ptr) {
		if (!compare(q, c, c_ctx, ptr->c, ptr->c_ctx)) {
			return ptr->nbr;
		}

		ptr = ptr->next;
	}

	add_result(nbr, c, c_ctx);
	return -1;
}

static int varunformat(const char *s)
{
	if ((*s < 'A') || (*s > 'Z'))
		return -1;

	char ch = 0;
	unsigned i = 0;
	sscanf(s, "%c%u", &ch, &i);

	if (i > 26)
		return -1;

	unsigned j = ch - 'A' + (i * 26);
	return (int)j;
}

static bool any_attributed(const query *q)
{
	const parser *p = q->p;
	const frame *f = GET_FIRST_FRAME();
	bool any = false;

	for (unsigned i = 0; i < p->nbr_vars; i++) {
		if (!strcmp(p->vartab.var_name[i], "_"))
			continue;

		const slot *e = GET_SLOT(f, i);

		if (!is_empty(&e->c) || !e->c.attrs)
			continue;

		any = true;
	}

	return any;
}

void dump_vars(query *q, bool partial)
{
	if (q->in_attvar_print)
		return;

	parser *p = q->p;
	frame *f = GET_FIRST_FRAME();
	q->is_dump_vars = true;
	q->pl->tab_idx = 0;
	bool any = false;

	// Build the ignore list for variable name clashes....

	for (unsigned i = 0; i < MAX_ARITY; i++)
		q->ignore[i] = false;

	for (unsigned i = 0; i < p->nbr_vars; i++) {
		int j;

		if ((p->vartab.var_name[i][0] == '_')
			&& ((j = varunformat(p->vartab.var_name[i]+1)) != -1))
			q->ignore[j] = true;
	}

	// Build the variable-names list for dumping vars...

	for (unsigned i = 0; i < p->nbr_vars; i++) {
		cell tmp[3];
		make_structure(tmp, g_eq_s, NULL, 2, 2);
		make_cstring(tmp+1, p->vartab.var_name[i]);
		make_variable(tmp+2, g_anon_s, i);

		if (i == 0)
			allocate_list(q, tmp);
		else
			append_list(q, tmp);
	}

	// Now go through the variables (slots actually) and
	// dump them out...

	cell *vlist = p->nbr_vars ? end_list(q) : NULL;
	bool space = false;

	for (unsigned i = 0; i < p->nbr_vars; i++) {
		if (!strcmp(p->vartab.var_name[i], "_"))
			continue;

		slot *e = GET_SLOT(f, i);

		if (is_empty(&e->c))
			continue;

		cell *c = deref(q, &e->c, e->ctx);
		pl_idx_t c_ctx = q->latest_ctx;

		if (is_indirect(&e->c)) {
			c = e->c.val_ptr;
			c_ctx = e->ctx;
		}

		if (is_variable(c) && is_anon(c))
			continue;

		if (any)
			fprintf(stdout, ", ");
		else if (!q->is_redo)
			fprintf(stdout, "   ");
		else
			fprintf(stdout, "  ");

		fprintf(stdout, "%s = ", p->vartab.var_name[i]);

		int j = check_duplicate_result(q, i, c, c_ctx);

		if ((j >= 0) && ((unsigned)j != i)) {
			fprintf(stdout, "%s", p->vartab.var_name[j]);
			any = true;
			continue;
		}

		bool parens = false;
		space = false;

		if (is_structure(c)) {
			unsigned pri = find_op(q->st.m, GET_STR(q, c), GET_OP(c));

			if (pri >= 700)
				parens = true;
		}

		if (is_atom(c) && !is_string(c) && LEN_STR(q, c) && !is_nil(c)) {
			if (search_op(q->st.m, GET_STR(q, c), NULL, false))
				parens = true;

			if (!parens) {
				const char *src = GET_STR(q, c);
				int ch = peek_char_utf8(src);

				if (!iswalpha(ch) && (ch != '_'))
					space = true;
			}
		}

		if (parens) fputc('(', stdout);
		int saveq = q->quoted;
		q->quoted = 1;
		q->variable_names = vlist;
		q->variable_names_ctx = INITIAL_FRAME;
		q->max_depth = 9;

		print_term(q, stdout, c, c_ctx, 1);

		if (parens) fputc(')', stdout);
		if (q->did_quote) space = false;
		q->quoted = saveq;
		any = true;
	}

	if (any && any_attributed(q))
		fprintf(stdout, ",\n");

	// Print residual goals of attributed variables...

	if (any_attributed(q) && !q->in_attvar_print) {
		q->variable_names = vlist;
		q->variable_names_ctx = INITIAL_FRAME;
		cell p1;
		make_literal(&p1, index_from_pool(q->pl, "dump_attvars"));
		cell *tmp = clone_to_heap(q, false, &p1, 1);
		pl_idx_t nbr_cells = 0 + p1.nbr_cells;
		make_end(tmp+nbr_cells);
		q->st.curr_cell = tmp;
		q->in_attvar_print = true;
		start(q);
		q->in_attvar_print = false;
		any = true;
	}

	g_tpl_interrupt = false;
	q->is_dump_vars = false;

	if (any && !partial) {
		if (space) fprintf(stdout, " ");
		fprintf(stdout, ".\n");
		fflush(stdout);
	}

	q->pl->did_dump_vars = any;
	clear_write_options(q);
	clear_results();
}

int check_interrupt(query *q)
{
	signal(SIGINT, &sigfn);
	g_tpl_interrupt = 0;

	for (;;) {
		printf("\nAction or (h)elp: ");

		LOOP:

		fflush(stdout);
		int ch = history_getch();
		printf("%c\n", ch);

		if (ch == 'h') {
			printf("Action (a)ll, (e)nd, e(x)it, (r)etry, (c)ontinue, (t)race, cree(p): ");
			goto LOOP;
		}

		if (ch == 't') {
			q->trace = !q->trace;
			return 0;
		}

		if (ch == 'p') {
			q->trace = q->creep = !q->creep;
			return 0;
		}

		if ((ch == ';') || (ch == ' ') || (ch == 'r')) {
			return 0;
		}

		if (ch == '\n')
			return -1;

		if (ch == 'e') {
			q->abort = true;
			return 1;
		}

		if (ch == 'x') {
			if (!q->run_init)
				printf("\n");

			signal(SIGINT, NULL);
			q->halt = true;
			return 1;
		}
	}
}

bool check_redo(query *q)
{
	if (q->do_dump_vars && q->cp) {
		dump_vars(q, true);

		if (!q->pl->did_dump_vars)
			printf("   true");
	}

	fflush(stdout);

	if (q->autofail) {
		printf("\n;");
		fflush(stdout);
		q->is_redo = true;
		q->retry = QUERY_RETRY;
		q->pl->did_dump_vars = false;
		return false;
	}

	for (;;) {
		printf("\n");
		fflush(stdout);
		int ch = history_getch();

		if ((ch == 'h') || (ch == '?')) {
			printf("Action (a)ll, e(x)it, (r)etry, (e)nd:\n");
			fflush(stdout);
			continue;
		}

		if (ch == 'a') {
			printf(";");
			fflush(stdout);
			q->is_redo = true;
			q->retry = QUERY_RETRY;
			q->pl->did_dump_vars = false;
			q->autofail = true;
			break;
		}

		if ((ch == ' ') || (ch == ';') || (ch == 'r')) {
			printf(";");
			fflush(stdout);
			q->is_redo = true;
			q->retry = QUERY_RETRY;
			q->pl->did_dump_vars = false;
			break;
		}

		if ((ch == '\n') || (ch == 'e')) {
			printf(";  ... .\n");
			q->pl->did_dump_vars = true;
			q->abort = true;
			return true;
		}

		if (ch == 'x') {
			if (!q->run_init)
				printf("\n");

			signal(SIGINT, NULL);
			q->error = q->halt = true;
			return true;
		}
	}

	return false;
}
