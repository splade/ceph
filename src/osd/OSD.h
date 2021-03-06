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

#ifndef CEPH_OSD_H
#define CEPH_OSD_H

#include "PG.h"

#include "msg/Dispatcher.h"

#include "common/Mutex.h"
#include "common/RWLock.h"
#include "common/Timer.h"
#include "common/WorkQueue.h"
#include "common/LogClient.h"

#include "os/ObjectStore.h"
#include "OSDCaps.h"

#include "common/DecayCounter.h"
#include "osd/ClassHandler.h"

#include "include/CompatSet.h"

#include "auth/KeyRing.h"
#include "messages/MOSDRepScrub.h"

#include <map>
#include <memory>
#include <tr1/memory>
using namespace std;

#include <ext/hash_map>
#include <ext/hash_set>
using namespace __gnu_cxx;


#define CEPH_OSD_PROTOCOL     9 /* cluster internal */


enum {
  l_osd_first = 10000,
  l_osd_opq,
  l_osd_op_wip,
  l_osd_op,
  l_osd_op_inb,
  l_osd_op_outb,
  l_osd_op_lat,
  l_osd_op_r,
  l_osd_op_r_outb,
  l_osd_op_r_lat,
  l_osd_op_w,
  l_osd_op_w_inb,
  l_osd_op_w_rlat,
  l_osd_op_w_lat,
  l_osd_op_rw,
  l_osd_op_rw_inb,
  l_osd_op_rw_outb,
  l_osd_op_rw_rlat,
  l_osd_op_rw_lat,

  l_osd_sop,
  l_osd_sop_inb,
  l_osd_sop_lat,
  l_osd_sop_w,
  l_osd_sop_w_inb,
  l_osd_sop_w_lat,
  l_osd_sop_pull,
  l_osd_sop_pull_lat,
  l_osd_sop_push,
  l_osd_sop_push_inb,
  l_osd_sop_push_lat,

  l_osd_pull,
  l_osd_push,
  l_osd_push_outb,

  l_osd_rop,

  l_osd_loadavg,
  l_osd_buf,

  l_osd_pg,
  l_osd_pg_primary,
  l_osd_pg_replica,
  l_osd_pg_stray,
  l_osd_hb_to,
  l_osd_hb_from,
  l_osd_map,
  l_osd_mape,
  l_osd_mape_dup,

  l_osd_last,
};

class Messenger;
class Message;
class MonClient;
class PerfCounters;
class ObjectStore;
class OSDMap;
class MLog;
class MClass;
class MOSDPGMissing;

class Watch;
class Notification;
class ReplicatedPG;

class AuthAuthorizeHandlerRegistry;

class OpRequest;
class OpsFlightSocketHook;

extern const coll_t meta_coll;

class OSD : public Dispatcher {
  /** OSD **/
protected:
  Mutex osd_lock;			// global lock
  SafeTimer timer;    // safe timer (osd_lock)

  AuthAuthorizeHandlerRegistry *authorize_handler_registry;

  Messenger   *cluster_messenger;
  Messenger   *client_messenger;
  MonClient   *monc;
  PerfCounters      *logger;
  ObjectStore *store;

  // cover OSDMap update data when using multiple msgrs
  Cond *map_in_progress_cond;
  bool map_in_progress;

  LogClient clog;

  int whoami;
  std::string dev_path, journal_path;

  class C_Tick : public Context {
    OSD *osd;
  public:
    C_Tick(OSD *o) : osd(o) {}
    void finish(int r) {
      osd->tick();
    }
  };

  Cond dispatch_cond;
  int dispatch_running;

  void create_logger();
  void tick();
  void _dispatch(Message *m);
  void dispatch_op(OpRequest *op);

public:
  ClassHandler  *class_handler;
  int get_nodeid() { return whoami; }
  
  static hobject_t get_osdmap_pobject_name(epoch_t epoch) { 
    char foo[20];
    snprintf(foo, sizeof(foo), "osdmap.%d", epoch);
    return hobject_t(sobject_t(object_t(foo), 0)); 
  }
  static hobject_t get_inc_osdmap_pobject_name(epoch_t epoch) { 
    char foo[20];
    snprintf(foo, sizeof(foo), "inc_osdmap.%d", epoch);
    return hobject_t(sobject_t(object_t(foo), 0)); 
  }

  hobject_t make_pg_log_oid(pg_t pg) {
    stringstream ss;
    ss << "pglog_" << pg;
    string s;
    getline(ss, s);
    return hobject_t(sobject_t(object_t(s.c_str()), 0));
  }
  
  hobject_t make_pg_biginfo_oid(pg_t pg) {
    stringstream ss;
    ss << "pginfo_" << pg;
    string s;
    getline(ss, s);
    return hobject_t(sobject_t(object_t(s.c_str()), 0));
  }
  

private:
  // -- superblock --
  OSDSuperblock superblock;

  void write_superblock();
  void write_superblock(ObjectStore::Transaction& t);
  int read_superblock();

  CompatSet osd_compat;

  // -- state --
public:
  static const int STATE_BOOTING = 1;
  static const int STATE_ACTIVE = 2;
  static const int STATE_STOPPING = 3;

private:
  int state;
  epoch_t boot_epoch;  // _first_ epoch we were marked up (after this process started)
  epoch_t up_epoch;    // _most_recent_ epoch we were marked up
  epoch_t bind_epoch;  // epoch we last did a bind to new ip:ports

public:
  bool is_booting() { return state == STATE_BOOTING; }
  bool is_active() { return state == STATE_ACTIVE; }
  bool is_stopping() { return state == STATE_STOPPING; }

private:

  ThreadPool op_tp;
  ThreadPool recovery_tp;
  ThreadPool disk_tp;
  ThreadPool command_tp;

  // -- sessions --
public:
  struct Session : public RefCountedObject {
    EntityName entity_name;
    OSDCaps caps;
    epoch_t last_sent_epoch;
    Connection *con;
    std::map<void *, pg_t> watches;
    std::map<void *, entity_name_t> notifs;

    Session() : last_sent_epoch(0), con(0) {}
    void add_notif(void *n, entity_name_t& name) {
      notifs[n] = name;
    }
    void del_notif(void *n) {
      std::map<void *, entity_name_t>::iterator iter = notifs.find(n);
      if (iter != notifs.end())
        notifs.erase(iter);
    }
  };

private:
  // -- heartbeat --
  Mutex heartbeat_lock;
  Cond heartbeat_cond;
  bool heartbeat_stop;
  epoch_t heartbeat_epoch;
  map<int, epoch_t> heartbeat_to, heartbeat_from;
  map<int, utime_t> heartbeat_from_stamp;
  map<int, Connection*> heartbeat_to_con, heartbeat_from_con;
  utime_t last_mon_heartbeat;
  Messenger *hbin_messenger, *hbout_messenger;
  
  void _add_heartbeat_source(int p, map<int, epoch_t>& old_from, map<int, utime_t>& old_from_stamp,
			     map<int,Connection*>& old_con);
  void update_heartbeat_peers();
  void reset_heartbeat_peers();
  void heartbeat();
  void heartbeat_check();
  void heartbeat_entry();

  struct T_Heartbeat : public Thread {
    OSD *osd;
    T_Heartbeat(OSD *o) : osd(o) {}
    void *entry() {
      osd->heartbeat_entry();
      return 0;
    }
  } heartbeat_thread;

public:
  bool heartbeat_dispatch(Message *m);

  struct HeartbeatDispatcher : public Dispatcher {
  private:
    bool ms_dispatch(Message *m) {
      return osd->heartbeat_dispatch(m);
    };
    bool ms_handle_reset(Connection *con) { return false; }
    void ms_handle_remote_reset(Connection *con) {}
  public:
    OSD *osd;
    HeartbeatDispatcher(OSD *o) 
      : Dispatcher(g_ceph_context), osd(o)
    {
    }
  } heartbeat_dispatcher;


private:
  // -- stats --
  Mutex stat_lock;
  osd_stat_t osd_stat;

  void update_osd_stat();
  
  // -- waiters --
  list<OpRequest*> finished;
  Mutex finished_lock;
  
  void take_waiters(list<class OpRequest*>& ls) {
    finished_lock.Lock();
    finished.splice(finished.end(), ls);
    finished_lock.Unlock();
  }
  void take_waiter(OpRequest *op) {
    finished_lock.Lock();
    finished.push_back(op);
    finished_lock.Unlock();
  }
  void push_waiters(list<OpRequest*>& ls) {
    assert(osd_lock.is_locked());   // currently, at least.  be careful if we change this (see #743)
    finished_lock.Lock();
    finished.splice(finished.begin(), ls);
    finished_lock.Unlock();
  }
  void do_waiters();
  
  // -- op tracking --
  xlist<OpRequest*> ops_in_flight;
  /** This is an inner lock that is taken by the following three
   * functions without regard for what locks the callers hold. It
   * protects the xlist, but not the OpRequests. */
  Mutex ops_in_flight_lock;
  void register_inflight_op(xlist<OpRequest*>::item *i);
  void check_ops_in_flight();
  void unregister_inflight_op(xlist<OpRequest*>::item *i);
  void dump_ops_in_flight(ostream& ss);
  friend struct OpRequest;
  friend class OpsFlightSocketHook;
  OpsFlightSocketHook *admin_ops_hook;

  // -- op queue --
  deque<PG*> op_queue;
  int op_queue_len;

  struct OpWQ : public ThreadPool::WorkQueue<PG> {
    OSD *osd;
    OpWQ(OSD *o, time_t ti, ThreadPool *tp)
      : ThreadPool::WorkQueue<PG>("OSD::OpWQ", ti, ti*10, tp), osd(o) {}

    bool _enqueue(PG *pg);
    void _dequeue(PG *pg) {
      assert(0);
    }
    bool _empty() {
      return osd->op_queue.empty();
    }
    PG *_dequeue();
    void _process(PG *pg) {
      osd->dequeue_op(pg);
    }
    void _clear() {
      assert(osd->op_queue.empty());
    }
  } op_wq;

  void enqueue_op(PG *pg, OpRequest *op);
  void requeue_ops(PG *pg, list<OpRequest*>& ls);
  void dequeue_op(PG *pg);
  static void static_dequeueop(OSD *o, PG *pg) {
    o->dequeue_op(pg);
  };


  friend class PG;
  friend class ReplicatedPG;


 protected:

  // -- osd map --
  OSDMapRef       osdmap;
  utime_t         had_map_since;
  RWLock          map_lock;
  list<OpRequest*>  waiting_for_osdmap;

  Mutex peer_map_epoch_lock;
  map<int, epoch_t> peer_map_epoch;
  
  epoch_t get_peer_epoch(int p);
  epoch_t note_peer_epoch(int p, epoch_t e);
  void forget_peer_epoch(int p, epoch_t e);

  bool _share_map_incoming(const entity_inst_t& inst, epoch_t epoch,
			   Session *session = 0);
  void _share_map_outgoing(const entity_inst_t& inst);

  void wait_for_new_map(OpRequest *op);
  void handle_osd_map(class MOSDMap *m);
  void note_down_osd(int osd);
  void note_up_osd(int osd);
  
  void advance_map(ObjectStore::Transaction& t, C_Contexts *tfin);
  void activate_map(ObjectStore::Transaction& t, list<Context*>& tfin);

  // osd map cache (past osd maps)
  map<epoch_t,OSDMapRef > map_cache;
  map<epoch_t,bufferlist> map_inc_bl;
  map<epoch_t,bufferlist> map_bl;
  Mutex map_cache_lock;

  OSDMapRef get_map(epoch_t e);
  OSDMapRef add_map(OSDMap *o);
  void add_map_bl(epoch_t e, bufferlist& bl);
  void add_map_inc_bl(epoch_t e, bufferlist& bl);
  void trim_map_cache(epoch_t oldest);
  void trim_map_bl_cache(epoch_t oldest);
  void clear_map_cache();

  bool get_map_bl(epoch_t e, bufferlist& bl);
  bool get_inc_map_bl(epoch_t e, bufferlist& bl);
  bool get_inc_map(epoch_t e, OSDMap::Incremental &inc);
  
  MOSDMap *build_incremental_map_msg(epoch_t from, epoch_t to);
  void send_incremental_map(epoch_t since, const entity_inst_t& inst, bool lazy=false);
  void send_map(MOSDMap *m, const entity_inst_t& inst, bool lazy);


protected:
  Watch *watch; /* notify-watch handler */


protected:
  // -- placement groups --
  map<int, PGPool*> pool_map;
  hash_map<pg_t, PG*> pg_map;
  map<pg_t, list<OpRequest*> > waiting_for_pg;
  PGRecoveryStats pg_recovery_stats;

  PGPool *_get_pool(int id);
  void _put_pool(PGPool *p);

  bool  _have_pg(pg_t pgid);
  PG   *_lookup_lock_pg(pg_t pgid);
  PG   *_lookup_lock_pg_with_map_lock_held(pg_t pgid);
  PG   *_open_lock_pg(pg_t pg, bool no_lockdep_check=false, bool hold_map_lock=false);
  PG   *_create_lock_pg(pg_t pgid, bool newly_created, bool hold_map_lock,
			int role, vector<int>& up, vector<int>& acting, pg_history_t history,
			ObjectStore::Transaction& t);

  PG *lookup_lock_raw_pg(pg_t pgid);

  PG *get_or_create_pg(const pg_info_t& info, epoch_t epoch, int from, int& pcreated, bool primary,
		       ObjectStore::Transaction **pt,
		       C_Contexts **pfin);
  
  void load_pgs();
  void calc_priors_during(pg_t pgid, epoch_t start, epoch_t end, set<int>& pset);
  void project_pg_history(pg_t pgid, pg_history_t& h, epoch_t from,
			  vector<int>& lastup, vector<int>& lastacting);

  void wake_pg_waiters(pg_t pgid) {
    if (waiting_for_pg.count(pgid)) {
      take_waiters(waiting_for_pg[pgid]);
      waiting_for_pg.erase(pgid);
    }
  }
  void wake_all_pg_waiters() {
    for (map<pg_t, list<OpRequest*> >::iterator p = waiting_for_pg.begin();
	 p != waiting_for_pg.end();
	 p++)
      take_waiters(p->second);
    waiting_for_pg.clear();
  }


  // -- pg creation --
  struct create_pg_info {
    pg_history_t history;
    vector<int> acting;
    set<int> prior;
    pg_t parent;
    int split_bits;
  };
  hash_map<pg_t, create_pg_info> creating_pgs;

  bool can_create_pg(pg_t pgid);
  void handle_pg_create(OpRequest *op);

  void do_split(PG *parent, set<pg_t>& children, ObjectStore::Transaction &t, C_Contexts *tfin);
  void split_pg(PG *parent, map<pg_t,PG*>& children, ObjectStore::Transaction &t);


  // == monitor interaction ==
  utime_t last_mon_report;
  utime_t last_pg_stats_sent;

  /* if our monitor dies, we want to notice it and reconnect.
   *  So we keep track of when it last acked our stat updates,
   *  and if too much time passes (and we've been sending
   *  more updates) then we can call it dead and reconnect
   *  elsewhere.
   */
  utime_t last_pg_stats_ack;
  bool outstanding_pg_stats; // some stat updates haven't been acked yet

  void do_mon_report();

  // -- boot --
  void start_boot();
  void _got_boot_version(epoch_t oldest, epoch_t newest);
  void send_boot();
  
  friend class C_OSD_GetVersion;

  void clear_temp();

  // -- alive --
  epoch_t up_thru_wanted;
  epoch_t up_thru_pending;

  void queue_want_up_thru(epoch_t want);
  void send_alive();

  // -- pg_temp --
  map<pg_t, vector<int> > pg_temp_wanted;

  void queue_want_pg_temp(pg_t pgid, vector<int>& want);
  void send_pg_temp();

  // -- failures --
  set<int> failure_queue;
  map<int,entity_inst_t> failure_pending;


  void queue_failure(int n) {
    failure_queue.insert(n);
  }
  void send_failures();
  void send_still_alive(entity_inst_t i);

  // -- pg stats --
  Mutex pg_stat_queue_lock;
  Cond pg_stat_queue_cond;
  xlist<PG*> pg_stat_queue;
  bool osd_stat_updated;
  uint64_t pg_stat_tid, pg_stat_tid_flushed;

  void send_pg_stats(const utime_t &now);
  void handle_pg_stats_ack(class MPGStatsAck *ack);
  void flush_pg_stats();

  void pg_stat_queue_enqueue(PG *pg) {
    pg_stat_queue_lock.Lock();
    if (pg->is_primary() && !pg->stat_queue_item.is_on_list()) {
      pg->get();
      pg_stat_queue.push_back(&pg->stat_queue_item);
    }
    osd_stat_updated = true;
    pg_stat_queue_lock.Unlock();
  }
  void pg_stat_queue_dequeue(PG *pg) {
    pg_stat_queue_lock.Lock();
    if (pg->stat_queue_item.remove_myself())
      pg->put();
    pg_stat_queue_lock.Unlock();
  }
  void clear_pg_stat_queue() {
    pg_stat_queue_lock.Lock();
    while (!pg_stat_queue.empty()) {
      PG *pg = pg_stat_queue.front();
      pg_stat_queue.pop_front();
      pg->put();
    }
    pg_stat_queue_lock.Unlock();
  }


  // -- tids --
  // for ops i issue
  tid_t               last_tid;

  Mutex tid_lock;
  tid_t get_tid() {
    tid_t t;
    tid_lock.Lock();
    t = ++last_tid;
    tid_lock.Unlock();
    return t;
  }



  // -- generic pg peering --
  void do_notifies(map< int, vector<pg_info_t> >& notify_list,
		   epoch_t query_epoch);
  void do_queries(map< int, map<pg_t,pg_query_t> >& query_map);
  void do_infos(map<int, MOSDPGInfo*>& info_map);
  void repeer(PG *pg, map< int, map<pg_t,pg_query_t> >& query_map);

  bool require_mon_peer(Message *m);
  bool require_osd_peer(OpRequest *op);

  bool require_same_or_newer_map(OpRequest *op, epoch_t e);

  void handle_pg_query(OpRequest *op);
  void handle_pg_missing(OpRequest *op);
  void handle_pg_notify(OpRequest *op);
  void handle_pg_log(OpRequest *op);
  void handle_pg_info(OpRequest *op);
  void handle_pg_trim(OpRequest *op);

  void handle_pg_scan(OpRequest *op);
  bool scan_is_queueable(PG *pg, OpRequest *op);

  void handle_pg_backfill(OpRequest *op);
  bool backfill_is_queueable(PG *pg, OpRequest *op);

  void handle_pg_remove(OpRequest *op);
  void queue_pg_for_deletion(PG *pg);
  void _remove_pg(PG *pg);

  // -- commands --
  struct Command {
    vector<string> cmd;
    tid_t tid;
    bufferlist indata;
    Connection *con;

    Command(vector<string>& c, tid_t t, bufferlist& bl, Connection *co)
      : cmd(c), tid(t), indata(bl), con(co) {
      if (con)
	con->get();
    }
    ~Command() {
      if (con)
	con->put();
    }
  };
  list<Command*> command_queue;
  struct CommandWQ : public ThreadPool::WorkQueue<Command> {
    OSD *osd;
    CommandWQ(OSD *o, time_t ti, ThreadPool *tp)
      : ThreadPool::WorkQueue<Command>("OSD::CommandWQ", ti, 0, tp), osd(o) {}

    bool _empty() {
      return osd->command_queue.empty();
    }
    bool _enqueue(Command *c) {
      osd->command_queue.push_back(c);
      return true;
    }
    void _dequeue(Command *pg) {
      assert(0);
    }
    Command *_dequeue() {
      if (osd->command_queue.empty())
	return NULL;
      Command *c = osd->command_queue.front();
      osd->command_queue.pop_front();
      return c;
    }
    void _process(Command *c) {
      osd->osd_lock.Lock();
      osd->do_command(c->con, c->tid, c->cmd, c->indata);
      osd->osd_lock.Unlock();
      delete c;
    }
    void _clear() {
      while (!osd->command_queue.empty()) {
	Command *c = osd->command_queue.front();
	osd->command_queue.pop_front();
	delete c;
      }
    }
  } command_wq;

  void handle_command(class MMonCommand *m);
  void handle_command(class MCommand *m);
  void do_command(Connection *con, tid_t tid, vector<string>& cmd, bufferlist& data);

  // -- pg recovery --
  xlist<PG*> recovery_queue;
  utime_t defer_recovery_until;
  int recovery_ops_active;
#ifdef DEBUG_RECOVERY_OIDS
  map<pg_t, set<hobject_t> > recovery_oids;
#endif

  struct RecoveryWQ : public ThreadPool::WorkQueue<PG> {
    OSD *osd;
    RecoveryWQ(OSD *o, time_t ti, ThreadPool *tp)
      : ThreadPool::WorkQueue<PG>("OSD::RecoveryWQ", ti, ti*10, tp), osd(o) {}

    bool _empty() {
      return osd->recovery_queue.empty();
    }
    bool _enqueue(PG *pg) {
      if (!pg->recovery_item.is_on_list()) {
	pg->get();
	osd->recovery_queue.push_back(&pg->recovery_item);

	if (g_conf->osd_recovery_delay_start > 0) {
	  osd->defer_recovery_until = ceph_clock_now(g_ceph_context);
	  osd->defer_recovery_until += g_conf->osd_recovery_delay_start;
	}
	return true;
      }
      return false;
    }
    void _dequeue(PG *pg) {
      if (pg->recovery_item.remove_myself())
	pg->put();
    }
    PG *_dequeue() {
      if (osd->recovery_queue.empty())
	return NULL;
      
      if (!osd->_recover_now())
	return NULL;

      PG *pg = osd->recovery_queue.front();
      osd->recovery_queue.pop_front();
      return pg;
    }
    void _process(PG *pg) {
      osd->do_recovery(pg);
    }
    void _clear() {
      while (!osd->recovery_queue.empty()) {
	PG *pg = osd->recovery_queue.front();
	osd->recovery_queue.pop_front();
	pg->put();
      }
    }
  } recovery_wq;

  bool queue_for_recovery(PG *pg);
  void start_recovery_op(PG *pg, const hobject_t& soid);
  void finish_recovery_op(PG *pg, const hobject_t& soid, bool dequeue);
  void defer_recovery(PG *pg);
  void do_recovery(PG *pg);
  bool _recover_now();

  Mutex remove_list_lock;
  map<epoch_t, map<int, vector<pg_t> > > remove_list;

  void queue_for_removal(epoch_t epoch, int osd, pg_t pgid) {
    remove_list_lock.Lock();
    remove_list[epoch][osd].push_back(pgid);
    remove_list_lock.Unlock();
  }

  // replay / delayed pg activation
  Mutex replay_queue_lock;
  list< pair<pg_t, utime_t > > replay_queue;
  
  void check_replay_queue();


  // -- snap trimming --
  xlist<PG*> snap_trim_queue;
  
  struct SnapTrimWQ : public ThreadPool::WorkQueue<PG> {
    OSD *osd;
    SnapTrimWQ(OSD *o, time_t ti, ThreadPool *tp)
      : ThreadPool::WorkQueue<PG>("OSD::SnapTrimWQ", ti, 0, tp), osd(o) {}

    bool _empty() {
      return osd->snap_trim_queue.empty();
    }
    bool _enqueue(PG *pg) {
      if (pg->snap_trim_item.is_on_list())
	return false;
      pg->get();
      osd->snap_trim_queue.push_back(&pg->snap_trim_item);
      return true;
    }
    void _dequeue(PG *pg) {
      if (pg->snap_trim_item.remove_myself())
	pg->put();
    }
    PG *_dequeue() {
      if (osd->snap_trim_queue.empty())
	return NULL;
      PG *pg = osd->snap_trim_queue.front();
      osd->snap_trim_queue.pop_front();
      return pg;
    }
    void _process(PG *pg) {
      pg->snap_trimmer();
    }
    void _clear() {
      osd->snap_trim_queue.clear();
    }
  } snap_trim_wq;

  // -- scrub scheduling --
  Mutex sched_scrub_lock;
  int scrubs_pending;
  int scrubs_active;
  set< pair<utime_t,pg_t> > last_scrub_pg;

  bool scrub_should_schedule();
  void sched_scrub();

  void reg_last_pg_scrub(pg_t pgid, utime_t t) {
    Mutex::Locker l(sched_scrub_lock);
    last_scrub_pg.insert(pair<utime_t,pg_t>(t, pgid));
  }
  void unreg_last_pg_scrub(pg_t pgid, utime_t t) {
    Mutex::Locker l(sched_scrub_lock);
    pair<utime_t,pg_t> p(t, pgid);
    assert(last_scrub_pg.count(p));
    last_scrub_pg.erase(p);
  }

  bool inc_scrubs_pending();
  void dec_scrubs_pending();
  void dec_scrubs_active();

  // -- scrubbing --
  xlist<PG*> scrub_queue;


  struct ScrubWQ : public ThreadPool::WorkQueue<PG> {
    OSD *osd;
    ScrubWQ(OSD *o, time_t ti, ThreadPool *tp)
      : ThreadPool::WorkQueue<PG>("OSD::ScrubWQ", ti, 0, tp), osd(o) {}

    bool _empty() {
      return osd->scrub_queue.empty();
    }
    bool _enqueue(PG *pg) {
      if (pg->scrub_item.is_on_list()) {
	return false;
      }
      pg->get();
      osd->scrub_queue.push_back(&pg->scrub_item);
      return true;
    }
    void _dequeue(PG *pg) {
      if (pg->scrub_item.remove_myself()) {
	pg->put();
      }
    }
    PG *_dequeue() {
      if (osd->scrub_queue.empty())
	return NULL;
      PG *pg = osd->scrub_queue.front();
      osd->scrub_queue.pop_front();
      return pg;
    }
    void _process(PG *pg) {
      pg->scrub();
    }
    void _clear() {
      while (!osd->scrub_queue.empty()) {
	PG *pg = osd->scrub_queue.front();
	osd->scrub_queue.pop_front();
	pg->put();
      }
    }
  } scrub_wq;

  struct ScrubFinalizeWQ : public ThreadPool::WorkQueue<PG> {
  private:
    OSD *osd;
    xlist<PG*> scrub_finalize_queue;

  public:
    ScrubFinalizeWQ(OSD *o, time_t ti, ThreadPool *tp)
      : ThreadPool::WorkQueue<PG>("OSD::ScrubFinalizeWQ", ti, ti*10, tp), osd(o) {}

    bool _empty() {
      return scrub_finalize_queue.empty();
    }
    bool _enqueue(PG *pg) {
      if (pg->scrub_finalize_item.is_on_list()) {
	return false;
      }
      pg->get();
      scrub_finalize_queue.push_back(&pg->scrub_finalize_item);
      return true;
    }
    void _dequeue(PG *pg) {
      if (pg->scrub_finalize_item.remove_myself()) {
	pg->put();
      }
    }
    PG *_dequeue() {
      if (scrub_finalize_queue.empty())
	return NULL;
      PG *pg = scrub_finalize_queue.front();
      scrub_finalize_queue.pop_front();
      return pg;
    }
    void _process(PG *pg) {
      pg->scrub_finalize();
      pg->put();
    }
    void _clear() {
      while (!scrub_finalize_queue.empty()) {
	PG *pg = scrub_finalize_queue.front();
	scrub_finalize_queue.pop_front();
	pg->put();
      }
    }
  } scrub_finalize_wq;

  struct RepScrubWQ : public ThreadPool::WorkQueue<MOSDRepScrub> {
  private: 
    OSD *osd;
    list<MOSDRepScrub*> rep_scrub_queue;

  public:
    RepScrubWQ(OSD *o, time_t ti, ThreadPool *tp)
      : ThreadPool::WorkQueue<MOSDRepScrub>("OSD::RepScrubWQ", ti, 0, tp), osd(o) {}

    bool _empty() {
      return rep_scrub_queue.empty();
    }
    bool _enqueue(MOSDRepScrub *msg) {
      rep_scrub_queue.push_back(msg);
      return true;
    }
    void _dequeue(MOSDRepScrub *msg) {
      assert(0); // Not applicable for this wq
      return;
    }
    MOSDRepScrub *_dequeue() {
      if (rep_scrub_queue.empty())
	return NULL;
      MOSDRepScrub *msg = rep_scrub_queue.front();
      rep_scrub_queue.pop_front();
      return msg;
    }
    void _process(MOSDRepScrub *msg) {
      osd->osd_lock.Lock();
      if (osd->_have_pg(msg->pgid)) {
	PG *pg = osd->_lookup_lock_pg(msg->pgid);
	osd->osd_lock.Unlock();
	pg->replica_scrub(msg);
	pg->unlock();
      } else {
	msg->put();
	osd->osd_lock.Unlock();
      }
    }
    void _clear() {
      while (!rep_scrub_queue.empty()) {
	MOSDRepScrub *msg = rep_scrub_queue.front();
	rep_scrub_queue.pop_front();
	msg->put();
      }
    }
  } rep_scrub_wq;

  // -- removing --
  xlist<PG*> remove_queue;

  struct RemoveWQ : public ThreadPool::WorkQueue<PG> {
    OSD *osd;
    RemoveWQ(OSD *o, time_t ti, ThreadPool *tp)
      : ThreadPool::WorkQueue<PG>("OSD::RemoveWQ", ti, 0, tp), osd(o) {}

    bool _empty() {
      return osd->remove_queue.empty();
    }
    bool _enqueue(PG *pg) {
      if (pg->remove_item.is_on_list())
	return false;
      pg->get();
      osd->remove_queue.push_back(&pg->remove_item);
      return true;
    }
    void _dequeue(PG *pg) {
      if (pg->remove_item.remove_myself())
	pg->put();
    }
    PG *_dequeue() {
      if (osd->remove_queue.empty())
	return NULL;
      PG *pg = osd->remove_queue.front();
      osd->remove_queue.pop_front();
      return pg;
    }
    void _process(PG *pg) {
      osd->_remove_pg(pg);
    }
    void _clear() {
      while (!osd->remove_queue.empty()) {
	PG *pg = osd->remove_queue.front();
	osd->remove_queue.pop_front();
	pg->put();
      }
    }
  } remove_wq;

 private:
  bool ms_dispatch(Message *m);
  bool ms_get_authorizer(int dest_type, AuthAuthorizer **authorizer, bool force_new);
  bool ms_verify_authorizer(Connection *con, int peer_type,
			    int protocol, bufferlist& authorizer, bufferlist& authorizer_reply,
			    bool& isvalid);
  void ms_handle_connect(Connection *con);
  bool ms_handle_reset(Connection *con);
  void ms_handle_remote_reset(Connection *con) {}

 public:
  /* internal and external can point to the same messenger, they will still
   * be cleaned up properly*/
  OSD(int id, Messenger *internal, Messenger *external, Messenger *hbmin, Messenger *hbmout,
      MonClient *mc, const std::string &dev, const std::string &jdev);
  ~OSD();

  // static bits
  static int find_osd_dev(char *result, int whoami);
  static ObjectStore *create_object_store(const std::string &dev, const std::string &jdev);
  static int convertfs(const std::string &dev, const std::string &jdev);
  static int mkfs(const std::string &dev, const std::string &jdev,
		  uuid_d fsid, int whoami);
  static int mkjournal(const std::string &dev, const std::string &jdev);
  static int flushjournal(const std::string &dev, const std::string &jdev);
  static int dump_journal(const std::string &dev, const std::string &jdev, ostream& out);
  /* remove any non-user xattrs from a map of them */
  void filter_xattrs(map<string, bufferptr>& attrs) {
    for (map<string, bufferptr>::iterator iter = attrs.begin();
	 iter != attrs.end();
	 ) {
      if (('_' != iter->first.at(0)) || (iter->first.size() == 1))
	attrs.erase(iter++);
      else ++iter;
    }
  }

private:
  static int write_meta(const std::string &base, const std::string &file,
			const char *val, size_t vallen);
  static int read_meta(const std::string &base, const std::string &file,
		       char *val, size_t vallen);
  static int write_meta(const std::string &base,
			uuid_d& cluster_fsid, uuid_d& osd_fsid, int whoami);
public:
  static int peek_meta(const std::string &dev, string& magic,
		       uuid_d& cluster_fsid, uuid_d& osd_fsid, int& whoami);
  static int peek_journal_fsid(std::string jpath, uuid_d& fsid);
  

  // startup/shutdown
  int pre_init();
  int init();

  void suicide(int exitcode);
  int shutdown();

  void handle_signal(int signum);

  void reply_op_error(OpRequest *op, int r);
  void reply_op_error(OpRequest *op, int r, eversion_t v);
  void handle_misdirected_op(PG *pg, OpRequest *op);

  void handle_rep_scrub(MOSDRepScrub *m);
  void handle_scrub(class MOSDScrub *m);
  void handle_osd_ping(class MOSDPing *m);
  void handle_op(OpRequest *op);
  void handle_sub_op(OpRequest *op);
  void handle_sub_op_reply(OpRequest *op);

private:
  /// check if we can throw out op from a disconnected client
  bool op_is_discardable(class MOSDOp *m);
  /// check if op has sufficient caps
  bool op_has_sufficient_caps(PG *pg, class MOSDOp *m);
  /// check if op should be (re)queued for processing
  bool op_is_queueable(PG *pg, OpRequest *op);
  /// check if subop should be (re)queued for processing
  bool subop_is_queueable(PG *pg, OpRequest *op);

public:
  void force_remount();

  int init_op_flags(MOSDOp *op);


  void put_object_context(void *_obc, pg_t pgid);
  void complete_notify(void *notif, void *obc);
  void ack_notification(entity_name_t& peer_addr, void *notif, void *obc,
			ReplicatedPG *pg);
  Mutex watch_lock;
  SafeTimer watch_timer;
  void handle_notify_timeout(void *notif);
  void disconnect_session_watches(Session *session);
  void handle_watch_timeout(void *obc,
			    ReplicatedPG *pg,
			    entity_name_t entity,
			    utime_t expire);
};

//compatibility of the executable
extern const CompatSet::Feature ceph_osd_feature_compat[];
extern const CompatSet::Feature ceph_osd_feature_ro_compat[];
extern const CompatSet::Feature ceph_osd_feature_incompat[];

#endif
