#include "cache.h"
#include "commit.h"
#include "graph.h"
#include "diff.h"
#include "revision.h"

/* Internal API */

/*
 * Output the next line for a graph.
 * This formats the next graph line into the specified strbuf.  It is not
 * terminated with a newline.
 *
 * Returns 1 if the line includes the current commit, and 0 otherwise.
 * graph_next_line() will return 1 exactly once for each time
 * graph_update() is called.
 */
static int graph_next_line(struct git_graph *graph, struct strbuf *sb);

/*
 * Output a padding line in the graph.
 * This is similar to graph_next_line().  However, it is guaranteed to
 * never print the current commit line.  Instead, if the commit line is
 * next, it will simply output a line of vertical padding, extending the
 * branch lines downwards, but leaving them otherwise unchanged.
 */
static void graph_padding_line(struct git_graph *graph, struct strbuf *sb);

/*
 * Print a strbuf to stdout.  If the graph is non-NULL, all lines but the
 * first will be prefixed with the graph output.
 *
 * If the strbuf ends with a newline, the output will end after this
 * newline.  A new graph line will not be printed after the final newline.
 * If the strbuf is empty, no output will be printed.
 *
 * Since the first line will not include the graph ouput, the caller is
 * responsible for printing this line's graph (perhaps via
 * graph_show_commit() or graph_show_oneline()) before calling
 * graph_show_strbuf().
 */
static void graph_show_strbuf(struct git_graph *graph, struct strbuf const *sb);

/*
 * TODO:
 * - Add colors to the graph.
 *   Pick a color for each column, and print all characters
 *   in that column with the specified color.
 *
 * - Limit the number of columns, similar to the way gitk does.
 *   If we reach more than a specified number of columns, omit
 *   sections of some columns.
 *
 * - The output during the GRAPH_PRE_COMMIT and GRAPH_COLLAPSING states
 *   could be made more compact by printing horizontal lines, instead of
 *   long diagonal lines.  For example, during collapsing, something like
 *   this:          instead of this:
 *   | | | | |      | | | | |
 *   | |_|_|/       | | | |/
 *   |/| | |        | | |/|
 *   | | | |        | |/| |
 *                  |/| | |
 *                  | | | |
 *
 *   If there are several parallel diagonal lines, they will need to be
 *   replaced with horizontal lines on subsequent rows.
 */

struct column {
	/*
	 * The parent commit of this column.
	 */
	struct commit *commit;
	/*
	 * XXX: Once we add support for colors, struct column could also
	 * contain the color of its branch line.
	 */
};

enum graph_state {
	GRAPH_PADDING,
	GRAPH_SKIP,
	GRAPH_PRE_COMMIT,
	GRAPH_COMMIT,
	GRAPH_POST_MERGE,
	GRAPH_COLLAPSING
};

struct git_graph {
	/*
	 * The commit currently being processed
	 */
	struct commit *commit;
	/* The rev-info used for the current traversal */
	struct rev_info *revs;
	/*
	 * The number of interesting parents that this commit has.
	 *
	 * Note that this is not the same as the actual number of parents.
	 * This count excludes parents that won't be printed in the graph
	 * output, as determined by graph_is_interesting().
	 */
	int num_parents;
	/*
	 * The width of the graph output for this commit.
	 * All rows for this commit are padded to this width, so that
	 * messages printed after the graph output are aligned.
	 */
	int width;
	/*
	 * The next expansion row to print
	 * when state is GRAPH_PRE_COMMIT
	 */
	int expansion_row;
	/*
	 * The current output state.
	 * This tells us what kind of line graph_next_line() should output.
	 */
	enum graph_state state;
	/*
	 * The output state for the previous line of output.
	 * This is primarily used to determine how the first merge line
	 * should appear, based on the last line of the previous commit.
	 */
	enum graph_state prev_state;
	/*
	 * The index of the column that refers to this commit.
	 *
	 * If none of the incoming columns refer to this commit,
	 * this will be equal to num_columns.
	 */
	int commit_index;
	/*
	 * The commit_index for the previously displayed commit.
	 *
	 * This is used to determine how the first line of a merge
	 * graph output should appear, based on the last line of the
	 * previous commit.
	 */
	int prev_commit_index;
	/*
	 * The maximum number of columns that can be stored in the columns
	 * and new_columns arrays.  This is also half the number of entries
	 * that can be stored in the mapping and new_mapping arrays.
	 */
	int column_capacity;
	/*
	 * The number of columns (also called "branch lines" in some places)
	 */
	int num_columns;
	/*
	 * The number of columns in the new_columns array
	 */
	int num_new_columns;
	/*
	 * The number of entries in the mapping array
	 */
	int mapping_size;
	/*
	 * The column state before we output the current commit.
	 */
	struct column *columns;
	/*
	 * The new column state after we output the current commit.
	 * Only valid when state is GRAPH_COLLAPSING.
	 */
	struct column *new_columns;
	/*
	 * An array that tracks the current state of each
	 * character in the output line during state GRAPH_COLLAPSING.
	 * Each entry is -1 if this character is empty, or a non-negative
	 * integer if the character contains a branch line.  The value of
	 * the integer indicates the target position for this branch line.
	 * (I.e., this array maps the current column positions to their
	 * desired positions.)
	 *
	 * The maximum capacity of this array is always
	 * sizeof(int) * 2 * column_capacity.
	 */
	int *mapping;
	/*
	 * A temporary array for computing the next mapping state
	 * while we are outputting a mapping line.  This is stored as part
	 * of the git_graph simply so we don't have to allocate a new
	 * temporary array each time we have to output a collapsing line.
	 */
	int *new_mapping;
};

struct git_graph *graph_init(struct rev_info *opt)
{
	struct git_graph *graph = xmalloc(sizeof(struct git_graph));
	graph->commit = NULL;
	graph->revs = opt;
	graph->num_parents = 0;
	graph->expansion_row = 0;
	graph->state = GRAPH_PADDING;
	graph->prev_state = GRAPH_PADDING;
	graph->commit_index = 0;
	graph->prev_commit_index = 0;
	graph->num_columns = 0;
	graph->num_new_columns = 0;
	graph->mapping_size = 0;

	/*
	 * Allocate a reasonably large default number of columns
	 * We'll automatically grow columns later if we need more room.
	 */
	graph->column_capacity = 30;
	graph->columns = xmalloc(sizeof(struct column) *
				 graph->column_capacity);
	graph->new_columns = xmalloc(sizeof(struct column) *
				     graph->column_capacity);
	graph->mapping = xmalloc(sizeof(int) * 2 * graph->column_capacity);
	graph->new_mapping = xmalloc(sizeof(int) * 2 * graph->column_capacity);

	return graph;
}

static void graph_update_state(struct git_graph *graph, enum graph_state s)
{
	graph->prev_state = graph->state;
	graph->state = s;
}

static void graph_ensure_capacity(struct git_graph *graph, int num_columns)
{
	if (graph->column_capacity >= num_columns)
		return;

	do {
		graph->column_capacity *= 2;
	} while (graph->column_capacity < num_columns);

	graph->columns = xrealloc(graph->columns,
				  sizeof(struct column) *
				  graph->column_capacity);
	graph->new_columns = xrealloc(graph->new_columns,
				      sizeof(struct column) *
				      graph->column_capacity);
	graph->mapping = xrealloc(graph->mapping,
				  sizeof(int) * 2 * graph->column_capacity);
	graph->new_mapping = xrealloc(graph->new_mapping,
				      sizeof(int) * 2 * graph->column_capacity);
}

/*
 * Returns 1 if the commit will be printed in the graph output,
 * and 0 otherwise.
 */
static int graph_is_interesting(struct git_graph *graph, struct commit *commit)
{
	/*
	 * If revs->boundary is set, commits whose children have
	 * been shown are always interesting, even if they have the
	 * UNINTERESTING or TREESAME flags set.
	 */
	if (graph->revs && graph->revs->boundary) {
		if (commit->object.flags & CHILD_SHOWN)
			return 1;
	}

	/*
	 * Uninteresting and pruned commits won't be printed
	 */
	return (commit->object.flags & (UNINTERESTING | TREESAME)) ? 0 : 1;
}

static struct commit_list *next_interesting_parent(struct git_graph *graph,
						   struct commit_list *orig)
{
	struct commit_list *list;

	/*
	 * If revs->first_parent_only is set, only the first
	 * parent is interesting.  None of the others are.
	 */
	if (graph->revs->first_parent_only)
		return NULL;

	/*
	 * Return the next interesting commit after orig
	 */
	for (list = orig->next; list; list = list->next) {
		if (graph_is_interesting(graph, list->item))
			return list;
	}

	return NULL;
}

static struct commit_list *first_interesting_parent(struct git_graph *graph)
{
	struct commit_list *parents = graph->commit->parents;

	/*
	 * If this commit has no parents, ignore it
	 */
	if (!parents)
		return NULL;

	/*
	 * If the first parent is interesting, return it
	 */
	if (graph_is_interesting(graph, parents->item))
		return parents;

	/*
	 * Otherwise, call next_interesting_parent() to get
	 * the next interesting parent
	 */
	return next_interesting_parent(graph, parents);
}

static void graph_insert_into_new_columns(struct git_graph *graph,
					  struct commit *commit,
					  int *mapping_index)
{
	int i;

	/*
	 * If the commit is already in the new_columns list, we don't need to
	 * add it.  Just update the mapping correctly.
	 */
	for (i = 0; i < graph->num_new_columns; i++) {
		if (graph->new_columns[i].commit == commit) {
			graph->mapping[*mapping_index] = i;
			*mapping_index += 2;
			return;
		}
	}

	/*
	 * This commit isn't already in new_columns.  Add it.
	 */
	graph->new_columns[graph->num_new_columns].commit = commit;
	graph->mapping[*mapping_index] = graph->num_new_columns;
	*mapping_index += 2;
	graph->num_new_columns++;
}

static void graph_update_width(struct git_graph *graph,
			       int is_commit_in_existing_columns)
{
	/*
	 * Compute the width needed to display the graph for this commit.
	 * This is the maximum width needed for any row.  All other rows
	 * will be padded to this width.
	 *
	 * Compute the number of columns in the widest row:
	 * Count each existing column (graph->num_columns), and each new
	 * column added by this commit.
	 */
	int max_cols = graph->num_columns + graph->num_parents;

	/*
	 * Even if the current commit has no parents to be printed, it
	 * still takes up a column for itself.
	 */
	if (graph->num_parents < 1)
		max_cols++;

	/*
	 * We added a column for the the current commit as part of
	 * graph->num_parents.  If the current commit was already in
	 * graph->columns, then we have double counted it.
	 */
	if (is_commit_in_existing_columns)
		max_cols--;

	/*
	 * Each column takes up 2 spaces
	 */
	graph->width = max_cols * 2;
}

static void graph_update_columns(struct git_graph *graph)
{
	struct commit_list *parent;
	struct column *tmp_columns;
	int max_new_columns;
	int mapping_idx;
	int i, seen_this, is_commit_in_columns;

	/*
	 * Swap graph->columns with graph->new_columns
	 * graph->columns contains the state for the previous commit,
	 * and new_columns now contains the state for our commit.
	 *
	 * We'll re-use the old columns array as storage to compute the new
	 * columns list for the commit after this one.
	 */
	tmp_columns = graph->columns;
	graph->columns = graph->new_columns;
	graph->num_columns = graph->num_new_columns;

	graph->new_columns = tmp_columns;
	graph->num_new_columns = 0;

	/*
	 * Now update new_columns and mapping with the information for the
	 * commit after this one.
	 *
	 * First, make sure we have enough room.  At most, there will
	 * be graph->num_columns + graph->num_parents columns for the next
	 * commit.
	 */
	max_new_columns = graph->num_columns + graph->num_parents;
	graph_ensure_capacity(graph, max_new_columns);

	/*
	 * Clear out graph->mapping
	 */
	graph->mapping_size = 2 * max_new_columns;
	for (i = 0; i < graph->mapping_size; i++)
		graph->mapping[i] = -1;

	/*
	 * Populate graph->new_columns and graph->mapping
	 *
	 * Some of the parents of this commit may already be in
	 * graph->columns.  If so, graph->new_columns should only contain a
	 * single entry for each such commit.  graph->mapping should
	 * contain information about where each current branch line is
	 * supposed to end up after the collapsing is performed.
	 */
	seen_this = 0;
	mapping_idx = 0;
	is_commit_in_columns = 1;
	for (i = 0; i <= graph->num_columns; i++) {
		struct commit *col_commit;
		if (i == graph->num_columns) {
			if (seen_this)
				break;
			is_commit_in_columns = 0;
			col_commit = graph->commit;
		} else {
			col_commit = graph->columns[i].commit;
		}

		if (col_commit == graph->commit) {
			int old_mapping_idx = mapping_idx;
			seen_this = 1;
			graph->commit_index = i;
			for (parent = first_interesting_parent(graph);
			     parent;
			     parent = next_interesting_parent(graph, parent)) {
				graph_insert_into_new_columns(graph,
							      parent->item,
							      &mapping_idx);
			}
			/*
			 * We always need to increment mapping_idx by at
			 * least 2, even if it has no interesting parents.
			 * The current commit always takes up at least 2
			 * spaces.
			 */
			if (mapping_idx == old_mapping_idx)
				mapping_idx += 2;
		} else {
			graph_insert_into_new_columns(graph, col_commit,
						      &mapping_idx);
		}
	}

	/*
	 * Shrink mapping_size to be the minimum necessary
	 */
	while (graph->mapping_size > 1 &&
	       graph->mapping[graph->mapping_size - 1] < 0)
		graph->mapping_size--;

	/*
	 * Compute graph->width for this commit
	 */
	graph_update_width(graph, is_commit_in_columns);
}

void graph_update(struct git_graph *graph, struct commit *commit)
{
	struct commit_list *parent;

	/*
	 * Set the new commit
	 */
	graph->commit = commit;

	/*
	 * Count how many interesting parents this commit has
	 */
	graph->num_parents = 0;
	for (parent = first_interesting_parent(graph);
	     parent;
	     parent = next_interesting_parent(graph, parent))
	{
		graph->num_parents++;
	}

	/*
	 * Store the old commit_index in prev_commit_index.
	 * graph_update_columns() will update graph->commit_index for this
	 * commit.
	 */
	graph->prev_commit_index = graph->commit_index;

	/*
	 * Call graph_update_columns() to update
	 * columns, new_columns, and mapping.
	 */
	graph_update_columns(graph);

	graph->expansion_row = 0;

	/*
	 * Update graph->state.
	 * Note that we don't call graph_update_state() here, since
	 * we don't want to update graph->prev_state.  No line for
	 * graph->state was ever printed.
	 *
	 * If the previous commit didn't get to the GRAPH_PADDING state,
	 * it never finished its output.  Goto GRAPH_SKIP, to print out
	 * a line to indicate that portion of the graph is missing.
	 *
	 * If there are 3 or more parents, we may need to print extra rows
	 * before the commit, to expand the branch lines around it and make
	 * room for it.  We need to do this only if there is a branch row
	 * (or more) to the right of this commit.
	 *
	 * If there are less than 3 parents, we can immediately print the
	 * commit line.
	 */
	if (graph->state != GRAPH_PADDING)
		graph->state = GRAPH_SKIP;
	else if (graph->num_parents >= 3 &&
		 graph->commit_index < (graph->num_columns - 1))
		graph->state = GRAPH_PRE_COMMIT;
	else
		graph->state = GRAPH_COMMIT;
}

static int graph_is_mapping_correct(struct git_graph *graph)
{
	int i;

	/*
	 * The mapping is up to date if each entry is at its target,
	 * or is 1 greater than its target.
	 * (If it is 1 greater than the target, '/' will be printed, so it
	 * will look correct on the next row.)
	 */
	for (i = 0; i < graph->mapping_size; i++) {
		int target = graph->mapping[i];
		if (target < 0)
			continue;
		if (target == (i / 2))
			continue;
		return 0;
	}

	return 1;
}

static void graph_pad_horizontally(struct git_graph *graph, struct strbuf *sb)
{
	/*
	 * Add additional spaces to the end of the strbuf, so that all
	 * lines for a particular commit have the same width.
	 *
	 * This way, fields printed to the right of the graph will remain
	 * aligned for the entire commit.
	 */
	int extra;
	if (sb->len >= graph->width)
		return;

	extra = graph->width - sb->len;
	strbuf_addf(sb, "%*s", (int) extra, "");
}

static void graph_output_padding_line(struct git_graph *graph,
				      struct strbuf *sb)
{
	int i;

	/*
	 * We could conceivable be called with a NULL commit
	 * if our caller has a bug, and invokes graph_next_line()
	 * immediately after graph_init(), without first calling
	 * graph_update().  Return without outputting anything in this
	 * case.
	 */
	if (!graph->commit)
		return;

	/*
	 * Output a padding row, that leaves all branch lines unchanged
	 */
	for (i = 0; i < graph->num_new_columns; i++) {
		strbuf_addstr(sb, "| ");
	}

	graph_pad_horizontally(graph, sb);
}

static void graph_output_skip_line(struct git_graph *graph, struct strbuf *sb)
{
	/*
	 * Output an ellipsis to indicate that a portion
	 * of the graph is missing.
	 */
	strbuf_addstr(sb, "...");
	graph_pad_horizontally(graph, sb);

	if (graph->num_parents >= 3 &&
	    graph->commit_index < (graph->num_columns - 1))
		graph_update_state(graph, GRAPH_PRE_COMMIT);
	else
		graph_update_state(graph, GRAPH_COMMIT);
}

static void graph_output_pre_commit_line(struct git_graph *graph,
					 struct strbuf *sb)
{
	int num_expansion_rows;
	int i, seen_this;

	/*
	 * This function formats a row that increases the space around a commit
	 * with multiple parents, to make room for it.  It should only be
	 * called when there are 3 or more parents.
	 *
	 * We need 2 extra rows for every parent over 2.
	 */
	assert(graph->num_parents >= 3);
	num_expansion_rows = (graph->num_parents - 2) * 2;

	/*
	 * graph->expansion_row tracks the current expansion row we are on.
	 * It should be in the range [0, num_expansion_rows - 1]
	 */
	assert(0 <= graph->expansion_row &&
	       graph->expansion_row < num_expansion_rows);

	/*
	 * Output the row
	 */
	seen_this = 0;
	for (i = 0; i < graph->num_columns; i++) {
		struct column *col = &graph->columns[i];
		if (col->commit == graph->commit) {
			seen_this = 1;
			strbuf_addf(sb, "| %*s", graph->expansion_row, "");
		} else if (seen_this && (graph->expansion_row == 0)) {
			/*
			 * This is the first line of the pre-commit output.
			 * If the previous commit was a merge commit and
			 * ended in the GRAPH_POST_MERGE state, all branch
			 * lines after graph->prev_commit_index were
			 * printed as "\" on the previous line.  Continue
			 * to print them as "\" on this line.  Otherwise,
			 * print the branch lines as "|".
			 */
			if (graph->prev_state == GRAPH_POST_MERGE &&
			    graph->prev_commit_index < i)
				strbuf_addstr(sb, "\\ ");
			else
				strbuf_addstr(sb, "| ");
		} else if (seen_this && (graph->expansion_row > 0)) {
			strbuf_addstr(sb, "\\ ");
		} else {
			strbuf_addstr(sb, "| ");
		}
	}

	graph_pad_horizontally(graph, sb);

	/*
	 * Increment graph->expansion_row,
	 * and move to state GRAPH_COMMIT if necessary
	 */
	graph->expansion_row++;
	if (graph->expansion_row >= num_expansion_rows)
		graph_update_state(graph, GRAPH_COMMIT);
}

static void graph_output_commit_char(struct git_graph *graph, struct strbuf *sb)
{
	/*
	 * For boundary commits, print 'o'
	 * (We should only see boundary commits when revs->boundary is set.)
	 */
	if (graph->commit->object.flags & BOUNDARY) {
		assert(graph->revs->boundary);
		strbuf_addch(sb, 'o');
		return;
	}

	/*
	 * If revs->left_right is set, print '<' for commits that
	 * come from the left side, and '>' for commits from the right
	 * side.
	 */
	if (graph->revs && graph->revs->left_right) {
		if (graph->commit->object.flags & SYMMETRIC_LEFT)
			strbuf_addch(sb, '<');
		else
			strbuf_addch(sb, '>');
		return;
	}

	/*
	 * Print '*' in all other cases
	 */
	strbuf_addch(sb, '*');
}

static void graph_output_commit_line(struct git_graph *graph, struct strbuf *sb)
{
	int seen_this = 0;
	int i, j;

	/*
	 * Output the row containing this commit
	 * Iterate up to and including graph->num_columns,
	 * since the current commit may not be in any of the existing
	 * columns.  (This happens when the current commit doesn't have any
	 * children that we have already processed.)
	 */
	seen_this = 0;
	for (i = 0; i <= graph->num_columns; i++) {
		struct commit *col_commit;
		if (i == graph->num_columns) {
			if (seen_this)
				break;
			col_commit = graph->commit;
		} else {
			col_commit = graph->columns[i].commit;
		}

		if (col_commit == graph->commit) {
			seen_this = 1;
			graph_output_commit_char(graph, sb);

			if (graph->num_parents < 3)
				strbuf_addch(sb, ' ');
			else {
				int num_dashes =
					((graph->num_parents - 2) * 2) - 1;
				for (j = 0; j < num_dashes; j++)
					strbuf_addch(sb, '-');
				strbuf_addstr(sb, ". ");
			}
		} else if (seen_this && (graph->num_parents > 2)) {
			strbuf_addstr(sb, "\\ ");
		} else if (seen_this && (graph->num_parents == 2)) {
			/*
			 * This is a 2-way merge commit.
			 * There is no GRAPH_PRE_COMMIT stage for 2-way
			 * merges, so this is the first line of output
			 * for this commit.  Check to see what the previous
			 * line of output was.
			 *
			 * If it was GRAPH_POST_MERGE, the branch line
			 * coming into this commit may have been '\',
			 * and not '|' or '/'.  If so, output the branch
			 * line as '\' on this line, instead of '|'.  This
			 * makes the output look nicer.
			 */
			if (graph->prev_state == GRAPH_POST_MERGE &&
			    graph->prev_commit_index < i)
				strbuf_addstr(sb, "\\ ");
			else
				strbuf_addstr(sb, "| ");
		} else {
			strbuf_addstr(sb, "| ");
		}
	}

	graph_pad_horizontally(graph, sb);

	/*
	 * Update graph->state
	 */
	if (graph->num_parents > 1)
		graph_update_state(graph, GRAPH_POST_MERGE);
	else if (graph_is_mapping_correct(graph))
		graph_update_state(graph, GRAPH_PADDING);
	else
		graph_update_state(graph, GRAPH_COLLAPSING);
}

static void graph_output_post_merge_line(struct git_graph *graph, struct strbuf *sb)
{
	int seen_this = 0;
	int i, j;

	/*
	 * Output the post-merge row
	 */
	for (i = 0; i <= graph->num_columns; i++) {
		struct commit *col_commit;
		if (i == graph->num_columns) {
			if (seen_this)
				break;
			col_commit = graph->commit;
		} else {
			col_commit = graph->columns[i].commit;
		}

		if (col_commit == graph->commit) {
			seen_this = 1;
			strbuf_addch(sb, '|');
			for (j = 0; j < graph->num_parents - 1; j++)
				strbuf_addstr(sb, "\\ ");
		} else if (seen_this) {
			strbuf_addstr(sb, "\\ ");
		} else {
			strbuf_addstr(sb, "| ");
		}
	}

	graph_pad_horizontally(graph, sb);

	/*
	 * Update graph->state
	 */
	if (graph_is_mapping_correct(graph))
		graph_update_state(graph, GRAPH_PADDING);
	else
		graph_update_state(graph, GRAPH_COLLAPSING);
}

static void graph_output_collapsing_line(struct git_graph *graph, struct strbuf *sb)
{
	int i;
	int *tmp_mapping;

	/*
	 * Clear out the new_mapping array
	 */
	for (i = 0; i < graph->mapping_size; i++)
		graph->new_mapping[i] = -1;

	for (i = 0; i < graph->mapping_size; i++) {
		int target = graph->mapping[i];
		if (target < 0)
			continue;

		/*
		 * Since update_columns() always inserts the leftmost
		 * column first, each branch's target location should
		 * always be either its current location or to the left of
		 * its current location.
		 *
		 * We never have to move branches to the right.  This makes
		 * the graph much more legible, since whenever branches
		 * cross, only one is moving directions.
		 */
		assert(target * 2 <= i);

		if (target * 2 == i) {
			/*
			 * This column is already in the
			 * correct place
			 */
			assert(graph->new_mapping[i] == -1);
			graph->new_mapping[i] = target;
		} else if (graph->new_mapping[i - 1] < 0) {
			/*
			 * Nothing is to the left.
			 * Move to the left by one
			 */
			graph->new_mapping[i - 1] = target;
		} else if (graph->new_mapping[i - 1] == target) {
			/*
			 * There is a branch line to our left
			 * already, and it is our target.  We
			 * combine with this line, since we share
			 * the same parent commit.
			 *
			 * We don't have to add anything to the
			 * output or new_mapping, since the
			 * existing branch line has already taken
			 * care of it.
			 */
		} else {
			/*
			 * There is a branch line to our left,
			 * but it isn't our target.  We need to
			 * cross over it.
			 *
			 * The space just to the left of this
			 * branch should always be empty.
			 */
			assert(graph->new_mapping[i - 1] > target);
			assert(graph->new_mapping[i - 2] < 0);
			graph->new_mapping[i - 2] = target;
		}
	}

	/*
	 * The new mapping may be 1 smaller than the old mapping
	 */
	if (graph->new_mapping[graph->mapping_size - 1] < 0)
		graph->mapping_size--;

	/*
	 * Output out a line based on the new mapping info
	 */
	for (i = 0; i < graph->mapping_size; i++) {
		int target = graph->new_mapping[i];
		if (target < 0)
			strbuf_addch(sb, ' ');
		else if (target * 2 == i)
			strbuf_addch(sb, '|');
		else
			strbuf_addch(sb, '/');
	}

	graph_pad_horizontally(graph, sb);

	/*
	 * Swap mapping and new_mapping
	 */
	tmp_mapping = graph->mapping;
	graph->mapping = graph->new_mapping;
	graph->new_mapping = tmp_mapping;

	/*
	 * If graph->mapping indicates that all of the branch lines
	 * are already in the correct positions, we are done.
	 * Otherwise, we need to collapse some branch lines together.
	 */
	if (graph_is_mapping_correct(graph))
		graph_update_state(graph, GRAPH_PADDING);
}

static int graph_next_line(struct git_graph *graph, struct strbuf *sb)
{
	switch (graph->state) {
	case GRAPH_PADDING:
		graph_output_padding_line(graph, sb);
		return 0;
	case GRAPH_SKIP:
		graph_output_skip_line(graph, sb);
		return 0;
	case GRAPH_PRE_COMMIT:
		graph_output_pre_commit_line(graph, sb);
		return 0;
	case GRAPH_COMMIT:
		graph_output_commit_line(graph, sb);
		return 1;
	case GRAPH_POST_MERGE:
		graph_output_post_merge_line(graph, sb);
		return 0;
	case GRAPH_COLLAPSING:
		graph_output_collapsing_line(graph, sb);
		return 0;
	}

	assert(0);
	return 0;
}

static void graph_padding_line(struct git_graph *graph, struct strbuf *sb)
{
	int i, j;

	if (graph->state != GRAPH_COMMIT) {
		graph_next_line(graph, sb);
		return;
	}

	/*
	 * Output the row containing this commit
	 * Iterate up to and including graph->num_columns,
	 * since the current commit may not be in any of the existing
	 * columns.  (This happens when the current commit doesn't have any
	 * children that we have already processed.)
	 */
	for (i = 0; i < graph->num_columns; i++) {
		struct commit *col_commit = graph->columns[i].commit;
		if (col_commit == graph->commit) {
			strbuf_addch(sb, '|');

			if (graph->num_parents < 3)
				strbuf_addch(sb, ' ');
			else {
				int num_spaces = ((graph->num_parents - 2) * 2);
				for (j = 0; j < num_spaces; j++)
					strbuf_addch(sb, ' ');
			}
		} else {
			strbuf_addstr(sb, "| ");
		}
	}

	graph_pad_horizontally(graph, sb);

	/*
	 * Update graph->prev_state since we have output a padding line
	 */
	graph->prev_state = GRAPH_PADDING;
}

int graph_is_commit_finished(struct git_graph const *graph)
{
	return (graph->state == GRAPH_PADDING);
}

void graph_show_commit(struct git_graph *graph)
{
	struct strbuf msgbuf;
	int shown_commit_line = 0;

	if (!graph)
		return;

	strbuf_init(&msgbuf, 0);

	while (!shown_commit_line) {
		shown_commit_line = graph_next_line(graph, &msgbuf);
		fwrite(msgbuf.buf, sizeof(char), msgbuf.len, stdout);
		if (!shown_commit_line)
			putchar('\n');
		strbuf_setlen(&msgbuf, 0);
	}

	strbuf_release(&msgbuf);
}

void graph_show_oneline(struct git_graph *graph)
{
	struct strbuf msgbuf;

	if (!graph)
		return;

	strbuf_init(&msgbuf, 0);
	graph_next_line(graph, &msgbuf);
	fwrite(msgbuf.buf, sizeof(char), msgbuf.len, stdout);
	strbuf_release(&msgbuf);
}

void graph_show_padding(struct git_graph *graph)
{
	struct strbuf msgbuf;

	if (!graph)
		return;

	strbuf_init(&msgbuf, 0);
	graph_padding_line(graph, &msgbuf);
	fwrite(msgbuf.buf, sizeof(char), msgbuf.len, stdout);
	strbuf_release(&msgbuf);
}

int graph_show_remainder(struct git_graph *graph)
{
	struct strbuf msgbuf;
	int shown = 0;

	if (!graph)
		return 0;

	if (graph_is_commit_finished(graph))
		return 0;

	strbuf_init(&msgbuf, 0);
	for (;;) {
		graph_next_line(graph, &msgbuf);
		fwrite(msgbuf.buf, sizeof(char), msgbuf.len, stdout);
		strbuf_setlen(&msgbuf, 0);
		shown = 1;

		if (!graph_is_commit_finished(graph))
			putchar('\n');
		else
			break;
	}
	strbuf_release(&msgbuf);

	return shown;
}


static void graph_show_strbuf(struct git_graph *graph, struct strbuf const *sb)
{
	char *p;

	if (!graph) {
		fwrite(sb->buf, sizeof(char), sb->len, stdout);
		return;
	}

	/*
	 * Print the strbuf line by line,
	 * and display the graph info before each line but the first.
	 */
	p = sb->buf;
	while (p) {
		size_t len;
		char *next_p = strchr(p, '\n');
		if (next_p) {
			next_p++;
			len = next_p - p;
		} else {
			len = (sb->buf + sb->len) - p;
		}
		fwrite(p, sizeof(char), len, stdout);
		if (next_p && *next_p != '\0')
			graph_show_oneline(graph);
		p = next_p;
	}
}

void graph_show_commit_msg(struct git_graph *graph,
			   struct strbuf const *sb)
{
	int newline_terminated;

	if (!graph) {
		/*
		 * If there's no graph, just print the message buffer.
		 *
		 * The message buffer for CMIT_FMT_ONELINE and
		 * CMIT_FMT_USERFORMAT are already missing a terminating
		 * newline.  All of the other formats should have it.
		 */
		fwrite(sb->buf, sizeof(char), sb->len, stdout);
		return;
	}

	newline_terminated = (sb->len && sb->buf[sb->len - 1] == '\n');

	/*
	 * Show the commit message
	 */
	graph_show_strbuf(graph, sb);

	/*
	 * If there is more output needed for this commit, show it now
	 */
	if (!graph_is_commit_finished(graph)) {
		/*
		 * If sb doesn't have a terminating newline, print one now,
		 * so we can start the remainder of the graph output on a
		 * new line.
		 */
		if (!newline_terminated)
			putchar('\n');

		graph_show_remainder(graph);

		/*
		 * If sb ends with a newline, our output should too.
		 */
		if (newline_terminated)
			putchar('\n');
	}
}