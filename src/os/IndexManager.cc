// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include <tr1/memory>
#include <map>

#if defined(__FreeBSD__)
#include <sys/param.h>
#endif

#include "common/Mutex.h"
#include "common/Cond.h"
#include "common/config.h"
#include "common/debug.h"
#include "include/buffer.h"

#include "IndexManager.h"
#include "FlatIndex.h"
#include "HashIndex.h"
#include "FlatIndex.h"
#include "CollectionIndex.h"

int do_getxattr(const char *fn, const char *name, void *val, size_t size);
int do_setxattr(const char *fn, const char *name, const void *val, size_t size);

static int set_version(const char *path, uint32_t version) {
  bufferlist bl;
  ::encode(version, bl);
  return do_setxattr(path, "user.cephos.collection_version", bl.c_str(), 
		     bl.length());
}

static int get_version(const char *path, uint32_t *version) {
  bufferptr bp(PATH_MAX);
  int r = do_getxattr(path, "user.cephos.collection_version", 
		      bp.c_str(), bp.length());
  if (r < 0) {
    if (r != -ENOENT) {
      *version = 0;
      return 0;
    } else {
      return r;
    }
  }
  bp.set_length(r);
  bufferlist bl;
  bl.push_back(bp);
  bufferlist::iterator i = bl.begin();
  ::decode(*version, i);
  return 0;
}

void IndexManager::put_index(coll_t c) {
  Mutex::Locker l(lock);
  assert(col_indices.count(c));
  col_indices.erase(c);
  cond.Signal();
}

int IndexManager::init_index(coll_t c, const char *path, uint32_t version) {
  Mutex::Locker l(lock);
  int r = set_version(path, version);
  if (r < 0)
    return r;
  HashIndex index(c, path, g_conf->filestore_merge_threshold,
		  g_conf->filestore_split_multiple,
		  CollectionIndex::HASH_INDEX_TAG_2);
  return index.init();
}

int IndexManager::build_index(coll_t c, const char *path, Index *index) {
  int r;
  if (g_conf->filestore_update_collections) {
    // Need to check the collection generation
    uint32_t version = 0;
    r = get_version(path, &version);
    if (r < 0)
      return r;

    switch (version) {
    case CollectionIndex::FLAT_INDEX_TAG: {
      *index = Index(new FlatIndex(c, path),
		     RemoveOnDelete(c, this));
      return 0;
    }
    case CollectionIndex::HASH_INDEX_TAG: // fall through
    case CollectionIndex::HASH_INDEX_TAG_2: {
      // Must be a HashIndex
      *index = Index(new HashIndex(c, path, g_conf->filestore_merge_threshold,
				   g_conf->filestore_split_multiple, version), 
		     RemoveOnDelete(c, this));
      return 0;
    }
    default: assert(0);
    }

  } else {
    // No need to check
    *index = Index(new HashIndex(c, path, g_conf->filestore_merge_threshold,
				 g_conf->filestore_split_multiple,
				 CollectionIndex::HASH_INDEX_TAG_2), 
		   RemoveOnDelete(c, this));
    return 0;
  }
}

int IndexManager::get_index(coll_t c, const char *path, Index *index) {
  Mutex::Locker l(lock);
  while (1) {
    if (!col_indices.count(c)) {
      int r = build_index(c, path, index);
      if (r < 0)
	return r;
      (*index)->set_ref(*index);
      col_indices[c] = (*index);
      break;
    } else {
      cond.Wait(lock);
    }
  }
  return 0;
}
