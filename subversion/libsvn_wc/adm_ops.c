/*
 * adm_ops.c: routines for affecting working copy administrative
 *            information.  NOTE: this code doesn't know where the adm
 *            info is actually stored.  Instead, generic handles to
 *            adm data are requested via a reference to some PATH
 *            (PATH being a regular, non-administrative directory or
 *            file in the working copy).
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */



#include <string.h>

#include <apr_pools.h>
#include <apr_tables.h>
#include <apr_hash.h>
#include <apr_file_io.h>
#include <apr_time.h>
#include <apr_errno.h>

#include "svn_types.h"
#include "svn_pools.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_hash.h"
#include "svn_wc.h"
#include "svn_io.h"
#include "svn_xml.h"
#include "svn_time.h"
#include "svn_diff.h"
#include "svn_sorts.h"

#include "wc.h"
#include "adm_files.h"
#include "entries.h"
#include "props.h"
#include "translate.h"
#include "tree_conflicts.h"
#include "workqueue.h"

#include "svn_private_config.h"
#include "private/svn_wc_private.h"



struct svn_wc_committed_queue_t
{
  /* The pool in which ->queue is allocated. */
  apr_pool_t *pool;
  /* Mapping (const char *) local_abspath to (committed_queue_item_t *). */
  apr_hash_t *queue;
  /* Is any item in the queue marked as 'recursive'? */
  svn_boolean_t have_recursive;
};

typedef struct committed_queue_item_t
{
  const char *local_abspath;
  svn_boolean_t recurse;
  svn_boolean_t no_unlock;
  svn_boolean_t keep_changelist;

  /* The pristine text checksum(s). Either or both may be present. */
  const svn_checksum_t *md5_checksum;
  const svn_checksum_t *sha1_checksum;

  apr_hash_t *new_dav_cache;
} committed_queue_item_t;


apr_pool_t *
svn_wc__get_committed_queue_pool(const struct svn_wc_committed_queue_t *queue)
{
  return queue->pool;
}



/*** Finishing updates and commits. ***/

/* Queue work items that will finish a commit of the file or directory
 * LOCAL_ABSPATH in DB:
 *   - queue the removal of any "revert-base" props and text files;
 *   - queue an update of the DB entry for this node
 *
 * ### The Pristine Store equivalent should be:
 *   - remember the old BASE_NODE and WORKING_NODE pristine text c'sums;
 *   - queue an update of the DB entry for this node (incl. updating the
 *       BASE_NODE c'sum and setting the WORKING_NODE c'sum to NULL);
 *   - queue deletion of the old pristine texts by the remembered checksums.
 *
 * CHECKSUM is the checksum of the new text base for LOCAL_ABSPATH, and must
 * be provided if there is one, else NULL. */
static svn_error_t *
process_committed_leaf(svn_wc__db_t *db,
                       const char *local_abspath,
                       svn_boolean_t via_recurse,
                       svn_revnum_t new_revnum,
                       apr_time_t new_changed_date,
                       const char *new_changed_author,
                       apr_hash_t *new_dav_cache,
                       svn_boolean_t no_unlock,
                       svn_boolean_t keep_changelist,
                       const svn_checksum_t *checksum,
                       apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;
  svn_wc__db_kind_t kind;
  const svn_checksum_t *copied_checksum;
  const char *adm_abspath;
  svn_revnum_t new_changed_rev = new_revnum;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_read_info(&status, &kind, NULL,
                               NULL, NULL, NULL,
                               NULL, NULL, NULL,
                               NULL, NULL, &copied_checksum,
                               NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL,
                               db, local_abspath,
                               scratch_pool, scratch_pool));

  if (kind == svn_wc__db_kind_dir)
    adm_abspath = local_abspath;
  else
    adm_abspath = svn_dirent_dirname(local_abspath, scratch_pool);
  SVN_ERR(svn_wc__write_check(db, adm_abspath, scratch_pool));

  if (status == svn_wc__db_status_deleted)
    {
      return svn_error_return(svn_wc__wq_add_deletion_postcommit(
                                db, local_abspath, new_revnum, no_unlock,
                                scratch_pool));
    }

  /* ### this picks up file and symlink  */
  if (kind != svn_wc__db_kind_dir)
    {
      /* If we sent a delta (meaning: post-copy modification),
         then this file will appear in the queue and so we should have
         its checksum already. */
      if (checksum == NULL)
        {
          /* It was copied and not modified. We must have a text
             base for it. And the node should have a checksum. */
          SVN_ERR_ASSERT(copied_checksum != NULL);

          checksum = copied_checksum;

          if (via_recurse)
            {
              /* If a copied node itself is not modified, but the op_root of
                the copy is committed we have to make sure that changed_rev,
                changed_date and changed_author don't change or the working
                copy used for committing will show different last modified
                information then a clean checkout of exactly the same
                revisions. (Issue #3676) */

                svn_boolean_t props_modified;
                svn_revnum_t changed_rev;
                const char *changed_author;
                apr_time_t changed_date;

                SVN_ERR(svn_wc__db_read_info(NULL, NULL, NULL, NULL, NULL,
                                             NULL, &changed_rev, &changed_date,
                                             &changed_author, NULL, NULL, NULL,
                                             NULL, NULL, NULL, NULL, NULL,
                                             NULL, NULL, &props_modified, NULL,
                                             NULL, NULL, NULL,
                                             db, local_abspath,
                                             scratch_pool, scratch_pool));

                if (!props_modified)
                  {
                    /* Unmodified child of copy: We keep changed_rev */
                    new_changed_rev = changed_rev;
                    new_changed_date = changed_date;
                    new_changed_author = changed_author;
                  }
            }
        }
    }
  else
    {
      /* ### If we can determine that nothing below this node was changed
         ### via this commit, we should keep new_changed_rev at its old
         ### value, like how we handle files. */
    }

  /* The new text base will be found in the pristine store by its checksum. */
  SVN_ERR(svn_wc__wq_add_postcommit(db, local_abspath, 
                                    new_revnum,
                                    new_changed_rev, new_changed_date,
                                    new_changed_author, checksum,
                                    new_dav_cache, keep_changelist, no_unlock,
                                    scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__process_committed_internal(svn_wc__db_t *db,
                                   const char *local_abspath,
                                   svn_boolean_t recurse,
                                   svn_boolean_t top_of_recurse,
                                   svn_revnum_t new_revnum,
                                   apr_time_t new_date,
                                   const char *rev_author,
                                   apr_hash_t *new_dav_cache,
                                   svn_boolean_t no_unlock,
                                   svn_boolean_t keep_changelist,
                                   const svn_checksum_t *md5_checksum,
                                   const svn_checksum_t *sha1_checksum,
                                   const svn_wc_committed_queue_t *queue,
                                   apr_pool_t *scratch_pool)
{
  svn_wc__db_kind_t kind;

  SVN_ERR(svn_wc__db_read_kind(&kind, db, local_abspath, TRUE, scratch_pool));

  SVN_ERR(process_committed_leaf(db, local_abspath, !top_of_recurse,
                                 new_revnum, new_date, rev_author,
                                 new_dav_cache,
                                 no_unlock, keep_changelist,
                                 sha1_checksum,
                                 scratch_pool));

  if (recurse && kind == svn_wc__db_kind_dir)
    {
      const apr_array_header_t *children;
      apr_pool_t *iterpool = svn_pool_create(scratch_pool);
      int i;

      /* Run the log. It might delete this node, leaving us nothing
         more to do.  */
      SVN_ERR(svn_wc__wq_run(db, local_abspath, NULL, NULL, iterpool));
      SVN_ERR(svn_wc__db_read_kind(&kind, db, local_abspath, TRUE, iterpool));
      if (kind == svn_wc__db_kind_unknown)
        return SVN_NO_ERROR;  /* it got deleted!  */

      /* Read PATH's entries;  this is the absolute path. */
      SVN_ERR(svn_wc__db_read_children(&children, db, local_abspath,
                                       scratch_pool, iterpool));

      /* Recursively loop over all children. */
      for (i = 0; i < children->nelts; i++)
        {
          const char *name = APR_ARRAY_IDX(children, i, const char *);
          const char *this_abspath;
          svn_wc__db_status_t status;

          svn_pool_clear(iterpool);

          this_abspath = svn_dirent_join(local_abspath, name, iterpool);

          SVN_ERR(svn_wc__db_read_info(&status, &kind, NULL,
                                       NULL, NULL, NULL,
                                       NULL, NULL, NULL,
                                       NULL, NULL, NULL,
                                       NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL,
                                       db, this_abspath,
                                       iterpool, iterpool));

          /* We come to this branch since we have committed a copied tree.
             svn_depth_exclude is possible in this situation. So check and
             skip */
          if (status == svn_wc__db_status_excluded)
            continue;

          md5_checksum = NULL;
          sha1_checksum = NULL;
          if (kind != svn_wc__db_kind_dir)
            {
              /* Suppress log creation for deleted entries in a replaced
                 directory.  By the time any log we create here is run,
                 those entries will already have been removed (as a result
                 of running the log for the replaced directory that was
                 created at the start of this function). */
              if (status == svn_wc__db_status_deleted)
                {
                  svn_boolean_t replaced;

                  SVN_ERR(svn_wc__internal_is_replaced(&replaced,
                                                       db, local_abspath,
                                                       iterpool));
                  if (replaced)
                    continue;
                }

              if (queue != NULL)
                {
                  const committed_queue_item_t *cqi
                    = apr_hash_get(queue->queue, this_abspath,
                                   APR_HASH_KEY_STRING);

                  if (cqi != NULL)
                    {
                      md5_checksum = cqi->md5_checksum;
                      sha1_checksum = cqi->sha1_checksum;
                    }
                }
            }

          /* Recurse.  Pass NULL for NEW_DAV_CACHE, because the
             ones present in the current call are only applicable to
             this one committed item. */
          SVN_ERR(svn_wc__process_committed_internal(db, this_abspath,
                                                     TRUE /* recurse */,
                                                     FALSE,
                                                     new_revnum, new_date,
                                                     rev_author,
                                                     NULL,
                                                     TRUE /* no_unlock */,
                                                     keep_changelist,
                                                     md5_checksum,
                                                     sha1_checksum,
                                                     queue, iterpool));

          if (kind == svn_wc__db_kind_dir)
            SVN_ERR(svn_wc__wq_run(db, this_abspath, NULL, NULL, iterpool));
        }

      svn_pool_destroy(iterpool);
   }

  return SVN_NO_ERROR;
}


apr_hash_t *
svn_wc__prop_array_to_hash(const apr_array_header_t *props,
                           apr_pool_t *result_pool)
{
  int i;
  apr_hash_t *prophash;

  if (props == NULL || props->nelts == 0)
    return NULL;

  prophash = apr_hash_make(result_pool);

  for (i = 0; i < props->nelts; i++)
    {
      const svn_prop_t *prop = APR_ARRAY_IDX(props, i, const svn_prop_t *);
      if (prop->value != NULL)
        apr_hash_set(prophash, prop->name, APR_HASH_KEY_STRING, prop->value);
    }

  return prophash;
}


svn_wc_committed_queue_t *
svn_wc_committed_queue_create(apr_pool_t *pool)
{
  svn_wc_committed_queue_t *q;

  q = apr_palloc(pool, sizeof(*q));
  q->pool = pool;
  q->queue = apr_hash_make(pool);
  q->have_recursive = FALSE;

  return q;
}

svn_error_t *
svn_wc_queue_committed3(svn_wc_committed_queue_t *queue,
                        svn_wc_context_t *wc_ctx,
                        const char *local_abspath,
                        svn_boolean_t recurse,
                        const apr_array_header_t *wcprop_changes,
                        svn_boolean_t remove_lock,
                        svn_boolean_t remove_changelist,
                        const svn_checksum_t *md5_checksum,
                        const svn_checksum_t *sha1_checksum,
                        apr_pool_t *scratch_pool)
{
  committed_queue_item_t *cqi;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  queue->have_recursive |= recurse;

  /* Use the same pool as the one QUEUE was allocated in,
     to prevent lifetime issues.  Intermediate operations
     should use SCRATCH_POOL. */

  /* Add to the array with paths and options */
  cqi = apr_palloc(queue->pool, sizeof(*cqi));
  cqi->local_abspath = local_abspath;
  cqi->recurse = recurse;
  cqi->no_unlock = !remove_lock;
  cqi->keep_changelist = !remove_changelist;
  cqi->md5_checksum = md5_checksum;
  cqi->sha1_checksum = sha1_checksum;
  cqi->new_dav_cache = svn_wc__prop_array_to_hash(wcprop_changes, queue->pool);

  apr_hash_set(queue->queue, local_abspath, APR_HASH_KEY_STRING, cqi);

  return SVN_NO_ERROR;
}

/* Return TRUE if any item of QUEUE is a parent of ITEM and will be
   processed recursively, return FALSE otherwise.

   The algorithmic complexity of this search implementation is O(queue
   length), but it's quite quick.
*/
static svn_boolean_t
have_recursive_parent(apr_hash_t *queue,
                      const committed_queue_item_t *item,
                      apr_pool_t *scratch_pool)
{
  apr_hash_index_t *hi;
  const char *local_abspath = item->local_abspath;

  for (hi = apr_hash_first(scratch_pool, queue); hi; hi = apr_hash_next(hi))
    {
      const committed_queue_item_t *qi = svn__apr_hash_index_val(hi);

      if (qi == item)
        continue;

      if (qi->recurse && svn_dirent_is_child(qi->local_abspath, local_abspath,
                                             NULL))
        return TRUE;
    }

  return FALSE;
}

svn_error_t *
svn_wc_process_committed_queue2(svn_wc_committed_queue_t *queue,
                                svn_wc_context_t *wc_ctx,
                                svn_revnum_t new_revnum,
                                const char *rev_date,
                                const char *rev_author,
                                apr_pool_t *scratch_pool)
{
  apr_array_header_t *sorted_queue;
  int i;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  apr_time_t new_date;

  if (rev_date)
    SVN_ERR(svn_time_from_cstring(&new_date, rev_date, iterpool));
  else
    new_date = 0;

  /* Process the queued items in order of their paths.  (The requirement is
   * probably just that a directory must be processed before its children.) */
  sorted_queue = svn_sort__hash(queue->queue, svn_sort_compare_items_as_paths,
                                scratch_pool);
  for (i = 0; i < sorted_queue->nelts; i++)
    {
      const svn_sort__item_t *sort_item
        = &APR_ARRAY_IDX(sorted_queue, i, svn_sort__item_t);
      const committed_queue_item_t *cqi = sort_item->value;

      svn_pool_clear(iterpool);

      /* Skip this item if it is a child of a recursive item, because it has
         been (or will be) accounted for when that recursive item was (or
         will be) processed. */
      if (queue->have_recursive && have_recursive_parent(queue->queue, cqi,
                                                         iterpool))
        continue;

      SVN_ERR(svn_wc__process_committed_internal(wc_ctx->db, cqi->local_abspath,
                                                 cqi->recurse, TRUE, new_revnum,
                                                 new_date, rev_author,
                                                 cqi->new_dav_cache,
                                                 cqi->no_unlock,
                                                 cqi->keep_changelist,
                                                 cqi->md5_checksum,
                                                 cqi->sha1_checksum, queue,
                                                 iterpool));

      SVN_ERR(svn_wc__wq_run(wc_ctx->db, cqi->local_abspath, NULL, NULL,
                             iterpool));
    }

  SVN_ERR(svn_hash__clear(queue->queue, iterpool));

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}



/* Remove/erase PATH from the working copy. This involves deleting PATH
 * from the physical filesystem. PATH is assumed to be an unversioned file
 * or directory.
 *
 * If ignore_enoent is TRUE, ignore missing targets.
 *
 * If CANCEL_FUNC is non-null, invoke it with CANCEL_BATON at various
 * points, return any error immediately.
 */
static svn_error_t *
erase_unversioned_from_wc(const char *path,
                          svn_boolean_t ignore_enoent,
                          svn_cancel_func_t cancel_func,
                          void *cancel_baton,
                          apr_pool_t *scratch_pool)
{
  svn_error_t *err;

  /* Optimize the common case: try to delete the file */
  err = svn_io_remove_file2(path, ignore_enoent, scratch_pool);
  if (err)
    {
      /* Then maybe it was a directory? */
      svn_error_clear(err);

      err = svn_io_remove_dir2(path, ignore_enoent, cancel_func, cancel_baton,
                               scratch_pool);

      if (err)
        {
          /* We're unlikely to end up here. But we need this fallback
             to make sure we report the right error *and* try the
             correct deletion at least once. */
          svn_node_kind_t kind;

          svn_error_clear(err);
          SVN_ERR(svn_io_check_path(path, &kind, scratch_pool));
          if (kind == svn_node_file)
            SVN_ERR(svn_io_remove_file2(path, ignore_enoent, scratch_pool));
          else if (kind == svn_node_dir)
            SVN_ERR(svn_io_remove_dir2(path, ignore_enoent,
                                       cancel_func, cancel_baton,
                                       scratch_pool));
          else if (kind == svn_node_none)
            return svn_error_createf(SVN_ERR_BAD_FILENAME, NULL,
                                     _("'%s' does not exist"),
                                     svn_dirent_local_style(path,
                                                            scratch_pool));
          else
            return svn_error_createf(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                                     _("Unsupported node kind for path '%s'"),
                                     svn_dirent_local_style(path, scratch_pool));

        }
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_delete4(svn_wc_context_t *wc_ctx,
               const char *local_abspath,
               svn_boolean_t keep_local,
               svn_boolean_t delete_unversioned_target,
               svn_cancel_func_t cancel_func,
               void *cancel_baton,
               svn_wc_notify_func2_t notify_func,
               void *notify_baton,
               apr_pool_t *pool)
{
  svn_wc__db_t *db = wc_ctx->db;
  svn_error_t *err;
  svn_wc__db_status_t status;
  svn_wc__db_kind_t kind;

  err = svn_wc__db_read_info(&status, &kind, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL,
                             db, local_abspath, pool, pool);

  if (delete_unversioned_target &&
      err != NULL && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
    {
      svn_error_clear(err);

      if (!keep_local)
        SVN_ERR(erase_unversioned_from_wc(local_abspath, FALSE,
                                          cancel_func, cancel_baton,
                                          pool));
      return SVN_NO_ERROR;
    }
  else
    SVN_ERR(err);

  switch (status)
    {
      case svn_wc__db_status_absent:
      case svn_wc__db_status_excluded:
      case svn_wc__db_status_not_present:
        return svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND, NULL,
                                 _("'%s' cannot be deleted"),
                                 svn_dirent_local_style(local_abspath, pool));

      /* Explicitly ignore other statii */
      default:
        break;
    }

  if (kind == svn_wc__db_kind_dir)
    {
      /* ### NODE_DATA We recurse into the subtree here, which is fine,
         except that we also need to record the op_depth to pass to
         svn_wc__db_temp_op_delete(), which is determined by the original
         path for which svn_wc_delete4() was called. We need a helper
         function which receives the op_depth as an argument to apply to
         the entire subtree.
       */
      apr_pool_t *iterpool = svn_pool_create(pool);
      const apr_array_header_t *children;
      int i;

      SVN_ERR(svn_wc__db_read_children(&children, db, local_abspath,
                                       pool, pool));

      for (i = 0; i < children->nelts; i++)
        {
          const char *child_basename = APR_ARRAY_IDX(children, i, const char *);
          const char *child_abspath;
          svn_boolean_t hidden;

          svn_pool_clear(iterpool);

          child_abspath = svn_dirent_join(local_abspath, child_basename,
                                          iterpool);
          SVN_ERR(svn_wc__db_node_hidden(&hidden, db, child_abspath, iterpool));
          if (hidden)
            continue;

          SVN_ERR(svn_wc_delete4(wc_ctx, child_abspath,
                                 keep_local, delete_unversioned_target,
                                 cancel_func, cancel_baton,
                                 notify_func, notify_baton,
                                 iterpool));
        }

      svn_pool_destroy(iterpool);
    }

  /* ### Maybe we should disallow deleting switched nodes here? */

    {
      /* ### The following two operations should be inside one SqLite
             transaction. For even better behavior the tree operation
             before this block needs the same handling.
             Luckily most of this is for free once properties and pristine
             are handled in the WC-NG way. */
      SVN_ERR(svn_wc__db_temp_op_delete(wc_ctx->db, local_abspath, pool));
    }

  /* Report the deletion to the caller. */
  if (notify_func != NULL)
    (*notify_func)(notify_baton,
                   svn_wc_create_notify(local_abspath, svn_wc_notify_delete,
                                        pool), pool);

  /* By the time we get here, anything that was scheduled to be added has
     become unversioned */
  if (!keep_local)
    {
        SVN_ERR(erase_unversioned_from_wc(local_abspath, TRUE,
                                          cancel_func, cancel_baton,
                                          pool));
    }

  return SVN_NO_ERROR;
}

/* Schedule the single node at LOCAL_ABSPATH, of kind KIND, for addition in
 * its parent directory in the WC.  It will have no properties. */
static svn_error_t *
add_from_disk(svn_wc__db_t *db,
              const char *local_abspath,
              svn_node_kind_t kind,
              svn_wc_notify_func2_t notify_func,
              void *notify_baton,
              apr_pool_t *scratch_pool)
{
  if (kind == svn_node_file)
    {
      SVN_ERR(svn_wc__db_op_add_file(db, local_abspath, NULL, scratch_pool));
    }
  else
    {
      SVN_ERR(svn_wc__db_op_add_directory(db, local_abspath, NULL,
                                          scratch_pool));

      /* Remove any existing changelist on the prior node. */
      SVN_ERR(svn_wc__db_op_set_changelist(db, local_abspath, NULL, NULL,
                                           svn_depth_empty, scratch_pool));

      /* And tell someone what we've done. */
      if (notify_func)
        SVN_ERR(svn_wc__db_changelist_list_notify(notify_func, notify_baton,
                                                  db, local_abspath,
                                                  scratch_pool));
    }

  return SVN_NO_ERROR;
}

/* Set *REPOS_ROOT_URL and *REPOS_UUID to the repository of the parent of
   LOCAL_ABSPATH.  REPOS_ROOT_URL and/or REPOS_UUID may be NULL if not
   wanted.  Check that the parent of LOCAL_ABSPATH is a versioned directory
   in a state in which a new child node can be scheduled for addition;
   return an error if not. */
static svn_error_t *
check_can_add_to_parent(const char **repos_root_url,
                        const char **repos_uuid,
                        svn_wc__db_t *db,
                        const char *local_abspath,
                        apr_pool_t *result_pool,
                        apr_pool_t *scratch_pool)
{
  const char *parent_abspath = svn_dirent_dirname(local_abspath, scratch_pool);
  svn_wc__db_status_t parent_status;
  svn_wc__db_kind_t parent_kind;
  svn_error_t *err;

  SVN_ERR(svn_wc__write_check(db, parent_abspath, scratch_pool));

  err = svn_wc__db_read_info(&parent_status, &parent_kind, NULL,
                             NULL, repos_root_url,
                             repos_uuid, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL,
                             db, parent_abspath, result_pool, scratch_pool);

  if (err
      || parent_status == svn_wc__db_status_not_present
      || parent_status == svn_wc__db_status_excluded
      || parent_status == svn_wc__db_status_absent)
    {
      return
        svn_error_createf(SVN_ERR_ENTRY_NOT_FOUND, err,
                          _("Can't find parent directory's node while"
                            " trying to add '%s'"),
                          svn_dirent_local_style(local_abspath,
                                                 scratch_pool));
    }
  else if (parent_status == svn_wc__db_status_deleted)
    {
      return
        svn_error_createf(SVN_ERR_WC_SCHEDULE_CONFLICT, NULL,
                          _("Can't add '%s' to a parent directory"
                            " scheduled for deletion"),
                          svn_dirent_local_style(local_abspath,
                                                 scratch_pool));
    }
  else if (parent_kind != svn_wc__db_kind_dir)
    return svn_error_createf(SVN_ERR_NODE_UNEXPECTED_KIND, NULL,
                             _("Can't schedule an addition of '%s'"
                               " below a not-directory node"),
                             svn_dirent_local_style(local_abspath,
                                                    scratch_pool));

  /* If we haven't found the repository info yet, find it now. */
  if ((repos_root_url && ! *repos_root_url)
      || (repos_uuid && ! *repos_uuid))
    {
      if (parent_status == svn_wc__db_status_added)
        SVN_ERR(svn_wc__db_scan_addition(NULL, NULL, NULL,
                                         repos_root_url, repos_uuid, NULL,
                                         NULL, NULL, NULL,
                                         db, parent_abspath,
                                         result_pool, scratch_pool));
      else
        SVN_ERR(svn_wc__db_scan_base_repos(NULL,
                                           repos_root_url, repos_uuid,
                                           db, parent_abspath,
                                           result_pool, scratch_pool));
    }

  return SVN_NO_ERROR;
}

/* Check that the on-disk item at LOCAL_ABSPATH can be scheduled for
 * addition to its WC parent directory.
 *
 * Set *KIND_P to the kind of node to be added, *DB_ROW_EXISTS_P to whether
 * it is already a versioned path, and if so, *IS_WC_ROOT_P to whether it's
 * a WC root.
 *
 * ### The checks here, and the outputs, are geared towards svn_wc_add4().
 */
static svn_error_t *
check_can_add_node(svn_node_kind_t *kind_p,
                   svn_boolean_t *db_row_exists_p,
                   svn_boolean_t *is_wc_root_p,
                   svn_wc__db_t *db,
                   const char *local_abspath,
                   const char *copyfrom_url,
                   svn_revnum_t copyfrom_rev,
                   apr_pool_t *scratch_pool)
{
  const char *base_name = svn_dirent_basename(local_abspath, scratch_pool);
  svn_boolean_t is_wc_root;
  svn_node_kind_t kind;
  svn_boolean_t exists;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(!copyfrom_url || (svn_uri_is_canonical(copyfrom_url,
                                                        scratch_pool)
                                   && SVN_IS_VALID_REVNUM(copyfrom_rev)));

  /* Check that the proposed node has an acceptable name. */
  if (svn_wc_is_adm_dir(base_name, scratch_pool))
    return svn_error_createf
      (SVN_ERR_ENTRY_FORBIDDEN, NULL,
       _("Can't create an entry with a reserved name while trying to add '%s'"),
       svn_dirent_local_style(local_abspath, scratch_pool));

  SVN_ERR(svn_path_check_valid(local_abspath, scratch_pool));

  /* Make sure something's there; set KIND and *KIND_P. */
  SVN_ERR(svn_io_check_path(local_abspath, &kind, scratch_pool));
  if (kind == svn_node_none)
    return svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND, NULL,
                             _("'%s' not found"),
                             svn_dirent_local_style(local_abspath,
                                                    scratch_pool));
  if (kind == svn_node_unknown)
    return svn_error_createf(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                             _("Unsupported node kind for path '%s'"),
                             svn_dirent_local_style(local_abspath,
                                                    scratch_pool));
  if (kind_p)
    *kind_p = kind;

  /* Determine whether a DB row for this node EXISTS, and whether it
     IS_WC_ROOT.  If it exists, check that it is in an acceptable state for
     adding the new node; if not, return an error. */
  {
    svn_wc__db_status_t status;
    svn_error_t *err
      = svn_wc__db_read_info(&status, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL,
                             db, local_abspath,
                             scratch_pool, scratch_pool);

    if (err)
      {
        if (err->apr_err != SVN_ERR_WC_PATH_NOT_FOUND)
          return svn_error_return(err);

        svn_error_clear(err);
        exists = FALSE;
        is_wc_root = FALSE;
      }
    else
      {
        is_wc_root = FALSE;
        exists = TRUE;
        switch (status)
          {
            case svn_wc__db_status_not_present:
              break;
            case svn_wc__db_status_deleted:
              /* A working copy root should never have a WORKING_NODE */
              SVN_ERR_ASSERT(!is_wc_root);
              break;
            case svn_wc__db_status_normal:
              if (copyfrom_url)
                {
                  SVN_ERR(svn_wc__check_wc_root(&is_wc_root, NULL, NULL,
                                                db, local_abspath,
                                                scratch_pool));

                  if (is_wc_root)
                    break;
                }
              /* else: Fall through in default error */

            default:
              return svn_error_createf(
                               SVN_ERR_ENTRY_EXISTS, NULL,
                               _("'%s' is already under version control"),
                               svn_dirent_local_style(local_abspath,
                                                      scratch_pool));
          }
      } /* err */

    if (db_row_exists_p)
      *db_row_exists_p = exists;
    if (is_wc_root_p)
      *is_wc_root_p = is_wc_root;
  }

  return SVN_NO_ERROR;
}

/* Convert the nested pristine working copy rooted at LOCAL_ABSPATH into
 * a copied subtree in the outer working copy.
 *
 * LOCAL_ABSPATH must be the root of a nested working copy that has no
 * local modifications.  The parent directory of LOCAL_ABSPATH must be a
 * versioned directory in the outer WC, and must belong to the same
 * repository as the nested WC.  The nested WC will be integrated into the
 * parent's WC, and will no longer be a separate WC. */
static svn_error_t *
integrate_nested_wc_as_copy(svn_wc_context_t *wc_ctx,
                            const char *local_abspath,
                            apr_pool_t *scratch_pool)
{
  svn_wc__db_t *db = wc_ctx->db;
  const char *moved_abspath;

  /* Drop any references to the wc that is to be rewritten */
  SVN_ERR(svn_wc__db_drop_root(db, local_abspath, scratch_pool));

  /* Move the admin dir from the wc to a temporary location: MOVED_ABSPATH */
  {
    const char *tmpdir_abspath, *moved_adm_abspath, *adm_abspath;

    SVN_ERR(svn_wc__db_temp_wcroot_tempdir(&tmpdir_abspath, db,
                                           svn_dirent_dirname(local_abspath,
                                                              scratch_pool),
                                           scratch_pool, scratch_pool));
    SVN_ERR(svn_io_open_unique_file3(NULL, &moved_abspath, tmpdir_abspath,
                                     svn_io_file_del_on_close,
                                     scratch_pool, scratch_pool));
    SVN_ERR(svn_io_dir_make(moved_abspath, APR_OS_DEFAULT, scratch_pool));

    adm_abspath = svn_wc__adm_child(local_abspath, "", scratch_pool);
    moved_adm_abspath = svn_wc__adm_child(moved_abspath, "", scratch_pool);
    SVN_ERR(svn_io_file_move(adm_abspath, moved_adm_abspath, scratch_pool));
  }

  /* Copy entries from temporary location into the main db */
  SVN_ERR(svn_wc_copy3(wc_ctx, moved_abspath, local_abspath,
                       TRUE /* metadata_only */,
                       NULL, NULL, NULL, NULL, scratch_pool));

  /* Cleanup the temporary admin dir */
  SVN_ERR(svn_wc__db_drop_root(db, moved_abspath, scratch_pool));
  SVN_ERR(svn_io_remove_dir2(moved_abspath, FALSE, NULL, NULL,
                             scratch_pool));

  /* The subdir is now part of our parent working copy. Our caller assumes
     that we return the new node locked, so obtain a lock if we didn't
     receive the lock via our depth infinity lock */
  {
    svn_boolean_t owns_lock;
    SVN_ERR(svn_wc__db_wclock_owns_lock(&owns_lock, db, local_abspath,
                                        FALSE, scratch_pool));
    if (!owns_lock)
      SVN_ERR(svn_wc__db_wclock_obtain(db, local_abspath, 0, FALSE,
                                       scratch_pool));
  }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_add4(svn_wc_context_t *wc_ctx,
            const char *local_abspath,
            svn_depth_t depth,
            const char *copyfrom_url,
            svn_revnum_t copyfrom_rev,
            svn_cancel_func_t cancel_func,
            void *cancel_baton,
            svn_wc_notify_func2_t notify_func,
            void *notify_baton,
            apr_pool_t *scratch_pool)
{
  svn_wc__db_t *db = wc_ctx->db;
  svn_node_kind_t kind;
  svn_boolean_t db_row_exists, is_wc_root;
  const char *repos_root_url, *repos_uuid;

  SVN_ERR(check_can_add_node(&kind, &db_row_exists, &is_wc_root,
                             db, local_abspath, copyfrom_url, copyfrom_rev,
                             scratch_pool));

  /* Get REPOS_ROOT_URL and REPOS_UUID.  Check that the
     parent is a versioned directory in an acceptable state. */
  SVN_ERR(check_can_add_to_parent(&repos_root_url, &repos_uuid,
                                  db, local_abspath, scratch_pool,
                                  scratch_pool));

  /* If we're performing a repos-to-WC copy, check that the copyfrom
     repository is the same as the parent dir's repository. */
  if (copyfrom_url && !svn_uri_is_ancestor(repos_root_url, copyfrom_url))
    return svn_error_createf(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                             _("The URL '%s' has a different repository "
                               "root than its parent"), copyfrom_url);

  /* Verify that we can actually integrate the inner working copy */
  if (is_wc_root)
    {
      const char *repos_relpath, *inner_repos_root_url, *inner_repos_uuid;
      const char *inner_url;

      SVN_ERR(svn_wc__db_scan_base_repos(&repos_relpath,
                                         &inner_repos_root_url,
                                         &inner_repos_uuid,
                                         db, local_abspath,
                                         scratch_pool, scratch_pool));

      if (strcmp(inner_repos_uuid, repos_uuid)
          || strcmp(repos_root_url, inner_repos_root_url))
        return svn_error_createf(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                                 _("Can't schedule the working copy at '%s' "
                                   "from repository '%s' with uuid '%s' "
                                   "for addition under a working copy from "
                                   "repository '%s' with uuid '%s'."),
                                 svn_dirent_local_style(local_abspath,
                                                        scratch_pool),
                                 inner_repos_root_url, inner_repos_uuid,
                                 repos_root_url, repos_uuid);

      inner_url = svn_path_url_add_component2(repos_root_url, repos_relpath,
                                              scratch_pool);

      if (strcmp(copyfrom_url, inner_url))
        return svn_error_createf(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                                 _("Can't add '%s' with URL '%s', but with "
                                   "the data from '%s'"),
                                 svn_dirent_local_style(local_abspath,
                                                        scratch_pool),
                                 copyfrom_url, inner_url);
    }

  if (!copyfrom_url)  /* Case 2a: It's a simple add */
    {
      SVN_ERR(add_from_disk(db, local_abspath, kind, notify_func, notify_baton,
                            scratch_pool));
      if (kind == svn_node_dir && !db_row_exists)
        {
          /* If using the legacy 1.6 interface the parent lock may not
             be recursive and add is expected to lock the new dir.

             ### Perhaps the lock should be created in the same
             transaction that adds the node? */
          svn_boolean_t owns_lock;
          SVN_ERR(svn_wc__db_wclock_owns_lock(&owns_lock, db, local_abspath,
                                              FALSE, scratch_pool));
          if (!owns_lock)
            SVN_ERR(svn_wc__db_wclock_obtain(db, local_abspath, 0, FALSE,
                                             scratch_pool));
        }
    }
  else if (!is_wc_root)  /* Case 2b: It's a copy from the repository */
    {
      if (kind == svn_node_file)
        {
          /* This code should never be used, as it doesn't install proper
             pristine and/or properties. But it was not an error in the old
             version of this function.

             ===> Use svn_wc_add_repos_file4() directly! */
          svn_stream_t *content = svn_stream_empty(scratch_pool);

          SVN_ERR(svn_wc_add_repos_file4(wc_ctx, local_abspath,
                                         content, NULL, NULL, NULL,
                                         copyfrom_url, copyfrom_rev,
                                         cancel_func, cancel_baton,
                                         scratch_pool));
        }
      else
        {
          const char *repos_relpath =
            svn_path_uri_decode(svn_uri_skip_ancestor(repos_root_url,
                                                      copyfrom_url),
                                scratch_pool);

          SVN_ERR(svn_wc__db_op_copy_dir(db, local_abspath,
                                         apr_hash_make(scratch_pool),
                                         copyfrom_rev, 0, NULL,
                                         repos_relpath,
                                         repos_root_url, repos_uuid,
                                         copyfrom_rev,
                                         NULL /* children */, depth,
                                         NULL /* conflicts */,
                                         NULL /* work items */,
                                         scratch_pool));
        }
    }
  else  /* Case 1: Integrating a separate WC into this one, in place */
    {
      SVN_ERR(integrate_nested_wc_as_copy(wc_ctx, local_abspath,
                                          scratch_pool));
    }

  /* Report the addition to the caller. */
  if (notify_func != NULL)
    {
      svn_wc_notify_t *notify = svn_wc_create_notify(local_abspath,
                                                     svn_wc_notify_add,
                                                     scratch_pool);
      notify->kind = kind;
      (*notify_func)(notify_baton, notify, scratch_pool);
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_add_from_disk(svn_wc_context_t *wc_ctx,
                     const char *local_abspath,
                     svn_wc_notify_func2_t notify_func,
                     void *notify_baton,
                     apr_pool_t *scratch_pool)
{
  svn_node_kind_t kind;

  SVN_ERR(check_can_add_node(&kind, NULL, NULL, wc_ctx->db, local_abspath,
                             NULL, SVN_INVALID_REVNUM, scratch_pool));
  SVN_ERR(check_can_add_to_parent(NULL, NULL, wc_ctx->db, local_abspath,
                                  scratch_pool, scratch_pool));
  SVN_ERR(add_from_disk(wc_ctx->db, local_abspath, kind, 
                        notify_func, notify_baton,
                        scratch_pool));

  /* Report the addition to the caller. */
  if (notify_func != NULL)
    {
      svn_wc_notify_t *notify = svn_wc_create_notify(local_abspath,
                                                     svn_wc_notify_add,
                                                     scratch_pool);
      notify->kind = kind;
      (*notify_func)(notify_baton, notify, scratch_pool);
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__register_file_external(svn_wc_context_t *wc_ctx,
                               const char *local_abspath,
                               const char *external_url,
                               const svn_opt_revision_t *external_peg_rev,
                               const svn_opt_revision_t *external_rev,
                               apr_pool_t *scratch_pool)
{
  svn_wc__db_t *db = wc_ctx->db;
  const char *parent_abspath, *base_name, *repos_root_url;

  svn_dirent_split(&parent_abspath, &base_name, local_abspath, scratch_pool);

  SVN_ERR(svn_wc__db_scan_base_repos(NULL, &repos_root_url, NULL, db,
                                     parent_abspath, scratch_pool,
                                     scratch_pool));

  SVN_ERR(svn_wc__set_file_external_location(wc_ctx, local_abspath,
                                             external_url, external_peg_rev,
                                             external_rev, repos_root_url,
                                             scratch_pool));
  return SVN_NO_ERROR;
}


/* Thoughts on Reversion.

    What does is mean to revert a given PATH in a tree?  We'll
    consider things by their modifications.

    Adds

    - For files, svn_wc_remove_from_revision_control(), baby.

    - Added directories may contain nothing but added children, and
      reverting the addition of a directory necessarily means reverting
      the addition of all the directory's children.  Again,
      svn_wc_remove_from_revision_control() should do the trick.

    Deletes

    - Restore properties to their unmodified state.

    - For files, restore the pristine contents, and reset the schedule
      to 'normal'.

    - For directories, reset the schedule to 'normal'.  All children
      of a directory marked for deletion must also be marked for
      deletion, but it's okay for those children to remain deleted even
      if their parent directory is restored.  That's what the
      recursive flag is for.

    Replaces

    - Restore properties to their unmodified state.

    - For files, restore the pristine contents, and reset the schedule
      to 'normal'.

    - For directories, reset the schedule to normal.  A replaced
      directory can have deleted children (left over from the initial
      deletion), replaced children (children of the initial deletion
      now re-added), and added children (new entries under the
      replaced directory).  Since this is technically an addition, it
      necessitates recursion.

    Modifications

    - Restore properties and, for files, contents to their unmodified
      state.

*/


/* Remove conflict file CONFLICT_ABSPATH, which may not exist, and set
 * *NOTIFY_REQUIRED to TRUE if the file was present and removed. */
static svn_error_t *
remove_conflict_file(svn_boolean_t *notify_required,
                     const char *conflict_abspath,
                     const char *local_abspath,
                     apr_pool_t *scratch_pool)
{
  if (conflict_abspath)
    {
      svn_error_t *err = svn_io_remove_file2(conflict_abspath, FALSE,
                                             scratch_pool);
      if (err)
        svn_error_clear(err);
      else
        *notify_required = TRUE;
    }

  return SVN_NO_ERROR;
}

/* Make the working tree under LOCAL_ABSPATH to depth DEPTH match the
   versioned tree.  This function is called after svn_wc__db_op_revert
   has done the database revert and created the revert list.  Notifies
   for all paths equal to or below LOCAL_ABSPATH that are reverted. */
static svn_error_t *
revert_restore(svn_wc__db_t *db,
               const char *revert_root,
               const char *local_abspath,
               svn_depth_t depth,
               svn_boolean_t use_commit_times,
               svn_cancel_func_t cancel_func,
               void *cancel_baton,
               svn_wc_notify_func2_t notify_func,
               void *notify_baton,
               apr_pool_t *scratch_pool)
{
  svn_error_t *err;
  svn_wc__db_status_t status;
  svn_wc__db_kind_t kind;
  svn_node_kind_t on_disk;
  svn_boolean_t special, notify_required;
  const char *conflict_old, *conflict_new, *conflict_working, *prop_reject;


  if (cancel_func)
    SVN_ERR(cancel_func(cancel_baton));

  SVN_ERR(svn_wc__db_revert_list_read(&notify_required,
                                      &conflict_old, &conflict_new,
                                      &conflict_working, &prop_reject,
                                      db, local_abspath,
                                      scratch_pool, scratch_pool));

  err = svn_wc__db_read_info(&status, &kind,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL,
                             db, local_abspath, scratch_pool, scratch_pool);

  if (err && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
    {
      svn_error_clear(err);

      if (notify_func && notify_required)
        notify_func(notify_baton,
                    svn_wc_create_notify(local_abspath, svn_wc_notify_revert,
                                         scratch_pool),
                    scratch_pool);

      if (notify_func)
        SVN_ERR(svn_wc__db_revert_list_notify(notify_func, notify_baton,
                                              db, local_abspath, scratch_pool));

      return SVN_NO_ERROR;
    }
  else if (err)
    return svn_error_return(err);

  SVN_ERR(svn_io_check_special_path(local_abspath, &on_disk, &special,
                                    scratch_pool));

  /* If we expect a versioned item to be present then check that any
     item on disk matches the versioned item, if it doesn't match then
     fix it or delete it.  */
  if (on_disk != svn_node_none
      && status != svn_wc__db_status_absent
      && status != svn_wc__db_status_deleted
      && status != svn_wc__db_status_excluded
      && status != svn_wc__db_status_not_present)
    {
      if (on_disk == svn_node_dir && kind != svn_wc__db_kind_dir)
        {
          SVN_ERR(svn_io_remove_dir2(local_abspath, FALSE,
                                     cancel_func, cancel_baton, scratch_pool));
          on_disk = svn_node_none;
        }
      else if (on_disk == svn_node_file && kind != svn_wc__db_kind_file)
        {
          SVN_ERR(svn_io_remove_file2(local_abspath, FALSE, scratch_pool));
          on_disk = svn_node_none;
        }
      else if (on_disk == svn_node_file)
        {
          svn_boolean_t modified, executable, read_only;
          apr_hash_t *props;
          svn_string_t *special_prop;

          SVN_ERR(svn_wc__db_read_pristine_props(&props, db, local_abspath,
                                                 scratch_pool, scratch_pool));

          special_prop = apr_hash_get(props, SVN_PROP_SPECIAL,
                                      APR_HASH_KEY_STRING);

          if ((special_prop != NULL) != special)
            {
              /* File/symlink mismatch. */
              SVN_ERR(svn_io_remove_file2(local_abspath, FALSE, scratch_pool));
              on_disk = svn_node_none;
            }
          else
            {
              SVN_ERR(svn_wc__internal_file_modified_p(&modified, &executable,
                                                       &read_only,
                                                       db, local_abspath,
                                                       FALSE, FALSE,
                                                       scratch_pool));
              if (modified)
                {
                  SVN_ERR(svn_io_remove_file2(local_abspath, FALSE,
                                              scratch_pool));
                  on_disk = svn_node_none;
                }
              else
                {
                  svn_string_t *needs_lock_prop;
#if !defined(WIN32) && !defined(__OS2__)
                  svn_string_t *executable_prop;
#endif
                  needs_lock_prop = apr_hash_get(props, SVN_PROP_NEEDS_LOCK,
                                                 APR_HASH_KEY_STRING);
                  if (needs_lock_prop && !read_only)
                    {
                      SVN_ERR(svn_io_set_file_read_only(local_abspath,
                                                        FALSE, scratch_pool));
                      notify_required = TRUE;
                    }
                  else if (!needs_lock_prop && read_only)
                    {
                      SVN_ERR(svn_io_set_file_read_write(local_abspath,
                                                         FALSE, scratch_pool));
                      notify_required = TRUE;
                    }

#if !defined(WIN32) && !defined(__OS2__)
                  executable_prop = apr_hash_get(props, SVN_PROP_EXECUTABLE,
                                                 APR_HASH_KEY_STRING);
                  if (executable_prop && !executable)
                    {
                      SVN_ERR(svn_io_set_file_executable(local_abspath, TRUE,
                                                         FALSE, scratch_pool));
                      notify_required = TRUE;
                    }
                  else if (!executable_prop && executable)
                    {
                      SVN_ERR(svn_io_set_file_executable(local_abspath, FALSE,
                                                         FALSE, scratch_pool));
                      notify_required = TRUE;
                    }
#endif
                }
            }
        }
    }

  /* If we expect a versioned item to be present and there is nothing
     on disk then recreate it. */
  if (on_disk == svn_node_none
      && status != svn_wc__db_status_absent
      && status != svn_wc__db_status_deleted
      && status != svn_wc__db_status_excluded
      && status != svn_wc__db_status_not_present)
    {
      if (kind == svn_wc__db_kind_dir)
        SVN_ERR(svn_io_dir_make(local_abspath, APR_OS_DEFAULT, scratch_pool));

      if (kind == svn_wc__db_kind_file)
        {
          svn_skel_t *work_item;

          /* ### Get the checksum from read_info above and pass in here? */
          SVN_ERR(svn_wc__wq_build_file_install(&work_item, db, local_abspath,
                                                NULL, use_commit_times, TRUE,
                                                scratch_pool, scratch_pool));
          SVN_ERR(svn_wc__db_wq_add(db, local_abspath, work_item,
                                    scratch_pool));
          SVN_ERR(svn_wc__wq_run(db, local_abspath, cancel_func, cancel_baton,
                                 scratch_pool));
        }
      notify_required = TRUE;
    }

  SVN_ERR(remove_conflict_file(&notify_required, conflict_old,
                               local_abspath, scratch_pool));
  SVN_ERR(remove_conflict_file(&notify_required, conflict_new,
                               local_abspath, scratch_pool));
  SVN_ERR(remove_conflict_file(&notify_required, conflict_working,
                               local_abspath, scratch_pool));
  SVN_ERR(remove_conflict_file(&notify_required, prop_reject,
                               local_abspath, scratch_pool));

  if (notify_func && notify_required)
    notify_func(notify_baton,
                svn_wc_create_notify(local_abspath, svn_wc_notify_revert,
                                     scratch_pool),
                scratch_pool);

  if (depth == svn_depth_infinity && kind == svn_wc__db_kind_dir)
    {
      apr_pool_t *iterpool = svn_pool_create(scratch_pool);
      const apr_array_header_t *children;
      int i;

      SVN_ERR(svn_wc__db_read_children_of_working_node(&children, db,
                                                       local_abspath,
                                                       scratch_pool,
                                                       iterpool));
      for (i = 0; i < children->nelts; ++i)
        {
          const char *child_abspath;

          svn_pool_clear(iterpool);

          child_abspath = svn_dirent_join(local_abspath,
                                          APR_ARRAY_IDX(children, i,
                                                        const char *),
                                          iterpool);

          SVN_ERR(revert_restore(db, revert_root, child_abspath, depth,
                                 use_commit_times,
                                 cancel_func, cancel_baton,
                                 notify_func, notify_baton,
                                 iterpool));
        }

      svn_pool_destroy(iterpool);
    }

  if (notify_func)
    SVN_ERR(svn_wc__db_revert_list_notify(notify_func, notify_baton,
                                          db, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
}

/* Revert tree LOCAL_ABSPATH to depth DEPTH and notify for all
   reverts. */
static svn_error_t *
new_revert_internal(svn_wc__db_t *db,
                    const char *revert_root,
                    const char *local_abspath,
                    svn_depth_t depth,
                    svn_boolean_t use_commit_times,
                    svn_cancel_func_t cancel_func,
                    void *cancel_baton,
                    svn_wc_notify_func2_t notify_func,
                    void *notify_baton,
                    apr_pool_t *scratch_pool)
{
  SVN_ERR_ASSERT(depth == svn_depth_empty || depth == svn_depth_infinity);

  SVN_ERR(svn_wc__db_op_revert(db, local_abspath, depth,
                               scratch_pool, scratch_pool));

  SVN_ERR(revert_restore(db, revert_root, local_abspath, depth,
                         use_commit_times,
                         cancel_func, cancel_baton,
                         notify_func, notify_baton,
                         scratch_pool));

  return SVN_NO_ERROR;
}

/* Revert files in LOCAL_ABSPATH to depth DEPTH that match
   CHANGELIST_HASH and notify for all reverts. */
static svn_error_t *
new_revert_changelist(svn_wc__db_t *db,
                      const char *revert_root,
                      const char *local_abspath,
                      svn_depth_t depth,
                      svn_boolean_t use_commit_times,
                      apr_hash_t *changelist_hash,
                      svn_cancel_func_t cancel_func,
                      void *cancel_baton,
                      svn_wc_notify_func2_t notify_func,
                      void *notify_baton,
                      apr_pool_t *scratch_pool)
{
  apr_pool_t *iterpool;
  const apr_array_header_t *children;
  int i;

  if (cancel_func)
    SVN_ERR(cancel_func(cancel_baton));

  if (svn_wc__internal_changelist_match(db, local_abspath, changelist_hash,
                                        scratch_pool))
    SVN_ERR(new_revert_internal(db, revert_root, local_abspath,
                                svn_depth_empty, use_commit_times,
                                cancel_func, cancel_baton, notify_func,
                                notify_baton, scratch_pool));

  if (depth == svn_depth_empty)
    return SVN_NO_ERROR;

  iterpool = svn_pool_create(scratch_pool);

  /* We can handle both depth=files and depth=immediates by setting
     depth=empty here.  We don't need to distinguish files and
     directories when making the recursive call because directories
     can never match a changelist, so making the recursive call for
     directories when asked for depth=files is a no-op. */
  if (depth == svn_depth_files || depth == svn_depth_immediates)
    depth = svn_depth_empty;

  SVN_ERR(svn_wc__db_read_children_of_working_node(&children, db,
                                                   local_abspath,
                                                   scratch_pool,
                                                   iterpool));
  for (i = 0; i < children->nelts; ++i)
    {
      const char *child_abspath;

      svn_pool_clear(iterpool);

      child_abspath = svn_dirent_join(local_abspath,
                                      APR_ARRAY_IDX(children, i,
                                                    const char *),
                                      iterpool);

      SVN_ERR(new_revert_changelist(db, revert_root, child_abspath, depth,
                                    use_commit_times, changelist_hash,
                                    cancel_func, cancel_baton,
                                    notify_func, notify_baton,
                                    iterpool));
    }

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

/* Does a partially recursive revert of LOCAL_ABSPATH to depth DEPTH
   (which must be either svn_depth_files or svn_depth_immediates) by
   doing a non-recursive revert on each permissible path.  Notifies
   all reverted paths.

   ### This won't revert a copied dir with one level of children since
   ### the non-recursive revert on the dir will fail.  Not sure how a
   ### partially recursive revert should handle actual-only nodes. */
static svn_error_t *
new_revert_partial(svn_wc__db_t *db,
                   const char *revert_root,
                   const char *local_abspath,
                   svn_depth_t depth,
                   svn_boolean_t use_commit_times,
                   svn_cancel_func_t cancel_func,
                   void *cancel_baton,
                   svn_wc_notify_func2_t notify_func,
                   void *notify_baton,
                   apr_pool_t *scratch_pool)
{
  apr_pool_t *iterpool;
  svn_wc__db_kind_t kind;
  const apr_array_header_t *children;
  svn_boolean_t is_revert_root = !strcmp(local_abspath, revert_root);
  int i;

  SVN_ERR_ASSERT(depth == svn_depth_files || depth == svn_depth_immediates);

  if (cancel_func)
    SVN_ERR(cancel_func(cancel_baton));

  SVN_ERR(svn_wc__db_read_info(NULL, &kind,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL,
                               db, local_abspath, scratch_pool, scratch_pool));

  if (is_revert_root || depth == svn_depth_immediates
      || (depth == svn_depth_files && kind == svn_wc__db_kind_file))
    SVN_ERR(new_revert_internal(db, revert_root, local_abspath, svn_depth_empty,
                                use_commit_times, cancel_func, cancel_baton,
                                notify_func, notify_baton, scratch_pool));

  if (!is_revert_root)
    return SVN_NO_ERROR;

  iterpool = svn_pool_create(scratch_pool);

  SVN_ERR(svn_wc__db_read_children_of_working_node(&children, db,
                                                   local_abspath,
                                                   scratch_pool,
                                                   iterpool));
  for (i = 0; i < children->nelts; ++i)
    {
      const char *child_abspath;

      svn_pool_clear(iterpool);

      child_abspath = svn_dirent_join(local_abspath,
                                      APR_ARRAY_IDX(children, i,
                                                    const char *),
                                      iterpool);

      SVN_ERR(new_revert_partial(db, revert_root, child_abspath, depth,
                                 use_commit_times,
                                 cancel_func, cancel_baton,
                                 notify_func, notify_baton,
                                 iterpool));
    }

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

/* This is just the guts of svn_wc_revert4() save that it accepts a
   hash CHANGELIST_HASH whose keys are changelist names instead of an
   array of said names.  See svn_wc_revert4() for additional
   documentation. */
static svn_error_t *
revert_internal(svn_wc__db_t *db,
                const char *revert_root,
                const char *local_abspath,
                svn_depth_t depth,
                svn_boolean_t use_commit_times,
                apr_hash_t *changelist_hash,
                svn_cancel_func_t cancel_func,
                void *cancel_baton,
                svn_wc_notify_func2_t notify_func,
                void *notify_baton,
                apr_pool_t *pool)
{
  if (changelist_hash)
    return svn_error_return(new_revert_changelist(db, revert_root,
                                                  local_abspath, depth,
                                                  use_commit_times,
                                                  changelist_hash,
                                                  cancel_func, cancel_baton,
                                                  notify_func, notify_baton,
                                                  pool));

  if (depth == svn_depth_empty || depth == svn_depth_infinity)
    return svn_error_return(new_revert_internal(db, revert_root, local_abspath,
                                                depth, use_commit_times,
                                                cancel_func, cancel_baton,
                                                notify_func, notify_baton,
                                                pool));

  /* The user may expect svn_depth_files/svn_depth_immediates to work
     on copied dirs with one level of children.  It doesn't, the user
     will get an error and will need to invoke an infinite revert.  If
     we identified those cases where svn_depth_infinity would not
     revert too much we could invoke the recursive call above. */

  if (depth == svn_depth_files || depth == svn_depth_immediates)
    return svn_error_return(new_revert_partial(db, revert_root, local_abspath,
                                               depth, use_commit_times,
                                               cancel_func, cancel_baton,
                                               notify_func, notify_baton,
                                               pool));

  /* Other depths, throw an error? */
  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_revert4(svn_wc_context_t *wc_ctx,
               const char *local_abspath,
               svn_depth_t depth,
               svn_boolean_t use_commit_times,
               const apr_array_header_t *changelists,
               svn_cancel_func_t cancel_func,
               void *cancel_baton,
               svn_wc_notify_func2_t notify_func,
               void *notify_baton,
               apr_pool_t *pool)
{
  apr_hash_t *changelist_hash = NULL;

  if (changelists && changelists->nelts)
    SVN_ERR(svn_hash_from_cstring_keys(&changelist_hash, changelists, pool));

  return svn_error_return(revert_internal(wc_ctx->db,
                                          local_abspath, local_abspath, depth,
                                          use_commit_times, changelist_hash,
                                          cancel_func, cancel_baton,
                                          notify_func, notify_baton,
                                          pool));
}


svn_error_t *
svn_wc_get_pristine_copy_path(const char *path,
                              const char **pristine_path,
                              apr_pool_t *pool)
{
  svn_wc__db_t *db;
  const char *local_abspath;
  svn_error_t *err;

  SVN_ERR(svn_wc__db_open(&db, NULL, TRUE, TRUE, pool, pool));
  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));

  err = svn_wc__text_base_path_to_read(pristine_path, db, local_abspath,
                                       pool, pool);
  if (err && err->apr_err == SVN_ERR_WC_PATH_UNEXPECTED_STATUS)
    {
      const char *adm_abspath = svn_dirent_dirname(local_abspath, pool);

      svn_error_clear(err);
      *pristine_path = svn_wc__nonexistent_path(db, adm_abspath, pool);
      return SVN_NO_ERROR;
    }
   SVN_ERR(err);

  return svn_error_return(svn_wc__db_close(db));
}

svn_error_t *
svn_wc_get_pristine_contents2(svn_stream_t **contents,
                              svn_wc_context_t *wc_ctx,
                              const char *local_abspath,
                              apr_pool_t *result_pool,
                              apr_pool_t *scratch_pool)
{
  return svn_error_return(svn_wc__get_pristine_contents(contents,
                                                        wc_ctx->db,
                                                        local_abspath,
                                                        result_pool,
                                                        scratch_pool));
}

svn_error_t *
svn_wc__internal_remove_from_revision_control(svn_wc__db_t *db,
                                              const char *local_abspath,
                                              svn_boolean_t destroy_wf,
                                              svn_boolean_t instant_error,
                                              svn_cancel_func_t cancel_func,
                                              void *cancel_baton,
                                              apr_pool_t *scratch_pool)
{
  svn_error_t *err;
  svn_boolean_t left_something = FALSE;
  svn_wc__db_status_t status;
  svn_wc__db_kind_t kind;

  /* ### This whole function should be rewritten to run inside a transaction,
     ### to allow a stable cancel behavior.
     ###
     ### Subversion < 1.7 marked the directory as incomplete to allow updating
     ### it from a canceled state. But this would not work because update
     ### doesn't retrieve deleted items.
     ###
     ### WC-NG doesn't support a delete+incomplete state, but we can't build
     ### transactions over multiple databases yet. */

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  /* Check cancellation here, so recursive calls get checked early. */
  if (cancel_func)
    SVN_ERR(cancel_func(cancel_baton));

  SVN_ERR(svn_wc__db_read_info(&status, &kind, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL,
                               db, local_abspath, scratch_pool, scratch_pool));

  if (kind == svn_wc__db_kind_file || kind == svn_wc__db_kind_symlink)
    {
      svn_node_kind_t on_disk;
      svn_boolean_t wc_special, local_special;
      svn_boolean_t text_modified_p;
      const svn_checksum_t *base_sha1_checksum, *working_sha1_checksum;

      /* Only check if the file was modified when it wasn't overwritten with a
         special file */
      SVN_ERR(svn_wc__get_translate_info(NULL, NULL, NULL,
                                         &wc_special, db, local_abspath, NULL,
                                         scratch_pool, scratch_pool));
      SVN_ERR(svn_io_check_special_path(local_abspath, &on_disk,
                                        &local_special, scratch_pool));
      if (wc_special || ! local_special)
        {
          /* Check for local mods. before removing entry */
          SVN_ERR(svn_wc__internal_text_modified_p(&text_modified_p, db,
                                                   local_abspath, FALSE,
                                                   TRUE, scratch_pool));
          if (text_modified_p && instant_error)
            return svn_error_createf(SVN_ERR_WC_LEFT_LOCAL_MOD, NULL,
                   _("File '%s' has local modifications"),
                   svn_dirent_local_style(local_abspath, scratch_pool));
        }

      /* Find the checksum(s) of the node's one or two pristine texts.  Note
         that read_info() may give us the one from BASE_NODE again. */
      err = svn_wc__db_base_get_info(NULL, NULL, NULL, NULL, NULL, NULL,
                                     NULL, NULL, NULL, NULL, NULL,
                                     &base_sha1_checksum,
                                     NULL, NULL, NULL, NULL,
                                     db, local_abspath,
                                     scratch_pool, scratch_pool);
      if (err && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
        {
          svn_error_clear(err);
          base_sha1_checksum = NULL;
        }
      else
        SVN_ERR(err);
      err = svn_wc__db_read_info(NULL, NULL, NULL, NULL, NULL, NULL,
                                 NULL, NULL, NULL, NULL, NULL,
                                 &working_sha1_checksum,
                                 NULL, NULL, NULL, NULL, NULL, NULL,
                                 NULL, NULL, NULL, NULL, NULL, NULL,
                                 db, local_abspath,
                                 scratch_pool, scratch_pool);
      if (err && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
        {
          svn_error_clear(err);
          working_sha1_checksum = NULL;
        }
      else
        SVN_ERR(err);

      /* Remove NAME from PATH's entries file: */
      SVN_ERR(svn_wc__db_temp_op_remove_entry(db, local_abspath,
                                              scratch_pool));

      /* Having removed the checksums that reference the pristine texts,
         remove the pristine texts (if now totally unreferenced) from the
         pristine store.  Don't try to remove the same pristine text twice.
         The two checksums might be the same, either because the copied base
         was exactly the same as the replaced base, or just because the
         ..._read_info() code above sets WORKING_SHA1_CHECKSUM to the base
         checksum if there is no WORKING_NODE row. */
      if (base_sha1_checksum)
        SVN_ERR(svn_wc__db_pristine_remove(db, local_abspath,
                                           base_sha1_checksum,
                                           scratch_pool));
      if (working_sha1_checksum
          && ! svn_checksum_match(base_sha1_checksum, working_sha1_checksum))
        SVN_ERR(svn_wc__db_pristine_remove(db, local_abspath,
                                           working_sha1_checksum,
                                           scratch_pool));

      /* If we were asked to destroy the working file, do so unless
         it has local mods. */
      if (destroy_wf)
        {
          /* Don't kill local mods. */
          if ((! wc_special && local_special) || text_modified_p)
            return svn_error_create(SVN_ERR_WC_LEFT_LOCAL_MOD, NULL, NULL);
          else  /* The working file is still present; remove it. */
            SVN_ERR(svn_io_remove_file2(local_abspath, TRUE, scratch_pool));
        }

    }  /* done with file case */
  else /* looking at THIS_DIR */
    {
      apr_pool_t *iterpool = svn_pool_create(scratch_pool);
      const apr_array_header_t *children;
      int i;

      /* ### sanity check:  check 2 places for DELETED flag? */

      /* Walk over every entry. */
      SVN_ERR(svn_wc__db_read_children(&children, db, local_abspath,
                                       scratch_pool, iterpool));

      for (i = 0; i < children->nelts; i++)
        {
          const char *entry_name = APR_ARRAY_IDX(children, i, const char*);
          const char *entry_abspath;
          svn_boolean_t hidden;

          svn_pool_clear(iterpool);

          entry_abspath = svn_dirent_join(local_abspath, entry_name, iterpool);

          /* ### where did the adm_missing and depth_exclude test go?!?

             ### BH: depth exclude is handled by hidden and missing is ok
                     for this temp_op. */

          SVN_ERR(svn_wc__db_node_hidden(&hidden, db, entry_abspath,
                                         iterpool));
          if (hidden)
            {
              SVN_ERR(svn_wc__db_temp_op_remove_entry(db, entry_abspath,
                                                      iterpool));
              continue;
            }

          err = svn_wc__internal_remove_from_revision_control(
            db, entry_abspath,
            destroy_wf, instant_error,
            cancel_func, cancel_baton,
            iterpool);

          if (err && (err->apr_err == SVN_ERR_WC_LEFT_LOCAL_MOD))
            {
              if (instant_error)
                return svn_error_return(err);
              else
                {
                  svn_error_clear(err);
                  left_something = TRUE;
                }
            }
          else if (err)
            return svn_error_return(err);
        }

      /* At this point, every directory below this one has been
         removed from revision control. */

      /* Remove self from parent's entries file, but only if parent is
         a working copy.  If it's not, that's fine, we just move on. */
      {
        svn_boolean_t is_root;

        SVN_ERR(svn_wc__check_wc_root(&is_root, NULL, NULL,
                                      db, local_abspath, iterpool));

        /* If full_path is not the top of a wc, then its parent
           directory is also a working copy and has an entry for
           full_path.  We need to remove that entry: */
        if (! is_root)
          {
            SVN_ERR(svn_wc__db_temp_op_remove_entry(db, local_abspath,
                                                    iterpool));
          }
      }

      /* Remove the entire administrative .svn area, thereby removing
         _this_ dir from revision control too.  */
      SVN_ERR(svn_wc__adm_destroy(db, local_abspath,
                                  cancel_func, cancel_baton, iterpool));

      /* If caller wants us to recursively nuke everything on disk, go
         ahead, provided that there are no dangling local-mod files
         below */
      if (destroy_wf && (! left_something))
        {
          /* If the dir is *truly* empty (i.e. has no unversioned
             resources, all versioned files are gone, all .svn dirs are
             gone, and contains nothing but empty dirs), then a
             *non*-recursive dir_remove should work.  If it doesn't,
             no big deal.  Just assume there are unversioned items in
             there and set "left_something" */
          err = svn_io_dir_remove_nonrecursive(local_abspath, iterpool);
          if (err)
            {
              if (!APR_STATUS_IS_ENOENT(err->apr_err))
                left_something = TRUE;
              svn_error_clear(err);
            }
        }

      svn_pool_destroy(iterpool);

    }  /* end of directory case */

  if (left_something)
    return svn_error_create(SVN_ERR_WC_LEFT_LOCAL_MOD, NULL, NULL);
  else
    return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_remove_from_revision_control2(svn_wc_context_t *wc_ctx,
                                    const char *local_abspath,
                                    svn_boolean_t destroy_wf,
                                    svn_boolean_t instant_error,
                                    svn_cancel_func_t cancel_func,
                                    void *cancel_baton,
                                    apr_pool_t *scratch_pool)
{
  return svn_error_return(
      svn_wc__internal_remove_from_revision_control(wc_ctx->db,
                                                    local_abspath,
                                                    destroy_wf,
                                                    instant_error,
                                                    cancel_func,
                                                    cancel_baton,
                                                    scratch_pool));
}

svn_error_t *
svn_wc_add_lock2(svn_wc_context_t *wc_ctx,
                 const char *local_abspath,
                 const svn_lock_t *lock,
                 apr_pool_t *scratch_pool)
{
  svn_wc__db_lock_t db_lock;
  svn_error_t *err;
  const svn_string_t *needs_lock;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  db_lock.token = lock->token;
  db_lock.owner = lock->owner;
  db_lock.comment = lock->comment;
  db_lock.date = lock->creation_date;
  err = svn_wc__db_lock_add(wc_ctx->db, local_abspath, &db_lock, scratch_pool);
  if (err)
    {
      if (err->apr_err != SVN_ERR_WC_PATH_NOT_FOUND)
        return svn_error_return(err);

      /* Remap the error.  */
      svn_error_clear(err);
      return svn_error_createf(SVN_ERR_ENTRY_NOT_FOUND, NULL,
                               _("'%s' is not under version control"),
                               svn_dirent_local_style(local_abspath,
                                                      scratch_pool));
    }

  /* if svn:needs-lock is present, then make the file read-write. */
  SVN_ERR(svn_wc__internal_propget(&needs_lock, wc_ctx->db, local_abspath,
                                   SVN_PROP_NEEDS_LOCK, scratch_pool,
                                   scratch_pool));
  if (needs_lock)
    SVN_ERR(svn_io_set_file_read_write(local_abspath, FALSE, scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_remove_lock2(svn_wc_context_t *wc_ctx,
                    const char *local_abspath,
                    apr_pool_t *scratch_pool)
{
  svn_error_t *err;
  const svn_string_t *needs_lock;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  err = svn_wc__db_lock_remove(wc_ctx->db, local_abspath, scratch_pool);
  if (err)
    {
      if (err->apr_err != SVN_ERR_WC_PATH_NOT_FOUND)
        return svn_error_return(err);

      /* Remap the error.  */
      svn_error_clear(err);
      return svn_error_createf(SVN_ERR_ENTRY_NOT_FOUND, NULL,
                               _("'%s' is not under version control"),
                               svn_dirent_local_style(local_abspath,
                                                      scratch_pool));
    }

  /* if svn:needs-lock is present, then make the file read-only. */
  SVN_ERR(svn_wc__internal_propget(&needs_lock, wc_ctx->db, local_abspath,
                                   SVN_PROP_NEEDS_LOCK, scratch_pool,
                                   scratch_pool));
  if (needs_lock)
    SVN_ERR(svn_io_set_file_read_only(local_abspath, FALSE, scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_set_changelist2(svn_wc_context_t *wc_ctx,
                       const char *local_abspath,
                       const char *changelist,
                       const apr_array_header_t *changelists,
                       svn_cancel_func_t cancel_func,
                       void *cancel_baton,
                       svn_wc_notify_func2_t notify_func,
                       void *notify_baton,
                       apr_pool_t *scratch_pool)
{
  const char *existing_changelist;
  svn_wc__db_kind_t kind;

  /* Assert that we aren't being asked to set an empty changelist. */
  SVN_ERR_ASSERT(! (changelist && changelist[0] == '\0'));

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_read_info(NULL, &kind, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               &existing_changelist,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL,
                               wc_ctx->db, local_abspath, scratch_pool,
                               scratch_pool));

  /* We can't add directories to changelists. */
  if (kind == svn_wc__db_kind_dir && changelist)
    return svn_error_createf(SVN_ERR_CLIENT_IS_DIRECTORY, NULL,
                             _("'%s' is a directory, and thus cannot"
                               " be a member of a changelist"), local_abspath);

  /* Set the changelist. */
  SVN_ERR(svn_wc__db_op_set_changelist(wc_ctx->db, local_abspath, changelist,
                                       changelists, svn_depth_empty,
                                       scratch_pool));

  /* And tell someone what we've done. */
  if (notify_func)
    SVN_ERR(svn_wc__db_changelist_list_notify(notify_func, notify_baton,
                                              wc_ctx->db, local_abspath,
                                              scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__set_file_external_location(svn_wc_context_t *wc_ctx,
                                   const char *local_abspath,
                                   const char *url,
                                   const svn_opt_revision_t *peg_rev,
                                   const svn_opt_revision_t *rev,
                                   const char *repos_root_url,
                                   apr_pool_t *scratch_pool)
{
  const char *external_repos_relpath;
  const svn_opt_revision_t unspecified_rev = { svn_opt_revision_unspecified };

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(!url || svn_uri_is_canonical(url, scratch_pool));

  if (url)
    {
      external_repos_relpath = svn_uri_is_child(repos_root_url, url,
                                                scratch_pool);

      if (external_repos_relpath == NULL)
          return svn_error_createf(SVN_ERR_ILLEGAL_TARGET, NULL,
                                   _("Can't add a file external to '%s' as it"
                                     " is not a file in repository '%s'."),
                                   url, repos_root_url);

      SVN_ERR_ASSERT(peg_rev != NULL);
      SVN_ERR_ASSERT(rev != NULL);
    }
  else
    {
      external_repos_relpath = NULL;
      peg_rev = &unspecified_rev;
      rev = &unspecified_rev;
    }

  SVN_ERR(svn_wc__db_temp_op_set_file_external(wc_ctx->db, local_abspath,
                                               external_repos_relpath, peg_rev,
                                               rev, scratch_pool));

  return SVN_NO_ERROR;
}


svn_boolean_t
svn_wc__internal_changelist_match(svn_wc__db_t *db,
                                  const char *local_abspath,
                                  apr_hash_t *clhash,
                                  apr_pool_t *scratch_pool)
{
  svn_error_t *err;
  const char *changelist;

  if (clhash == NULL)
    return TRUE;

  err = svn_wc__db_read_info(NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             &changelist,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL,
                             db, local_abspath, scratch_pool, scratch_pool);

  if (err)
    {
      svn_error_clear(err);
      return FALSE;
    }

  return (changelist
            && apr_hash_get(clhash, changelist, APR_HASH_KEY_STRING) != NULL);
}

svn_boolean_t
svn_wc__changelist_match(svn_wc_context_t *wc_ctx,
                         const char *local_abspath,
                         apr_hash_t *clhash,
                         apr_pool_t *scratch_pool)
{
  return svn_wc__internal_changelist_match(wc_ctx->db, local_abspath, clhash,
                                           scratch_pool);
}
