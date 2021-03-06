// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2010-2011 Dreamhost
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "common/DoutStreambuf.h"
#include "common/ceph_argparse.h"
#include "common/ceph_context.h"
#include "common/ceph_crypto.h"
#include "common/code_environment.h"
#include "common/common_init.h"
#include "common/config.h"
#include "common/dout.h"
#include "common/errno.h"
#include "common/safe_io.h"
#include "common/version.h"
#include "include/color.h"

#include <errno.h>
#include <deque>

#define _STR(x) #x
#define STRINGIFY(x) _STR(x)

CephContext *common_preinit(const CephInitParameters &iparams,
			  enum code_environment_t code_env, int flags)
{
  // set code environment
  g_code_env = code_env;

  // Create a configuration object
  CephContext *cct = new CephContext(iparams.module_type);

  md_config_t *conf = cct->_conf;
  // add config observers here

  // Set up our entity name.
  conf->name = iparams.name;

  // Set some defaults based on code type
  switch (code_env) {
  case CODE_ENVIRONMENT_DAEMON:
    conf->set_val_or_die("daemonize", "true");
    if (!(flags & CINIT_FLAG_UNPRIVILEGED_DAEMON_DEFAULTS)) {
      conf->set_val_or_die("pid_file", "/var/run/ceph/$type.$id.pid");
      conf->set_val_or_die("admin_socket", "/var/run/ceph/$name.asok");
      conf->set_val_or_die("log_file", "/var/log/ceph/$name.log");
    }
    conf->set_val_or_die("log_to_stderr", "false");
    conf->set_val_or_die("err_to_stderr", "true");
    break;

  case CODE_ENVIRONMENT_LIBRARY:
    conf->set_val_or_die("log_to_stderr", "false");
    conf->set_val_or_die("err_to_stderr", "false");
    break;

  default:
    break;
  }
  return cct;
}

void complain_about_parse_errors(CephContext *cct,
				 std::deque<std::string> *parse_errors)
{
  if (parse_errors->empty())
    return;
  lderr(cct) << "Errors while parsing config file!" << dendl;
  int cur_err = 0;
  static const int MAX_PARSE_ERRORS = 20;
  for (std::deque<std::string>::const_iterator p = parse_errors->begin();
       p != parse_errors->end(); ++p)
  {
    lderr(cct) << *p << dendl;
    if (cur_err == MAX_PARSE_ERRORS) {
      lderr(cct) << "Suppressed " << (parse_errors->size() - MAX_PARSE_ERRORS)
	   << " more errors." << dendl;
      break;
    }
    ++cur_err;
  }
}

/* Please be sure that this can safely be called multiple times by the
 * same application. */
void common_init_finish(CephContext *cct)
{
  ceph::crypto::init();
  cct->start_service_thread();

  // Trigger callbacks on any config observers that were waiting for
  // it to become safe to start threads.
  cct->_conf->set_val("internal_safe_to_start_threads", "true");
  cct->_conf->call_all_observers();
}

void common_destroy_context(CephContext *cct)
{
  //delete cct;        // TODO: fix #845
}
