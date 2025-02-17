/* -*- mode: c -*- */

/* Copyright (C) 2002-2022 Alexander Chernov <cher@ejudge.ru> */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define YYSTYPE struct filter_tree *

#include "ejudge/filter_tree.h"
#include "ejudge/filter_expr.h"
#include "ejudge/filter_eval.h"
#include "ejudge/teamdb.h"
#include "ejudge/userlist.h"
#include "ejudge/archive_paths.h"
#include "ejudge/ej_uuid.h"
#include "ejudge/prepare_dflt.h"
#include "ejudge/testing_report_xml.h"
#include "ejudge/fileutl.h"
#include "ejudge/misctext.h"

#include "ejudge/logger.h"
#include "ejudge/mempage.h"

static unsigned char *envdup(struct filter_env *env,
                             unsigned char const *str)
{
  int len;
  unsigned char *s;

  if (!str) str = "";
  len = strlen(str);
  s = filter_tree_alloc(env->mem, len + 1);
  memcpy(s, str, len);
  return s;
}

/* FIXME: dumb :( */
static int
is_latest(struct filter_env *env, int rid)
{
  int r;

  if (rid < env->rbegin || rid >= env->rtotal) return 0;
  switch (env->rentries[rid].status) {
  case RUN_OK:
  case RUN_PARTIAL:
  case RUN_ACCEPTED:
  case RUN_PENDING_REVIEW:
  case RUN_SUMMONED:
    break;
  default:
    return 0;
  }
  for (r = rid + 1; r < env->rtotal; r++) {
    if (!run_is_normal_status(env->rentries[r].status)) continue;
    if (env->rentries[rid].user_id != env->rentries[r].user_id
        || env->rentries[rid].prob_id != env->rentries[r].prob_id)
      continue;
    switch (env->rentries[r].status) {
    case RUN_OK:
    case RUN_PARTIAL:
    case RUN_ACCEPTED:
    case RUN_PENDING_REVIEW:
    case RUN_SUMMONED:
      return 0;
    }
  }
  return 1;
}

/* FIXME: dumb :( */
static int
is_latestmarked(struct filter_env *env, int rid)
{
  int r;

  if (rid < env->rbegin || rid >= env->rtotal) return 0;
  if (!env->rentries[rid].is_marked) return 0;
  for (r = rid + 1; r < env->rtotal; r++) {
    if (env->rentries[rid].user_id == env->rentries[r].user_id
        && env->rentries[rid].prob_id == env->rentries[r].prob_id
        && env->rentries[r].is_marked) return 0;
  }
  return 1;
}

/* FIXME: dumb :( */
static int
is_afterok(struct filter_env *env, int rid)
{
  int r;

  if (rid < env->rbegin || rid >= env->rtotal) return 0;
  if (env->rentries[rid].status >= RUN_PSEUDO_FIRST
      && env->rentries[rid].status <= RUN_PSEUDO_LAST)
    return 0;
  for (r = rid - 1; r >= 0; r--) {
    if (env->rentries[r].status != RUN_OK) continue;
    if (env->rentries[rid].user_id != env->rentries[r].user_id
        || env->rentries[rid].prob_id != env->rentries[r].prob_id)
      continue;
    return 1;
  }
  return 0;
}

static int
is_missing_source(
        struct filter_env *env,
        const struct run_entry *re)
{
  serve_state_t cs = 0;
  struct section_global_data *g = 0;
  int src_flags;
  path_t src_path;

  if (!env || !(cs = env->serve_state) || !(g = cs->global)) return 0;

  if (!run_is_normal_or_transient_status(re->status)) return 0;

  if (re->store_flags == STORE_FLAGS_UUID) {
    if ((src_flags = uuid_archive_make_read_path(cs, src_path, sizeof(src_path),
                                                 &re->run_uuid, DFLT_R_UUID_SOURCE, 0)) < 0)
      return 1;
  } else {
    if ((src_flags = archive_make_read_path(cs, src_path, sizeof(src_path),
                                            g->run_archive_dir,
                                            re->run_id, 0, 1)) < 0)
      return 1;
  }
  return 0;
}

static int
has_test_result(
        struct filter_env *env,
        const struct run_entry *re,
        int result)
{
  int retval = 0;
  serve_state_t cs = 0;
  struct section_global_data *g = 0;
  int rep_flag;
  path_t rep_path;
  char *rep_txt = 0;
  size_t rep_len = 0;
  const unsigned char *start_ptr = 0;
  testing_report_xml_t rep_xml = 0;

  if (!env || !(cs = env->serve_state) || !(g = cs->global)) goto cleanup;
  if (!run_is_normal_or_transient_status(re->status)) goto cleanup;

  rep_flag = serve_make_xml_report_read_path(cs, rep_path, sizeof(rep_path), re);
  if (rep_flag < 0) goto cleanup;
  if (re->store_flags == STORE_FLAGS_UUID_BSON) {
    if (!(rep_xml = testing_report_parse_bson_file(rep_path))) goto cleanup;
  } else {
    if (generic_read_file(&rep_txt, 0, &rep_len, rep_flag, 0, rep_path, 0) < 0) goto cleanup;
    if (get_content_type(rep_txt, &start_ptr) != CONTENT_TYPE_XML) goto cleanup;
    if (!(rep_xml = testing_report_parse_xml(start_ptr))) goto cleanup;
  }
  if (rep_xml->run_tests <= 0) goto cleanup;
  for (int i = 0; i < rep_xml->run_tests; ++i) {
    const struct testing_report_test *rep_tst = rep_xml->tests[i];
    if (rep_tst && rep_tst->status == result) {
      retval = 1;
      break;
    }
  }

cleanup:;
  testing_report_free(rep_xml);
  xfree(rep_txt);
  return retval;
}

static int
find_user_group(struct filter_env *env, const unsigned char *group_name)
{
  int i;

  if (!group_name || !*group_name) return FILTER_ERR_INV_USERGROUP;
  for (i = 0; i < env->serve_state->user_group_count; ++i) {
    if (!strcmp(env->serve_state->user_groups[i].group_name, group_name))
      return i;
  }
  return FILTER_ERR_INV_USERGROUP;
}

static int
check_user_group(struct filter_env *env, int user_id, int group_ind)
{
  serve_state_t cs = env->serve_state;
  int user_ind;
  unsigned int *b;

  if (user_id <= 0 || user_id >= cs->group_member_map_size) return 0;
  user_ind = cs->group_member_map[user_id];
  if (user_ind < 0 || user_ind >= cs->group_member_count) return 0;
  if (group_ind < 0 || group_ind >= cs->user_group_count) return 0;
  if (!(b = cs->group_members[user_ind].group_bitmap)) return 0;
  if ((b[group_ind >> 5] & (1U << (group_ind & 0x1F)))) return 1;
  return 0;
}

static int
do_eval(struct filter_env *env,
        struct filter_tree *t,
        struct filter_tree *res)
{
  int c;
  struct filter_tree r1, r2;
  int lang_id, prob_id, user_id, flags;
  const struct userlist_user *u;
  const struct userlist_member *m;
  const unsigned char *s;

  memset(res, 0, sizeof(*res));
  switch (t->kind) {
  case TOK_LOGOR:
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    if ((c = do_eval(env, t->v.t[0], &r1)) < 0) return c;
    ASSERT(r1.kind == TOK_BOOL_L);
    if (!r1.v.b) {
      if ((c = do_eval(env, t->v.t[1], &r2)) < 0) return c;
      ASSERT(r2.kind == TOK_BOOL_L);
      res->v.b = r2.v.b;
    } else {
      res->v.b = 1;
    }
    break;

  case TOK_LOGAND:
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    if ((c = do_eval(env, t->v.t[0], &r1)) < 0) return c;
    ASSERT(r1.kind == TOK_BOOL_L);
    if (r1.v.b) {
      if ((c = do_eval(env, t->v.t[1], &r2)) < 0) return c;
      ASSERT(r2.kind == TOK_BOOL_L);
      res->v.b = r2.v.b;
    } else {
      res->v.b = 0;
    }
    break;

    /* binary */
  case '^':
  case '|':
  case '&':
  case '*':
  case '/':
  case '%':
  case '+':
  case '-':
  case '>':
  case '<':
  case TOK_EQ:
  case TOK_NE:
  case TOK_LE:
  case TOK_GE:
  case TOK_ASL:
  case TOK_ASR:
  case TOK_REGEXP:
    if ((c = do_eval(env, t->v.t[0], &r1)) < 0) return c;
    if ((c = do_eval(env, t->v.t[1], &r2)) < 0) return c;
    return filter_tree_eval_node(env->mem, t->kind, res, &r1, &r2);

    /* unary */
  case '~':
  case '!':
  case TOK_UN_MINUS:
    if ((c = do_eval(env, t->v.t[0], &r1)) < 0) return c;
    return filter_tree_eval_node(env->mem, t->kind, res, &r1, 0);

  case TOK_TIME:
  case TOK_DUR:
  case TOK_SIZE:
  case TOK_HASH:
  case TOK_UUID:
  case TOK_IP:
  case TOK_PROB:
  case TOK_UID:
  case TOK_LOGIN:
  case TOK_NAME:
  case TOK_GROUP:
  case TOK_LANG:
  case TOK_ARCH:
  case TOK_RESULT:
  case TOK_SCORE:
  case TOK_SCORE_ADJ:
  case TOK_TEST:
  case TOK_IMPORTED:
  case TOK_HIDDEN:
  case TOK_READONLY:
  case TOK_MARKED:
  case TOK_SAVED:
  case TOK_VARIANT:
  case TOK_RAWVARIANT:
  case TOK_USERINVISIBLE:
  case TOK_USERBANNED:
  case TOK_USERLOCKED:
  case TOK_USERINCOMPLETE:
  case TOK_USERDISQUALIFIED:
  case TOK_USERPRIVILEGED:
  case TOK_USERREG_READONLY:
  case TOK_LATEST:
  case TOK_LATESTMARKED:
  case TOK_AFTEROK:
  case TOK_EXAMINABLE:
  case TOK_CYPHER:
  case TOK_MISSINGSOURCE:
  case TOK_JUDGE_ID:
  case TOK_PASSED_MODE:
  case TOK_EOLN_TYPE:
  case TOK_STORE_FLAGS:
  case TOK_TOKEN_FLAGS:
  case TOK_TOKEN_COUNT:
    if ((c = do_eval(env, t->v.t[0], &r1)) < 0) return c;
    ASSERT(r1.kind == TOK_INT_L);
    if (r1.v.i < 0) r1.v.i = env->rtotal + r1.v.i;
    if (r1.v.i >= env->rtotal) return -FILTER_ERR_RANGE;
    if (r1.v.i < env->rbegin) return -FILTER_ERR_RANGE;
    switch (t->kind) {
    case TOK_TIME:
      res->kind = TOK_TIME_L;
      res->type = FILTER_TYPE_TIME;
      res->v.a = env->rentries[r1.v.i].time;
      break;
    case TOK_DUR:
      res->kind = TOK_DUR_L;
      res->type = FILTER_TYPE_DUR;
      res->v.u = env->rentries[r1.v.i].time - env->rhead.start_time;
      break;
    case TOK_SIZE:
      res->kind = TOK_SIZE_L;
      res->type = FILTER_TYPE_SIZE;
      res->v.z = env->rentries[r1.v.i].size;
      break;
    case TOK_HASH:
      res->kind = TOK_HASH_L;
      res->type = FILTER_TYPE_HASH;
      memcpy(res->v.h, env->rentries[r1.v.i].h.sha1, sizeof(env->cur->h.sha1));
      break;
    case TOK_UUID:
      res->kind = TOK_STRING_L;
      res->type = FILTER_TYPE_STRING;
      res->v.s = envdup(env, ej_uuid_unparse(&env->rentries[r1.v.i].run_uuid, ""));
      break;
    case TOK_IP:
      res->kind = TOK_IP_L;
      res->type = FILTER_TYPE_IP;
      run_entry_to_ipv6(&env->rentries[r1.v.i], &res->v.p);
      break;
    case TOK_PROB:
      res->kind = TOK_STRING_L;
      res->type = FILTER_TYPE_STRING;
      prob_id = env->rentries[r1.v.i].prob_id;
      if (prob_id <= 0 || prob_id > env->maxprob || !env->probs[prob_id]) {
        res->v.s = envdup(env, "");
      } else {
        res->v.s = envdup(env, env->probs[prob_id]->short_name);
      }
      break;
    case TOK_UID:
      res->kind = TOK_INT_L;
      res->type = FILTER_TYPE_INT;
      res->v.i = env->rentries[r1.v.i].user_id;
      break;
    case TOK_LOGIN:
      res->kind = TOK_STRING_L;
      res->type = FILTER_TYPE_STRING;
      user_id = env->rentries[r1.v.i].user_id;
      if (!user_id) {
        res->v.s = envdup(env, "");
      } else {
        res->v.s = envdup(env, teamdb_get_login(env->teamdb_state, user_id));
      }
      break;
    case TOK_NAME:
      res->kind = TOK_STRING_L;
      res->type = FILTER_TYPE_STRING;
      user_id = env->rentries[r1.v.i].user_id;
      if (!user_id) {
        res->v.s = envdup(env, "");
      } else {
        res->v.s = envdup(env, teamdb_get_name(env->teamdb_state, user_id));
      }
      break;
    case TOK_GROUP:
      res->kind = TOK_STRING_L;
      res->type = FILTER_TYPE_STRING;
      user_id = env->rentries[r1.v.i].user_id;
      if (user_id > 0
          && (u = teamdb_get_userlist(env->teamdb_state, user_id))
          && u->cnts0
          && (m = userlist_members_get_first(u->cnts0->members))) {
        res->v.s = envdup(env, m->group);
      } else {
        res->v.s = envdup(env, "");
      }
      break;
    case TOK_LANG:
      res->kind = TOK_STRING_L;
      res->type = FILTER_TYPE_STRING;
      lang_id = env->rentries[r1.v.i].lang_id;
      if (lang_id <= 0 || lang_id > env->maxlang || !env->langs[lang_id]) {
        res->v.s = envdup(env, "");
      } else {
        res->v.s = envdup(env, env->langs[lang_id]->short_name);
      }
      break;
    case TOK_ARCH:
      res->kind = TOK_STRING_L;
      res->type = FILTER_TYPE_STRING;
      lang_id = env->rentries[r1.v.i].lang_id;
      if (lang_id <= 0 || lang_id > env->maxlang || !env->langs[lang_id] || !env->langs[lang_id]->arch) {
        res->v.s = envdup(env, "");
      } else {
        res->v.s = envdup(env, env->langs[lang_id]->arch);
      }
      break;
    case TOK_RESULT:
      res->kind = TOK_RESULT_L;
      res->type = FILTER_TYPE_RESULT;
      res->v.r = env->rentries[r1.v.i].status;
      break;
    case TOK_SCORE:
      res->kind = TOK_INT_L;
      res->type = FILTER_TYPE_INT;
      res->v.i = env->rentries[r1.v.i].score;
      break;
    case TOK_SCORE_ADJ:
      res->kind = TOK_INT_L;
      res->type = FILTER_TYPE_INT;
      res->v.i = env->rentries[r1.v.i].score_adj;
      break;
    case TOK_TEST:
      res->kind = TOK_INT_L;
      res->type = FILTER_TYPE_INT;
      res->v.i = env->rentries[r1.v.i].test;
      break;
    case TOK_IMPORTED:
      res->kind = TOK_BOOL_L;
      res->type = FILTER_TYPE_BOOL;
      res->v.b = env->rentries[r1.v.i].is_imported;
      break;
    case TOK_HIDDEN:
      res->kind = TOK_BOOL_L;
      res->type = FILTER_TYPE_BOOL;
      res->v.b = env->rentries[r1.v.i].is_hidden;
      break;
    case TOK_READONLY:
      res->kind = TOK_BOOL_L;
      res->type = FILTER_TYPE_BOOL;
      res->v.b = env->rentries[r1.v.i].is_readonly;
      break;
    case TOK_MARKED:
      res->kind = TOK_BOOL_L;
      res->type = FILTER_TYPE_BOOL;
      res->v.b = env->rentries[r1.v.i].is_marked;
      break;
    case TOK_SAVED:
      res->kind = TOK_BOOL_L;
      res->type = FILTER_TYPE_BOOL;
      res->v.b = env->rentries[r1.v.i].is_saved;
      break;
    case TOK_VARIANT:
      res->kind = TOK_INT_L;
      res->type = FILTER_TYPE_INT;
      c = env->rentries[r1.v.i].variant;
      if (!c) {
        c = find_variant(env->serve_state, env->rentries[r1.v.i].user_id,
                         env->rentries[r1.v.i].prob_id, 0);
      }
      res->v.i = c;
      break;
    case TOK_RAWVARIANT:
      res->kind = TOK_INT_L;
      res->type = FILTER_TYPE_INT;
      c = env->rentries[r1.v.i].variant;
      res->v.i = c;
      break;
    case TOK_USERINVISIBLE:
      res->kind = TOK_BOOL_L;
      res->type = FILTER_TYPE_BOOL;
      user_id = env->rentries[r1.v.i].user_id;
      if (!user_id) {
        res->v.b = 0;
      } else if ((flags = teamdb_get_flags(env->teamdb_state, user_id)) < 0) {
        res->v.b = 0;
      } else if ((flags & TEAM_INVISIBLE)) {
        res->v.b = 1;
      } else {
        res->v.b = 0;
      }
      break;
    case TOK_USERBANNED:
      res->kind = TOK_BOOL_L;
      res->type = FILTER_TYPE_BOOL;
      user_id = env->rentries[r1.v.i].user_id;
      if (!user_id) {
        res->v.b = 0;
      } else if ((flags = teamdb_get_flags(env->teamdb_state, user_id)) < 0) {
        res->v.b = 0;
      } else if ((flags & TEAM_BANNED)) {
        res->v.b = 1;
      } else {
        res->v.b = 0;
      }
      break;
    case TOK_USERLOCKED:
      res->kind = TOK_BOOL_L;
      res->type = FILTER_TYPE_BOOL;
      user_id = env->rentries[r1.v.i].user_id;
      if (!user_id) {
        res->v.b = 0;
      } else if ((flags = teamdb_get_flags(env->teamdb_state, user_id)) < 0) {
        res->v.b = 0;
      } else if ((flags & TEAM_LOCKED)) {
        res->v.b = 1;
      } else {
        res->v.b = 0;
      }
      break;
    case TOK_USERINCOMPLETE:
      res->kind = TOK_BOOL_L;
      res->type = FILTER_TYPE_BOOL;
      user_id = env->rentries[r1.v.i].user_id;
      if (!user_id) {
        res->v.b = 0;
      } else if ((flags = teamdb_get_flags(env->teamdb_state, user_id)) < 0) {
        res->v.b = 0;
      } else if ((flags & TEAM_INCOMPLETE)) {
        res->v.b = 1;
      } else {
        res->v.b = 0;
      }
      break;
    case TOK_USERDISQUALIFIED:
      res->kind = TOK_BOOL_L;
      res->type = FILTER_TYPE_BOOL;
      user_id = env->rentries[r1.v.i].user_id;
      if (!user_id) {
        res->v.b = 0;
      } else if ((flags = teamdb_get_flags(env->teamdb_state, user_id)) < 0) {
        res->v.b = 0;
      } else if ((flags & TEAM_DISQUALIFIED)) {
        res->v.b = 1;
      } else {
        res->v.b = 0;
      }
      break;
    case TOK_USERPRIVILEGED:
      res->kind = TOK_BOOL_L;
      res->type = FILTER_TYPE_BOOL;
      user_id = env->rentries[r1.v.i].user_id;
      if (!user_id) {
        res->v.b = 0;
      } else if ((flags = teamdb_get_flags(env->teamdb_state, user_id)) < 0) {
        res->v.b = 0;
      } else if ((flags & TEAM_PRIVILEGED)) {
        res->v.b = 1;
      } else {
        res->v.b = 0;
      }
      break;
    case TOK_USERREG_READONLY:
      res->kind = TOK_BOOL_L;
      res->type = FILTER_TYPE_BOOL;
      user_id = env->rentries[r1.v.i].user_id;
      if (!user_id) {
        res->v.b = 0;
      } else if ((flags = teamdb_get_flags(env->teamdb_state, user_id)) < 0) {
        res->v.b = 0;
      } else if ((flags & TEAM_REG_READONLY)) {
        res->v.b = 1;
      } else {
        res->v.b = 0;
      }
      break;
    case TOK_LATEST:
      res->kind = TOK_BOOL_L;
      res->type = FILTER_TYPE_BOOL;
      res->v.b = is_latest(env, r1.v.i);
      break;
    case TOK_LATESTMARKED:
      res->kind = TOK_BOOL_L;
      res->type = FILTER_TYPE_BOOL;
      res->v.b = is_latestmarked(env, r1.v.i);
      break;
    case TOK_AFTEROK:
      res->kind = TOK_BOOL_L;
      res->type = FILTER_TYPE_BOOL;
      res->v.b = is_afterok(env, r1.v.i);
      break;
    case TOK_EXAMINABLE:
      res->kind = TOK_BOOL_L;
      res->type = FILTER_TYPE_BOOL;
      //res->v.b = env->rentries[r1.v.i].is_examinable;
      res->v.b = 0;
      break;
    case TOK_CYPHER:
      res->kind = TOK_STRING_L;
      res->type = FILTER_TYPE_STRING;
      user_id = env->rentries[r1.v.i].user_id;
      u = 0; s = 0;
      if (user_id > 0) u = teamdb_get_userlist(env->teamdb_state, user_id);
      if (u && u->cnts0) s = u->cnts0->exam_cypher;
      res->v.s = envdup(env, s);
      break;
    case TOK_MISSINGSOURCE:
      res->kind = TOK_BOOL_L;
      res->type = FILTER_TYPE_BOOL;
      res->v.b = is_missing_source(env, &env->rentries[r1.v.i]);
      break;
    case TOK_JUDGE_ID:
      res->kind = TOK_INT_L;
      res->type = FILTER_TYPE_INT;
      res->v.i = env->rentries[r1.v.i].j.judge_id;
      break;
    case TOK_PASSED_MODE:
      res->kind = TOK_BOOL_L;
      res->type = FILTER_TYPE_BOOL;
      res->v.i = !!env->rentries[r1.v.i].passed_mode;
      break;
    case TOK_EOLN_TYPE:
      res->kind = TOK_INT_L;
      res->type = FILTER_TYPE_INT;
      res->v.i = !!env->rentries[r1.v.i].eoln_type;
      break;
    case TOK_STORE_FLAGS:
      res->kind = TOK_INT_L;
      res->type = FILTER_TYPE_INT;
      res->v.i = !!env->rentries[r1.v.i].store_flags;
      break;
    case TOK_TOKEN_FLAGS:
      res->kind = TOK_INT_L;
      res->type = FILTER_TYPE_INT;
      res->v.i = !!env->rentries[r1.v.i].token_flags;
      break;
    case TOK_TOKEN_COUNT:
      res->kind = TOK_INT_L;
      res->type = FILTER_TYPE_INT;
      res->v.i = !!env->rentries[r1.v.i].token_count;
      break;
    default:
      abort();
    }
    break;

  case TOK_INT:
  case TOK_STRING:
  case TOK_BOOL:
  case TOK_TIME_T:
  case TOK_DUR_T:
  case TOK_SIZE_T:
  case TOK_RESULT_T:
  case TOK_HASH_T:
  case TOK_IP_T:
    if ((c = do_eval(env, t->v.t[0], &r1)) < 0) return c;
    return filter_tree_eval_node(env->mem, t->kind, res, &r1, 0);

    /* variables */
  case TOK_ID:
    res->kind = TOK_INT_L;
    res->type = FILTER_TYPE_INT;
    res->v.i = env->rid;
    break;
  case TOK_CURTIME:
    res->kind = TOK_TIME_L;
    res->type = FILTER_TYPE_TIME;
    res->v.a = env->cur->time;
    break;
  case TOK_CURDUR:
    res->kind = TOK_DUR_L;
    res->type = FILTER_TYPE_DUR;
    res->v.u = env->cur->time - env->rhead.start_time;
    break;
  case TOK_CURSIZE:
    res->kind = TOK_SIZE_L;
    res->type = FILTER_TYPE_SIZE;
    res->v.z = env->cur->size;
    break;
  case TOK_CURHASH:
    res->kind = TOK_HASH_L;
    res->type = FILTER_TYPE_HASH;
    memcpy(res->v.h, env->cur->h.sha1, sizeof(env->cur->h.sha1));
    break;
  case TOK_CURUUID:
    res->kind = TOK_STRING_L;
    res->type = FILTER_TYPE_STRING;
    res->v.s = envdup(env, ej_uuid_unparse(&env->cur->run_uuid, ""));
    break;
  case TOK_CURIP:
    res->kind = TOK_IP_L;
    res->type = FILTER_TYPE_IP;
    run_entry_to_ipv6(env->cur, &res->v.p);
    break;
  case TOK_CURPROB:
    res->kind = TOK_STRING_L;
    res->type = FILTER_TYPE_STRING;
    if (env->cur->prob_id <= 0 || env->cur->prob_id > env->maxprob || !env->probs[env->cur->prob_id]) {
      res->v.s = envdup(env, "");
    } else {
      res->v.s = envdup(env, env->probs[env->cur->prob_id]->short_name);
    }
    break;
  case TOK_CURPROB_DIR:
    res->kind = TOK_STRING_L;
    res->type = FILTER_TYPE_STRING;
    if (env->cur->prob_id <= 0 || env->cur->prob_id > env->maxprob || !env->probs[env->cur->prob_id] || !env->probs[env->cur->prob_id]->problem_dir) {
      res->v.s = envdup(env, "");
    } else {
      res->v.s = envdup(env, env->probs[env->cur->prob_id]->problem_dir);
      for (int i = 0; res->v.s[i]; ++i) {
        if (res->v.s[i] == '/') res->v.s[i] = '.';
      }
    }
    break;
  case TOK_CURUID:
    res->kind = TOK_INT_L;
    res->type = FILTER_TYPE_INT;
    res->v.i = env->cur->user_id;
    break;
  case TOK_CURLOGIN:
    res->kind = TOK_STRING_L;
    res->type = FILTER_TYPE_STRING;
    if (!env->cur->user_id) {
      res->v.s = envdup(env, "");
    } else {
      res->v.s = envdup(env, teamdb_get_login(env->teamdb_state, env->cur->user_id));
    }
    break;
  case TOK_CURNAME:
    res->kind = TOK_STRING_L;
    res->type = FILTER_TYPE_STRING;
    if (!env->cur->user_id) {
      res->v.s = envdup(env, "");
    } else {
      res->v.s = envdup(env, teamdb_get_name(env->teamdb_state, env->cur->user_id));
    }
    break;
  case TOK_CURGROUP:
    res->kind = TOK_STRING_L;
    res->type = FILTER_TYPE_STRING;
    user_id = env->cur->user_id;
    if (user_id > 0
        && (u = teamdb_get_userlist(env->teamdb_state, user_id))
        && u->cnts0
        && (m = userlist_members_get_first(u->cnts0->members))) {
      res->v.s = envdup(env, m->group);
    } else {
      res->v.s = envdup(env, "");
    }
    break;
  case TOK_CURLANG:
    res->kind = TOK_STRING_L;
    res->type = FILTER_TYPE_STRING;
    if (env->cur->lang_id <= 0 || env->cur->lang_id > env->maxlang || !env->langs[env->cur->lang_id]) {
      res->v.s = envdup(env, "");
    } else {
      res->v.s = envdup(env, env->langs[env->cur->lang_id]->short_name);
    }
    break;
  case TOK_CURARCH:
    res->kind = TOK_STRING_L;
    res->type = FILTER_TYPE_STRING;
    if (env->cur->lang_id <= 0 || env->cur->lang_id > env->maxlang || !env->langs[env->cur->lang_id] || !env->langs[env->cur->lang_id]->arch) {
      res->v.s = envdup(env, "");
    } else {
      res->v.s = envdup(env, env->langs[env->cur->lang_id]->arch);
    }
    break;
  case TOK_CURRESULT:
    res->kind = TOK_RESULT_L;
    res->type = FILTER_TYPE_RESULT;
    res->v.r = env->cur->status;
    break;
  case TOK_CURSCORE:
    res->kind = TOK_INT_L;
    res->type = FILTER_TYPE_INT;
    res->v.i = env->cur->score;
    break;
  case TOK_CURSCORE_ADJ:
    res->kind = TOK_INT_L;
    res->type = FILTER_TYPE_INT;
    res->v.i = env->cur->score_adj;
    break;
  case TOK_CURTEST:
    res->kind = TOK_INT_L;
    res->type = FILTER_TYPE_INT;
    res->v.i = env->cur->test;
    break;
  case TOK_CURIMPORTED:
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    res->v.b = env->cur->is_imported;
    break;
  case TOK_CURHIDDEN:
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    res->v.b = env->cur->is_hidden;
    break;
  case TOK_CURREADONLY:
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    res->v.b = env->cur->is_readonly;
    break;
  case TOK_CURMARKED:
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    res->v.b = env->cur->is_marked;
    break;
  case TOK_CURSAVED:
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    res->v.b = env->cur->is_saved;
    break;
  case TOK_CURVARIANT:
    res->kind = TOK_INT_L;
    res->type = FILTER_TYPE_INT;
    c = env->cur->variant;
    if (!c) c = find_variant(env->serve_state, env->cur->user_id,
                             env->cur->prob_id, 0);
    res->v.i = c;
    break;
  case TOK_CURRAWVARIANT:
    res->kind = TOK_INT_L;
    res->type = FILTER_TYPE_INT;
    c = env->cur->variant;
    res->v.i = c;
    break;
  case TOK_CURUSERINVISIBLE:
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    user_id = env->cur->user_id;
    if (!user_id) {
      res->v.b = 0;
    } else if ((flags = teamdb_get_flags(env->teamdb_state, user_id)) < 0) {
      res->v.b = 0;
    } else if ((flags & TEAM_INVISIBLE)) {
      res->v.b = 1;
    } else {
      res->v.b = 0;
    }
    break;
  case TOK_CURUSERBANNED:
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    user_id = env->cur->user_id;
    if (!user_id) {
      res->v.b = 0;
    } else if ((flags = teamdb_get_flags(env->teamdb_state, user_id)) < 0) {
      res->v.b = 0;
    } else if ((flags & TEAM_BANNED)) {
      res->v.b = 1;
    } else {
      res->v.b = 0;
    }
    break;
  case TOK_CURUSERLOCKED:
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    user_id = env->cur->user_id;
    if (!user_id) {
      res->v.b = 0;
    } else if ((flags = teamdb_get_flags(env->teamdb_state, user_id)) < 0) {
      res->v.b = 0;
    } else if ((flags & TEAM_LOCKED)) {
      res->v.b = 1;
    } else {
      res->v.b = 0;
    }
    break;
  case TOK_CURUSERINCOMPLETE:
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    user_id = env->cur->user_id;
    if (!user_id) {
      res->v.b = 0;
    } else if ((flags = teamdb_get_flags(env->teamdb_state, user_id)) < 0) {
      res->v.b = 0;
    } else if ((flags & TEAM_INCOMPLETE)) {
      res->v.b = 1;
    } else {
      res->v.b = 0;
    }
    break;
  case TOK_CURUSERDISQUALIFIED:
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    user_id = env->cur->user_id;
    if (!user_id) {
      res->v.b = 0;
    } else if ((flags = teamdb_get_flags(env->teamdb_state, user_id)) < 0) {
      res->v.b = 0;
    } else if ((flags & TEAM_DISQUALIFIED)) {
      res->v.b = 1;
    } else {
      res->v.b = 0;
    }
    break;
  case TOK_CURUSERPRIVILEGED:
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    user_id = env->cur->user_id;
    if (!user_id) {
      res->v.b = 0;
    } else if ((flags = teamdb_get_flags(env->teamdb_state, user_id)) < 0) {
      res->v.b = 0;
    } else if ((flags & TEAM_PRIVILEGED)) {
      res->v.b = 1;
    } else {
      res->v.b = 0;
    }
    break;
  case TOK_CURUSERREG_READONLY:
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    user_id = env->cur->user_id;
    if (!user_id) {
      res->v.b = 0;
    } else if ((flags = teamdb_get_flags(env->teamdb_state, user_id)) < 0) {
      res->v.b = 0;
    } else if ((flags & TEAM_REG_READONLY)) {
      res->v.b = 1;
    } else {
      res->v.b = 0;
    }
    break;
  case TOK_CURLATEST:
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    res->v.b = is_latest(env, env->cur->run_id);
    break;
  case TOK_CURLATESTMARKED:
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    res->v.b = is_latestmarked(env, env->cur->run_id);
    break;
  case TOK_CURAFTEROK:
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    res->v.b = is_afterok(env, env->cur->run_id);
    break;
  case TOK_CUREXAMINABLE:
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    //res->v.b = env->cur->is_examinable;
    res->v.b = 0;
    break;
  case TOK_CURCYPHER:
    res->kind = TOK_STRING_L;
    res->type = FILTER_TYPE_STRING;
    user_id = env->cur->user_id;
    u = 0; s = 0;
    if (user_id > 0) u = teamdb_get_userlist(env->teamdb_state, user_id);
    if (u && u->cnts0) s = u->cnts0->exam_cypher;
    res->v.s = envdup(env, s);
    break;
  case TOK_CURMISSINGSOURCE:
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    res->v.b = is_missing_source(env, env->cur);
    break;
  case TOK_CURJUDGE_ID:
    res->kind = TOK_INT_L;
    res->type = FILTER_TYPE_INT;
    res->v.i = env->cur->j.judge_id;
    break;
  case TOK_CURPASSED_MODE:
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    res->v.i = !!env->cur->passed_mode;
    break;
  case TOK_CUREOLN_TYPE:
    res->kind = TOK_INT_L;
    res->type = FILTER_TYPE_INT;
    res->v.i = env->cur->eoln_type;
    break;
  case TOK_CURSTORE_FLAGS:
    res->kind = TOK_INT_L;
    res->type = FILTER_TYPE_INT;
    res->v.i = env->cur->store_flags;
    break;
  case TOK_CURTOKEN_FLAGS:
    res->kind = TOK_INT_L;
    res->type = FILTER_TYPE_INT;
    res->v.i = env->cur->token_flags;
    break;
  case TOK_CURTOKEN_COUNT:
    res->kind = TOK_INT_L;
    res->type = FILTER_TYPE_INT;
    res->v.i = env->cur->token_count;
    break;
  case TOK_CURTOTAL_SCORE:
    res->kind = TOK_INT_L;
    res->type = FILTER_TYPE_INT;
    res->v.i = serve_get_user_result_score(env->serve_state,env->cur->user_id);
    break;

  case TOK_NOW:
    res->kind = TOK_TIME_L;
    res->type = FILTER_TYPE_TIME;
    res->v.a = env->cur_time;
    break;
  case TOK_START:
    res->kind = TOK_TIME_L;
    res->type = FILTER_TYPE_TIME;
    res->v.a = env->rhead.start_time;
    break;
  case TOK_FINISH:
    res->kind = TOK_TIME_L;
    res->type = FILTER_TYPE_TIME;
    res->v.a = env->rhead.stop_time;
    break;
  case TOK_TOTAL:
    res->kind = TOK_INT_L;
    res->type = FILTER_TYPE_INT;
    res->v.i = env->rtotal;
    break;

  case TOK_INT_L:
  case TOK_STRING_L:
  case TOK_BOOL_L:
  case TOK_TIME_L:
  case TOK_DUR_L:
  case TOK_SIZE_L:
  case TOK_RESULT_L:
  case TOK_HASH_L:
  case TOK_IP_L:
    *res = *t;
    return 0;

  case TOK_EXAMINATOR:
    if ((c = do_eval(env, t->v.t[0], &r1)) < 0) return c;
    ASSERT(r1.kind == TOK_INT_L);
    if (r1.v.i < 0) r1.v.i = env->rtotal + r1.v.i;
    if (r1.v.i >= env->rtotal) return -FILTER_ERR_RANGE;
    if (r1.v.i < env->rbegin) return -FILTER_ERR_RANGE;
    if ((c = do_eval(env, t->v.t[1], &r2)) < 0) return c;
    ASSERT(r2.kind == TOK_INT_L);
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    res->v.b = 0;
    /*
    for (c = 0; c < 3; c++) {
      if (env->rentries[r1.v.i].examiners[c] == r2.v.i) {
        res->v.b = 1;
        break;
      }
    }
    */
    break;

  case TOK_CUREXAMINATOR:
    if ((c = do_eval(env, t->v.t[0], &r1)) < 0) return c;
    ASSERT(r1.kind == TOK_INT_L);
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    res->v.b = 0;
    /*
    for (c = 0; c < 3; c++) {
      if (env->cur->examiners[c] == r1.v.i) {
        res->v.b = 1;
        break;
      }
    }
    */
    break;

  case TOK_CURHAS_TEST_RESULT:
    if ((c = do_eval(env, t->v.t[0], &r1)) < 0) return c;
    ASSERT(r1.kind == TOK_RESULT_L);
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    res->v.b = has_test_result(env, env->cur, r1.v.r);
    break;

  case TOK_INUSERGROUP:
    if ((c = do_eval(env, t->v.t[0], &r1)) < 0) return c;
    ASSERT(r1.kind == TOK_STRING_L);
    if ((c = find_user_group(env, r1.v.s)) < 0) return c;
    t->kind = TOK_INUSERGROUPINT;
    t->v.t[0] = filter_tree_new_int(env->mem, c);
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    res->v.b = check_user_group(env, env->cur->user_id, c);
    break;

  case TOK_INUSERGROUPINT:
    if ((c = do_eval(env, t->v.t[0], &r1)) < 0) return c;
    ASSERT(r1.kind == TOK_INT_L);
    res->kind = TOK_BOOL_L;
    res->type = FILTER_TYPE_BOOL;
    res->v.b = check_user_group(env, env->cur->user_id, r1.v.i);
    break;

  default:
    SWERR(("unhandled kind: %d", t->kind));
  }

  return 0;
}

int
filter_tree_bool_eval(struct filter_env *env,
                      struct filter_tree *t)
{
  struct filter_tree *res = 0;
  int r;

  ASSERT(t);
  ASSERT(t->type == FILTER_TYPE_BOOL);
  res = filter_tree_new_int(env->mem, 0);
  env->cur = &env->rentries[env->rid];
  if ((r = do_eval(env, t, res)) < 0) return r;
  ASSERT(res->type == FILTER_TYPE_BOOL);
  ASSERT(res->kind == TOK_BOOL_L);
  return res->v.b;
}
