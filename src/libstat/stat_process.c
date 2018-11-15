/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"
#include "stat_api.h"
#include "rspamd.h"
#include "stat_internal.h"
#include "libmime/message.h"
#include "libmime/images.h"
#include "libserver/html.h"
#include "lua/lua_common.h"
#include "libserver/mempool_vars_internal.h"
#include "utlist.h"
#include <math.h>

#define RSPAMD_CLASSIFY_OP 0
#define RSPAMD_LEARN_OP 1
#define RSPAMD_UNLEARN_OP 2

static const gdouble similarity_treshold = 80.0;

static void
rspamd_stat_tokenize_header (struct rspamd_task *task,
		const gchar *name, const gchar *prefix, GArray *ar)
{
	struct rspamd_mime_header *cur;
	GPtrArray *hdrs;
	guint i;
	rspamd_stat_token_t str;

	hdrs = g_hash_table_lookup (task->raw_headers, name);
	str.flags = RSPAMD_STAT_TOKEN_FLAG_META;

	if (hdrs != NULL) {

		PTR_ARRAY_FOREACH (hdrs, i, cur) {
			if (cur->name != NULL) {
				str.begin = cur->name;
				str.len = strlen (cur->name);
				g_array_append_val (ar, str);
			}
			if (cur->decoded != NULL) {
				str.begin = cur->decoded;
				str.len = strlen (cur->decoded);
				g_array_append_val (ar, str);
			}
			else if (cur->value != NULL) {
				str.begin = cur->value;
				str.len = strlen (cur->value);
				g_array_append_val (ar, str);
			}
		}

		msg_debug_task ("added stat tokens for header '%s'", name);
	}
}

static void
rspamd_stat_tokenize_parts_metadata (struct rspamd_stat_ctx *st_ctx,
		struct rspamd_task *task)
{
	struct rspamd_image *img;
	struct rspamd_mime_part *part;
	struct rspamd_mime_text_part *tp;
	GList *cur;
	GArray *ar;
	rspamd_stat_token_t elt;
	guint i;
	gchar tmpbuf[128];
	lua_State *L = task->cfg->lua_state;
	const gchar *headers_hash;
	struct rspamd_mime_header *hdr;

	ar = g_array_sized_new (FALSE, FALSE, sizeof (elt), 16);
	elt.flags = RSPAMD_STAT_TOKEN_FLAG_META;

	/* Insert images */
	for (i = 0; i < task->parts->len; i ++) {
		part = g_ptr_array_index (task->parts, i);

		if ((part->flags & RSPAMD_MIME_PART_IMAGE) && part->specific.img) {
			img = part->specific.img;

			/* If an image has a linked HTML part, then we push its details to the stat */
			if (img->html_image) {
				elt.begin = (gchar *)"image";
				elt.len = 5;
				g_array_append_val (ar, elt);
				elt.begin = (gchar *)&img->html_image->height;
				elt.len = sizeof (img->html_image->height);
				g_array_append_val (ar, elt);
				elt.begin = (gchar *)&img->html_image->width;
				elt.len = sizeof (img->html_image->width);
				g_array_append_val (ar, elt);
				elt.begin = (gchar *)&img->type;
				elt.len = sizeof (img->type);
				g_array_append_val (ar, elt);

				if (img->filename) {
					elt.begin = (gchar *)img->filename;
					elt.len = strlen (elt.begin);
					g_array_append_val (ar, elt);
				}

				msg_debug_task ("added stat tokens for image '%s'", img->html_image->src);
			}
		}
		else if (part->cd && part->cd->filename.len > 0) {
			elt.begin = (gchar *)part->cd->filename.begin;
			elt.len = part->cd->filename.len;
			g_array_append_val (ar, elt);
		}
	}

	/* Process mime parts */
	for (i = 0; i < task->parts->len; i ++) {
		part = g_ptr_array_index (task->parts, i);

		if (IS_CT_MULTIPART (part->ct)) {
			elt.begin = (gchar *)part->ct->boundary.begin;
			elt.len = part->ct->boundary.len;

			if (elt.len) {
				msg_debug_task ("added stat tokens for mime boundary '%*s'",
						(gint)elt.len, elt.begin);
				g_array_append_val (ar, elt);
			}

			if (part->parsed_data.len > 1) {
				rspamd_snprintf (tmpbuf, sizeof (tmpbuf), "mime%d:%dlog",
						i, (gint)log2 (part->parsed_data.len));
				elt.begin = rspamd_mempool_strdup (task->task_pool, tmpbuf);
				elt.len = strlen (elt.begin);
				g_array_append_val (ar, elt);
			}
		}
	}

	/* Process text parts metadata */
	for (i = 0; i < task->text_parts->len; i ++) {
		tp = g_ptr_array_index (task->text_parts, i);

		if (tp->language != NULL && tp->language[0] != '\0') {
			elt.begin = (gchar *)tp->language;
			elt.len = strlen (elt.begin);
			msg_debug_task ("added stat tokens for part language '%s'", elt.begin);
			g_array_append_val (ar, elt);
		}
		if (tp->real_charset != NULL) {
			elt.begin = (gchar *)tp->real_charset;
			elt.len = strlen (elt.begin);
			msg_debug_task ("added stat tokens for part charset '%s'", elt.begin);
			g_array_append_val (ar, elt);
		}
	}

	cur = g_list_first (task->cfg->classify_headers);

	while (cur) {
		rspamd_stat_tokenize_header (task, cur->data, "UA:", ar);

		cur = g_list_next (cur);
	}

	/* Use headers order */
	headers_hash = rspamd_mempool_get_variable (task->task_pool,
			RSPAMD_MEMPOOL_HEADERS_HASH);

	if (headers_hash) {
		elt.begin = (gchar *)headers_hash;
		elt.len = 16;
		g_array_append_val (ar, elt);
	}

	/* Use more precise headers order */
	cur = g_list_first (task->headers_order->head);
	while (cur) {
		hdr = cur->data;

		if (hdr->name && hdr->type != RSPAMD_HEADER_RECEIVED) {
			elt.begin = hdr->name;
			elt.len = strlen (hdr->name);
			g_array_append_val (ar, elt);
		}

		cur = g_list_next (cur);
	}

	/* Use metatokens plugin from Lua */
	lua_getglobal (L, "rspamd_plugins");

	if (lua_type (L, -1) == LUA_TTABLE) {
		lua_pushstring (L, "stat_metatokens");
		lua_gettable (L, -2);

		if (lua_type (L, -1) == LUA_TTABLE) {
			gint old_top;

			old_top = lua_gettop (L);
			lua_pushstring (L, "callback");
			lua_gettable (L, -2);

			if (lua_type (L, -1) == LUA_TFUNCTION) {
				struct rspamd_task **ptask;

				ptask = lua_newuserdata (L, sizeof (*ptask));
				rspamd_lua_setclass (L, "rspamd{task}", -1);
				*ptask = task;

				if (lua_pcall (L, 1, LUA_MULTRET, 0) != 0) {
					msg_err_task ("stat_metatokens failed: %s",
							lua_tostring (L, -1));
					lua_pop (L, 1);
				} else {
					if (lua_gettop (L) > old_top &&
							lua_istable (L, old_top + 1)) {
						lua_pushvalue (L, old_top + 1);
						/* Iterate over table of tables */
						for (lua_pushnil (L); lua_next (L, -2);
								lua_pop (L, 1)) {
							elt.flags = RSPAMD_STAT_TOKEN_FLAG_META|
									RSPAMD_STAT_TOKEN_FLAG_LUA_META;

							if (lua_isnumber (L, -1)) {
								gdouble num = lua_tonumber (L, -1);
								guint8 *pnum = rspamd_mempool_alloc (
										task->task_pool,
										sizeof (num));

								msg_debug_task ("got metatoken number: %.2f",
										num);
								memcpy (pnum, &num, sizeof (num));
								elt.begin = (gchar *) pnum;
								elt.len = sizeof (num);
								g_array_append_val (ar, elt);
							} else if (lua_isstring (L, -1)) {
								const gchar *str;
								gsize tlen;

								str = lua_tolstring (L, -1, &tlen);
								guint8 *pstr = rspamd_mempool_alloc (
										task->task_pool,
										tlen);
								memcpy (pstr, str, tlen);

								msg_debug_task ("got metatoken string: %*s",
										(gint) tlen, str);
								elt.begin = (gchar *) pstr;
								elt.len = tlen;
								g_array_append_val (ar, elt);
							}
							else if (lua_istable (L, -1)) {
								/* Treat that as unigramms */
								for (lua_pushnil (L); lua_next (L, -2);
										lua_pop (L, 1)) {
									if (lua_isstring (L, -1)) {
										const gchar *str;
										gsize tlen;

										str = lua_tolstring (L, -1, &tlen);
										guint8 *pstr = rspamd_mempool_alloc (
												task->task_pool,
												tlen);
										memcpy (pstr, str, tlen);

										msg_debug_task ("got unigramm "
												"metatoken string: %*s",
												(gint) tlen, str);
										elt.begin = (gchar *) pstr;
										elt.len = tlen;
										elt.flags |= RSPAMD_STAT_TOKEN_FLAG_UNIGRAM;
										g_array_append_val (ar, elt);
									}
								}
							}
						}
					}
				}
			}
		}
	}

	lua_settop (L, 0);
	st_ctx->tokenizer->tokenize_func (st_ctx,
			task->task_pool,
			ar,
			TRUE,
			"META:",
			task->tokens);

	rspamd_mempool_add_destructor (task->task_pool,
			rspamd_array_free_hard, ar);
}

/*
 * Tokenize task using the tokenizer specified
 */
void
rspamd_stat_process_tokenize (struct rspamd_stat_ctx *st_ctx,
		struct rspamd_task *task)
{
	struct rspamd_mime_text_part *part;
	rspamd_cryptobox_hash_state_t hst;
	rspamd_token_t *st_tok;
	GArray *words;
	guint i, reserved_len = 0;
	gdouble *pdiff;
	guchar hout[rspamd_cryptobox_HASHBYTES];
	gchar *b32_hout;

	if (st_ctx == NULL) {
		st_ctx = rspamd_stat_get_ctx ();
	}

	g_assert (st_ctx != NULL);

	for (i = 0; i < task->text_parts->len; i++) {
		part = g_ptr_array_index (task->text_parts, i);

		if (!IS_PART_EMPTY (part) && part->utf_words != NULL) {
			reserved_len += part->utf_words->len;
		}
		/* XXX: normal window size */
		reserved_len += 5;
	}

	task->tokens = g_ptr_array_sized_new (reserved_len);
	rspamd_mempool_add_destructor (task->task_pool,
			rspamd_ptr_array_free_hard, task->tokens);
	pdiff = rspamd_mempool_get_variable (task->task_pool, "parts_distance");

	for (i = 0; i < task->text_parts->len; i ++) {
		part = g_ptr_array_index (task->text_parts, i);

		if (!IS_PART_EMPTY (part) && part->utf_words != NULL) {
			st_ctx->tokenizer->tokenize_func (st_ctx, task->task_pool,
					part->utf_words, IS_PART_UTF (part),
					NULL, task->tokens);
		}


		if (pdiff != NULL && (1.0 - *pdiff) * 100.0 > similarity_treshold) {
			msg_debug_task ("message has two common parts (%.2f), so skip the last one",
					*pdiff);
			break;
		}
	}

	if (task->subject != NULL) {
		words = rspamd_tokenize_subject (task);
		if (words != NULL) {
			st_ctx->tokenizer->tokenize_func (st_ctx,
					task->task_pool,
					words,
					TRUE,
					"SUBJECT",
					task->tokens);

			rspamd_mempool_add_destructor (task->task_pool,
					rspamd_array_free_hard, words);
		}
	}

	rspamd_stat_tokenize_parts_metadata (st_ctx, task);

	/* Produce signature */
	rspamd_cryptobox_hash_init (&hst, NULL, 0);

	PTR_ARRAY_FOREACH (task->tokens, i, st_tok) {
		rspamd_cryptobox_hash_update (&hst, (guchar *)&st_tok->data,
				sizeof (st_tok->data));
	}

	rspamd_cryptobox_hash_final (&hst, hout);
	b32_hout = rspamd_encode_base32 (hout, sizeof (hout));
	/*
	 * We need to strip it to 32 characters providing ~160 bits of
	 * hash distribution
	 */
	b32_hout[32] = '\0';
	rspamd_mempool_set_variable (task->task_pool, RSPAMD_MEMPOOL_STAT_SIGNATURE,
			b32_hout, g_free);
}

static void
rspamd_stat_preprocess (struct rspamd_stat_ctx *st_ctx,
		struct rspamd_task *task, gboolean learn)
{
	guint i;
	struct rspamd_statfile *st;
	gpointer bk_run;

	if (task->tokens == NULL) {
		rspamd_stat_process_tokenize (st_ctx, task);
	}

	task->stat_runtimes = g_ptr_array_sized_new (st_ctx->statfiles->len);
	g_ptr_array_set_size (task->stat_runtimes, st_ctx->statfiles->len);
	rspamd_mempool_add_destructor (task->task_pool,
			rspamd_ptr_array_free_hard, task->stat_runtimes);

	for (i = 0; i < st_ctx->statfiles->len; i ++) {
		st = g_ptr_array_index (st_ctx->statfiles, i);
		g_assert (st != NULL);

		if (st->classifier->cfg->flags & RSPAMD_FLAG_CLASSIFIER_NO_BACKEND) {
			g_ptr_array_index (task->stat_runtimes, i) = NULL;
			continue;
		}

		if (!rspamd_symcache_is_symbol_enabled (task, task->cfg->cache,
				st->stcf->symbol)) {
			g_ptr_array_index (task->stat_runtimes, i) = NULL;
			msg_debug_task ("symbol %s is disabled, skip classification",
					st->stcf->symbol);
			continue;
		}

		bk_run = st->backend->runtime (task, st->stcf, learn, st->bkcf);

		if (bk_run == NULL) {
			msg_err_task ("cannot init backend %s for statfile %s",
					st->backend->name, st->stcf->symbol);
		}

		g_ptr_array_index (task->stat_runtimes, i) = bk_run;
	}
}

static void
rspamd_stat_backends_process (struct rspamd_stat_ctx *st_ctx,
		struct rspamd_task *task)
{
	guint i;
	struct rspamd_statfile *st;
	struct rspamd_classifier *cl;
	gpointer bk_run;

	g_assert (task->stat_runtimes != NULL);

	for (i = 0; i < st_ctx->statfiles->len; i++) {
		st = g_ptr_array_index (st_ctx->statfiles, i);
		cl = st->classifier;

		if (cl->cfg->flags & RSPAMD_FLAG_CLASSIFIER_NO_BACKEND) {
			continue;
		}

		bk_run = g_ptr_array_index (task->stat_runtimes, i);

		if (bk_run != NULL) {
			st->backend->process_tokens (task, task->tokens, i, bk_run);
		}
	}
}

static gboolean
rspamd_stat_backends_post_process (struct rspamd_stat_ctx *st_ctx,
		struct rspamd_task *task)
{
	guint i;
	struct rspamd_statfile *st;
	struct rspamd_classifier *cl;
	gpointer bk_run;

	g_assert (task->stat_runtimes != NULL);

	for (i = 0; i < st_ctx->statfiles->len; i++) {
		st = g_ptr_array_index (st_ctx->statfiles, i);
		cl = st->classifier;

		if (cl->cfg->flags & RSPAMD_FLAG_CLASSIFIER_NO_BACKEND) {
			continue;
		}

		bk_run = g_ptr_array_index (task->stat_runtimes, i);

		if (bk_run != NULL) {
			if (!st->backend->finalize_process (task, bk_run, st_ctx)) {
				return FALSE;
			}
		}
	}

	return TRUE;
}

static void
rspamd_stat_classifiers_process (struct rspamd_stat_ctx *st_ctx,
		struct rspamd_task *task)
{
	guint i, j, id;
	struct rspamd_classifier *cl;
	struct rspamd_statfile *st;
	gpointer bk_run;
	gboolean skip;

	if (st_ctx->classifiers->len == 0) {
		return;
	}

	/*
	 * Do not classify a message if some class is missing
	 */
	if (!(task->flags & RSPAMD_TASK_FLAG_HAS_SPAM_TOKENS)) {
		msg_info_task ("skip statistics as SPAM class is missing");

		return;
	}
	if (!(task->flags & RSPAMD_TASK_FLAG_HAS_HAM_TOKENS)) {
		msg_info_task ("skip statistics as HAM class is missing");

		return;
	}

	for (i = 0; i < st_ctx->classifiers->len; i++) {
		cl = g_ptr_array_index (st_ctx->classifiers, i);
		cl->spam_learns = 0;
		cl->ham_learns = 0;
	}

	for (i = 0; i < st_ctx->statfiles->len; i++) {
		st = g_ptr_array_index (st_ctx->statfiles, i);
		cl = st->classifier;

		if (cl->cfg->flags & RSPAMD_FLAG_CLASSIFIER_NO_BACKEND) {
			continue;
		}

		bk_run = g_ptr_array_index (task->stat_runtimes, i);
		g_assert (st != NULL);

		if (bk_run != NULL) {
			if (st->stcf->is_spam) {
				cl->spam_learns += st->backend->total_learns (task,
						bk_run,
						st_ctx);
			}
			else {
				cl->ham_learns += st->backend->total_learns (task,
						bk_run,
						st_ctx);
			}
		}
	}

	for (i = 0; i < st_ctx->classifiers->len; i++) {
		cl = g_ptr_array_index (st_ctx->classifiers, i);

		g_assert (cl != NULL);

		/* Ensure that all symbols enabled */
		skip = FALSE;

		if (!(cl->cfg->flags & RSPAMD_FLAG_CLASSIFIER_NO_BACKEND)) {
			for (j = 0; j < cl->statfiles_ids->len; j++) {
				id = g_array_index (cl->statfiles_ids, gint, j);
				bk_run =  g_ptr_array_index (task->stat_runtimes, id);
				st = g_ptr_array_index (st_ctx->statfiles, id);

				if (bk_run == NULL) {
					skip = TRUE;
					msg_debug_task ("disable classifier %s as statfile symbol %s is disabled",
							cl->cfg->name, st->stcf->symbol);
					break;
				}
			}
		}

		if (!skip) {
			if (cl->cfg->min_tokens > 0 && task->tokens->len < cl->cfg->min_tokens) {
				msg_debug_task (
						"<%s> contains less tokens than required for %s classifier: "
						"%ud < %ud",
						task->message_id,
						cl->cfg->name,
						task->tokens->len,
						cl->cfg->min_tokens);
				continue;
			}
			else if (cl->cfg->max_tokens > 0 && task->tokens->len > cl->cfg->max_tokens) {
				msg_debug_task (
						"<%s> contains more tokens than allowed for %s classifier: "
						"%ud > %ud",
						task->message_id,
						cl->cfg->name,
						task->tokens->len,
						cl->cfg->max_tokens);
				continue;
			}

			cl->subrs->classify_func (cl, task->tokens, task);
		}
	}
}

rspamd_stat_result_t
rspamd_stat_classify (struct rspamd_task *task, lua_State *L, guint stage,
		GError **err)
{
	struct rspamd_stat_ctx *st_ctx;
	rspamd_stat_result_t ret = RSPAMD_STAT_PROCESS_OK;

	st_ctx = rspamd_stat_get_ctx ();
	g_assert (st_ctx != NULL);

	if (st_ctx->classifiers->len == 0) {
		task->processed_stages |= stage;
		return ret;
	}

	if (stage == RSPAMD_TASK_STAGE_CLASSIFIERS_PRE) {
		/* Preprocess tokens */
		rspamd_stat_preprocess (st_ctx, task, FALSE);
	}
	else if (stage == RSPAMD_TASK_STAGE_CLASSIFIERS) {
		/* Process backends */
		rspamd_stat_backends_process (st_ctx, task);
	}
	else if (stage == RSPAMD_TASK_STAGE_CLASSIFIERS_POST) {
		/* Process classifiers */
		if (rspamd_stat_backends_post_process (st_ctx, task)) {
			rspamd_stat_classifiers_process (st_ctx, task);
		}
		/* Do not process classifiers on backend failures */
	}

	task->processed_stages |= stage;

	return ret;
}

static gboolean
rspamd_stat_cache_check (struct rspamd_stat_ctx *st_ctx,
		struct rspamd_task *task,
		 const gchar *classifier,
		 gboolean spam,
		 GError **err)
{
	rspamd_learn_t learn_res = RSPAMD_LEARN_OK;
	struct rspamd_classifier *cl, *sel = NULL;
	gpointer rt;
	guint i;

	/* Check whether we have learned that file */
	for (i = 0; i < st_ctx->classifiers->len; i ++) {
		cl = g_ptr_array_index (st_ctx->classifiers, i);

		/* Skip other classifiers if they are not needed */
		if (classifier != NULL && (cl->cfg->name == NULL ||
				g_ascii_strcasecmp (classifier, cl->cfg->name) != 0)) {
			continue;
		}

		sel = cl;

		if (sel->cache && sel->cachecf) {
			rt = cl->cache->runtime (task, sel->cachecf, FALSE);
			learn_res = cl->cache->check (task, spam, rt);
		}

		if (learn_res == RSPAMD_LEARN_INGORE) {
			/* Do not learn twice */
			g_set_error (err, rspamd_stat_quark (), 404, "<%s> has been already "
					"learned as %s, ignore it", task->message_id,
					spam ? "spam" : "ham");
			task->flags |= RSPAMD_TASK_FLAG_ALREADY_LEARNED;

			return FALSE;
		}
		else if (learn_res == RSPAMD_LEARN_UNLEARN) {
			task->flags |= RSPAMD_TASK_FLAG_UNLEARN;
			break;
		}
	}

	if (sel == NULL) {
		if (classifier) {
			g_set_error (err, rspamd_stat_quark (), 404, "cannot find classifier "
					"with name %s", classifier);
		}
		else {
			g_set_error (err, rspamd_stat_quark (), 404, "no classifiers defined");
		}

		return FALSE;
	}

	return TRUE;
}

static gboolean
rspamd_stat_classifiers_learn (struct rspamd_stat_ctx *st_ctx,
		struct rspamd_task *task,
		 const gchar *classifier,
		 gboolean spam,
		 GError **err)
{
	struct rspamd_classifier *cl, *sel = NULL;
	guint i;
	gboolean learned = FALSE, too_small = FALSE, too_large = FALSE,
			conditionally_skipped = FALSE;
	lua_State *L;
	struct rspamd_task **ptask;
	GList *cur;
	gint cb_ref;
	gchar *cond_str = NULL;

	if ((task->flags & RSPAMD_TASK_FLAG_ALREADY_LEARNED) && err != NULL &&
			*err == NULL) {
		/* Do not learn twice */
		g_set_error (err, rspamd_stat_quark (), 404, "<%s> has been already "
				"learned as %s, ignore it", task->message_id,
				spam ? "spam" : "ham");

		return FALSE;
	}

	/* Check whether we have learned that file */
	for (i = 0; i < st_ctx->classifiers->len; i ++) {
		cl = g_ptr_array_index (st_ctx->classifiers, i);

		/* Skip other classifiers if they are not needed */
		if (classifier != NULL && (cl->cfg->name == NULL ||
				g_ascii_strcasecmp (classifier, cl->cfg->name) != 0)) {
			continue;
		}

		sel = cl;

		/* Now check max and min tokens */
		if (cl->cfg->min_tokens > 0 && task->tokens->len < cl->cfg->min_tokens) {
			msg_info_task (
				"<%s> contains less tokens than required for %s classifier: "
						"%ud < %ud",
				task->message_id,
				cl->cfg->name,
				task->tokens->len,
				cl->cfg->min_tokens);
			too_small = TRUE;
			continue;
		}
		else if (cl->cfg->max_tokens > 0 && task->tokens->len > cl->cfg->max_tokens) {
			msg_info_task (
				"<%s> contains more tokens than allowed for %s classifier: "
						"%ud > %ud",
				task->message_id,
				cl->cfg->name,
				task->tokens->len,
				cl->cfg->max_tokens);
			too_large = TRUE;
			continue;
		}

		/* Check all conditions for this classifier */
		cur = cl->cfg->learn_conditions;
		L = task->cfg->lua_state;

		while (cur) {
			cb_ref = GPOINTER_TO_INT (cur->data);

			lua_settop (L, 0);
			lua_rawgeti (L, LUA_REGISTRYINDEX, cb_ref);
			/* Push task and two booleans: is_spam and is_unlearn */
			ptask = lua_newuserdata (L, sizeof (*ptask));
			*ptask = task;
			rspamd_lua_setclass (L, "rspamd{task}", -1);
			lua_pushboolean (L, spam);
			lua_pushboolean (L,
					task->flags & RSPAMD_TASK_FLAG_UNLEARN ? true : false);

			if (lua_pcall (L, 3, LUA_MULTRET, 0) != 0) {
				msg_err_task ("call to %s failed: %s",
						"condition callback",
						lua_tostring (L, -1));
			}
			else {
				if (lua_isboolean (L, 1)) {
					if (!lua_toboolean (L, 1)) {
						conditionally_skipped = TRUE;
						/* Also check for error string if needed */
						if (lua_isstring (L, 2)) {
							cond_str = rspamd_mempool_strdup (task->task_pool,
									lua_tostring (L, 2));
						}

						lua_settop (L, 0);
						break;
					}
				}
			}

			lua_settop (L, 0);
			cur = g_list_next (cur);
		}

		if (conditionally_skipped) {
			break;
		}

		if (cl->subrs->learn_spam_func (cl, task->tokens, task, spam,
				task->flags & RSPAMD_TASK_FLAG_UNLEARN, err)) {
			learned = TRUE;
		}
	}

	if (sel == NULL) {
		if (classifier) {
			g_set_error (err, rspamd_stat_quark (), 404, "cannot find classifier "
					"with name %s", classifier);
		}
		else {
			g_set_error (err, rspamd_stat_quark (), 404, "no classifiers defined");
		}

		return FALSE;
	}

	if (!learned && err && *err == NULL) {
		if (too_large) {
			g_set_error (err, rspamd_stat_quark (), 400,
					"<%s> contains more tokens than allowed for %s classifier: "
					"%d > %d",
					task->message_id,
					cl->cfg->name,
					task->tokens->len,
					cl->cfg->max_tokens);
		}
		else if (too_small) {
			g_set_error (err, rspamd_stat_quark (), 400,
					"<%s> contains less tokens than required for %s classifier: "
					"%d < %d",
					task->message_id,
					cl->cfg->name,
					task->tokens->len,
					cl->cfg->min_tokens);
		}
		else if (conditionally_skipped) {
			g_set_error (err, rspamd_stat_quark (), 410,
					"<%s> is skipped for %s classifier: "
					"%s",
					task->message_id,
					cl->cfg->name,
					cond_str ? cond_str : "unknown reason");
		}
	}

	return learned;
}

static gboolean
rspamd_stat_backends_learn (struct rspamd_stat_ctx *st_ctx,
		struct rspamd_task *task,
		 const gchar *classifier,
		 gboolean spam,
		 GError **err)
{
	struct rspamd_classifier *cl, *sel = NULL;
	struct rspamd_statfile *st;
	gpointer bk_run;
	guint i, j;
	gint id;
	gboolean res = FALSE;

	for (i = 0; i < st_ctx->classifiers->len; i ++) {
		cl = g_ptr_array_index (st_ctx->classifiers, i);

		/* Skip other classifiers if they are not needed */
		if (classifier != NULL && (cl->cfg->name == NULL ||
				g_ascii_strcasecmp (classifier, cl->cfg->name) != 0)) {
			continue;
		}

		if (cl->cfg->flags & RSPAMD_FLAG_CLASSIFIER_NO_BACKEND) {
			res = TRUE;
			continue;
		}

		sel = cl;

		for (j = 0; j < cl->statfiles_ids->len; j ++) {
			id = g_array_index (cl->statfiles_ids, gint, j);
			st = g_ptr_array_index (st_ctx->statfiles, id);
			bk_run = g_ptr_array_index (task->stat_runtimes, id);

			g_assert (st != NULL);

			if (bk_run == NULL) {
				/* XXX: must be error */
				continue;
			}

			if (!(task->flags & RSPAMD_TASK_FLAG_UNLEARN)) {
				if (!!spam != !!st->stcf->is_spam) {
					/* If we are not unlearning, then do not touch another class */
					continue;
				}
			}

			if (!st->backend->learn_tokens (task, task->tokens, id, bk_run)) {
				if (err && *err == NULL) {
					g_set_error (err, rspamd_stat_quark (), 500, "Cannot push "
							"learned results to the backend");
				}

				res = FALSE;
				goto end;
			}
			else {
				if (!!spam == !!st->stcf->is_spam) {
					st->backend->inc_learns (task, bk_run, st_ctx);
				}
				else if (task->flags & RSPAMD_TASK_FLAG_UNLEARN) {
					st->backend->dec_learns (task, bk_run, st_ctx);
				}

				res = TRUE;
			}
		}
	}

end:
	if (!res && err) {
		return res;
	}

	if (!res && sel == NULL) {
		if (classifier) {
			g_set_error (err, rspamd_stat_quark (), 404, "cannot find classifier "
					"with name %s", classifier);
		}
		else {
			g_set_error (err, rspamd_stat_quark (), 404, "no classifiers defined");
		}

		return FALSE;
	}

	if (!res) {
		g_set_error (err, rspamd_stat_quark (), 404, "cannot find statfile "
				"backend to learn %s in %s", spam ? "spam" : "ham",
				classifier ? classifier : "default classifier");
	}

	return res;
}

static gboolean
rspamd_stat_backends_post_learn (struct rspamd_stat_ctx *st_ctx,
		struct rspamd_task *task,
		const gchar *classifier,
		gboolean spam,
		GError **err)
{
	struct rspamd_classifier *cl;
	struct rspamd_statfile *st;
	gpointer bk_run, cache_run;
	guint i, j;
	gint id;
	gboolean res = TRUE;

	for (i = 0; i < st_ctx->classifiers->len; i ++) {
		cl = g_ptr_array_index (st_ctx->classifiers, i);

		/* Skip other classifiers if they are not needed */
		if (classifier != NULL && (cl->cfg->name == NULL ||
				g_ascii_strcasecmp (classifier, cl->cfg->name) != 0)) {
			continue;
		}

		if (cl->cfg->flags & RSPAMD_FLAG_CLASSIFIER_NO_BACKEND) {
			res = TRUE;
			continue;
		}

		for (j = 0; j < cl->statfiles_ids->len; j ++) {
			id = g_array_index (cl->statfiles_ids, gint, j);
			st = g_ptr_array_index (st_ctx->statfiles, id);
			bk_run = g_ptr_array_index (task->stat_runtimes, id);

			g_assert (st != NULL);

			if (bk_run == NULL) {
				/* XXX: must be error */
				continue;
			}

			if (!st->backend->finalize_learn (task, bk_run, st_ctx, err)) {
				return RSPAMD_STAT_PROCESS_ERROR;
			}
		}

		if (cl->cache) {
			cache_run = cl->cache->runtime (task, cl->cachecf, TRUE);
			cl->cache->learn (task, spam, cache_run);
		}
	}

	g_atomic_int_add (&task->worker->srv->stat->messages_learned, 1);

	return res;
}

rspamd_stat_result_t
rspamd_stat_learn (struct rspamd_task *task,
		gboolean spam, lua_State *L, const gchar *classifier, guint stage,
		GError **err)
{
	struct rspamd_stat_ctx *st_ctx;
	rspamd_stat_result_t ret = RSPAMD_STAT_PROCESS_OK;

	/*
	 * We assume now that a task has been already classified before
	 * coming to learn
	 */
	g_assert (RSPAMD_TASK_IS_CLASSIFIED (task));

	st_ctx = rspamd_stat_get_ctx ();
	g_assert (st_ctx != NULL);

	if (st_ctx->classifiers->len == 0) {
		task->processed_stages |= stage;
		return ret;
	}

	if (stage == RSPAMD_TASK_STAGE_LEARN_PRE) {
		/* Process classifiers */
		if (!rspamd_stat_cache_check (st_ctx, task, classifier, spam, err)) {
			return RSPAMD_STAT_PROCESS_ERROR;
		}
	}
	else if (stage == RSPAMD_TASK_STAGE_LEARN) {
		/* Process classifiers */
		if (!rspamd_stat_classifiers_learn (st_ctx, task, classifier,
				spam, err)) {
			return RSPAMD_STAT_PROCESS_ERROR;
		}

		/* Process backends */
		if (!rspamd_stat_backends_learn (st_ctx, task, classifier, spam, err)) {
			return RSPAMD_STAT_PROCESS_ERROR;
		}
	}
	else if (stage == RSPAMD_TASK_STAGE_LEARN_POST) {
		if (!rspamd_stat_backends_post_learn (st_ctx, task, classifier, spam, err)) {
			return RSPAMD_STAT_PROCESS_ERROR;
		}
	}

	task->processed_stages |= stage;

	return ret;
}

static gboolean
rspamd_stat_has_classifier_symbols (struct rspamd_task *task,
		struct rspamd_metric_result *mres,
		struct rspamd_classifier *cl)
{
	guint i;
	gint id;
	struct rspamd_statfile *st;
	struct rspamd_stat_ctx *st_ctx;
	gboolean is_spam;

	if (mres == NULL) {
		return FALSE;
	}

	st_ctx = rspamd_stat_get_ctx ();
	is_spam = !!(task->flags & RSPAMD_TASK_FLAG_LEARN_SPAM);

	for (i = 0; i < cl->statfiles_ids->len; i ++) {
		id = g_array_index (cl->statfiles_ids, gint, i);
		st = g_ptr_array_index (st_ctx->statfiles, id);

		if (rspamd_task_find_symbol_result (task, st->stcf->symbol)) {
			if (is_spam == !!st->stcf->is_spam) {
				msg_debug_task ("do not autolearn %s as symbol %s is already "
						"added", is_spam ? "spam" : "ham", st->stcf->symbol);

				return TRUE;
			}
		}
	}

	return FALSE;
}

gboolean
rspamd_stat_check_autolearn (struct rspamd_task *task)
{
	struct rspamd_stat_ctx *st_ctx;
	struct rspamd_classifier *cl;
	const ucl_object_t *obj, *elt1, *elt2;
	struct rspamd_metric_result *mres = NULL;
	struct rspamd_task **ptask;
	lua_State *L;
	GString *tb;
	guint i;
	gint err_idx;
	gboolean ret = FALSE;
	gdouble ham_score, spam_score;
	const gchar *lua_script, *lua_ret;

	g_assert (RSPAMD_TASK_IS_CLASSIFIED (task));
	st_ctx = rspamd_stat_get_ctx ();
	g_assert (st_ctx != NULL);

	for (i = 0; i < st_ctx->classifiers->len; i ++) {
		cl = g_ptr_array_index (st_ctx->classifiers, i);
		ret = FALSE;

		if (cl->cfg->opts) {
			obj = ucl_object_lookup (cl->cfg->opts, "autolearn");

			if (ucl_object_type (obj) == UCL_BOOLEAN) {
				if (ucl_object_toboolean (obj)) {
					/*
					 * Default learning algorithm:
					 *
					 * - We learn spam if action is ACTION_REJECT
					 * - We learn ham if score is less than zero
					 */
					mres = task->result;

					if (mres) {
						if (mres->score > rspamd_task_get_required_score (task, mres)) {
							task->flags |= RSPAMD_TASK_FLAG_LEARN_SPAM;

							ret = TRUE;
						}
						else if (mres->score < 0) {
							task->flags |= RSPAMD_TASK_FLAG_LEARN_HAM;
							ret = TRUE;
						}
					}
				}
			}
			else if (ucl_object_type (obj) == UCL_ARRAY && obj->len == 2) {
				/*
				 * We have an array of 2 elements, treat it as a
				 * ham_score, spam_score
				 */
				elt1 = ucl_array_find_index (obj, 0);
				elt2 = ucl_array_find_index (obj, 1);

				if ((ucl_object_type (elt1) == UCL_FLOAT ||
						ucl_object_type (elt1) == UCL_INT) &&
					(ucl_object_type (elt2) == UCL_FLOAT ||
						ucl_object_type (elt2) == UCL_INT)) {
					ham_score = ucl_object_todouble (elt1);
					spam_score = ucl_object_todouble (elt2);

					if (ham_score > spam_score) {
						gdouble t;

						t = ham_score;
						ham_score = spam_score;
						spam_score = t;
					}

					mres = task->result;

					if (mres) {
						if (mres->score >= spam_score) {
							task->flags |= RSPAMD_TASK_FLAG_LEARN_SPAM;

							ret = TRUE;
						}
						else if (mres->score <= ham_score) {
							task->flags |= RSPAMD_TASK_FLAG_LEARN_HAM;
							ret = TRUE;
						}
					}
				}
			}
			else if (ucl_object_type (obj) == UCL_STRING) {
				lua_script = ucl_object_tostring (obj);
				L = task->cfg->lua_state;

				if (luaL_dostring (L, lua_script) != 0) {
					msg_err_task ("cannot execute lua script for autolearn "
							"extraction: %s", lua_tostring (L, -1));
				}
				else {
					if (lua_type (L, -1) == LUA_TFUNCTION) {
						lua_pushcfunction (L, &rspamd_lua_traceback);
						err_idx = lua_gettop (L);
						lua_pushvalue (L, -2); /* Function itself */

						ptask = lua_newuserdata (L, sizeof (struct rspamd_task *));
						*ptask = task;
						rspamd_lua_setclass (L, "rspamd{task}", -1);

						if (lua_pcall (L, 1, 1, err_idx) != 0) {
							tb = lua_touserdata (L, -1);
							msg_err_task ("call to autolearn script failed: "
									"%v", tb);
							g_string_free (tb, TRUE);
						}
						else {
							lua_ret = lua_tostring (L, -1);

							if (lua_ret) {
								if (strcmp (lua_ret, "ham") == 0) {
									task->flags |= RSPAMD_TASK_FLAG_LEARN_HAM;
									ret = TRUE;
								}
								else if (strcmp (lua_ret, "spam") == 0) {
									task->flags |= RSPAMD_TASK_FLAG_LEARN_SPAM;
									ret = TRUE;
								}
							}
						}

						/* Result + error function + original function */
						lua_pop (L, 3);
					}
					else {
						msg_err_task ("lua script must return "
								"function(task) and not %s",
								lua_typename (L, lua_type (
										L, -1)));
					}
				}
			}

			if (ret) {
				/* Do not autolearn if we have this symbol already */
				if (rspamd_stat_has_classifier_symbols (task, mres, cl)) {
					ret = FALSE;
					task->flags &= ~(RSPAMD_TASK_FLAG_LEARN_HAM |
							RSPAMD_TASK_FLAG_LEARN_SPAM);
				}
				else if (mres != NULL) {
					if (task->flags & RSPAMD_TASK_FLAG_LEARN_HAM) {
						msg_info_task ("<%s>: autolearn ham for classifier "
								"'%s' as message's "
								"score is negative: %.2f",
								task->message_id, cl->cfg->name,
								mres->score);
					}
					else {
						msg_info_task ("<%s>: autolearn spam for classifier "
								"'%s' as message's "
								"action is reject, score: %.2f",
								task->message_id, cl->cfg->name,
								mres->score);
					}

					task->classifier = cl->cfg->name;
					break;
				}
			}
		}
	}

	return ret;
}

/**
 * Get the overall statistics for all statfile backends
 * @param cfg configuration
 * @param total_learns the total number of learns is stored here
 * @return array of statistical information
 */
rspamd_stat_result_t
rspamd_stat_statistics (struct rspamd_task *task,
		struct rspamd_config *cfg,
		guint64 *total_learns,
		ucl_object_t **target)
{
	struct rspamd_stat_ctx *st_ctx;
	struct rspamd_classifier *cl;
	struct rspamd_statfile *st;
	gpointer backend_runtime;
	ucl_object_t *res = NULL, *elt;
	guint64 learns = 0;
	guint i, j;
	gint id;

	st_ctx = rspamd_stat_get_ctx ();
	g_assert (st_ctx != NULL);

	res = ucl_object_typed_new (UCL_ARRAY);

	for (i = 0; i < st_ctx->classifiers->len; i ++) {
		cl = g_ptr_array_index (st_ctx->classifiers, i);

		if (cl->cfg->flags & RSPAMD_FLAG_CLASSIFIER_NO_BACKEND) {
			continue;
		}

		for (j = 0; j < cl->statfiles_ids->len; j ++) {
			id = g_array_index (cl->statfiles_ids, gint, j);
			st = g_ptr_array_index (st_ctx->statfiles, id);
			backend_runtime = st->backend->runtime (task, st->stcf, FALSE,
					st->bkcf);
			elt = st->backend->get_stat (backend_runtime, st->bkcf);

			if (elt && ucl_object_type (elt) == UCL_OBJECT) {
				const ucl_object_t *rev = ucl_object_lookup (elt, "revision");

				learns += ucl_object_toint (rev);
			}
			else {
				learns += st->backend->total_learns (task, backend_runtime,
						st->bkcf);
			}

			if (elt != NULL) {
				ucl_array_append (res, elt);
			}
		}
	}

	if (total_learns != NULL) {
		*total_learns = learns;
	}

	if (target) {
		*target = res;
	}

	return RSPAMD_STAT_PROCESS_OK;
}
