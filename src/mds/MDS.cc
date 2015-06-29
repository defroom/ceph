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

#include <unistd.h>

#include "global/signal_handler.h"

#include "include/types.h"
#include "include/str_list.h"
#include "common/entity_name.h"
#include "common/Clock.h"
#include "common/signal.h"
#include "common/ceph_argparse.h"
#include "common/errno.h"

#include "msg/Messenger.h"
#include "mon/MonClient.h"

#include "osdc/Objecter.h"

#include "MDSMap.h"

#include "MDS.h"
#include "Server.h"
#include "Locker.h"
#include "MDCache.h"
#include "MDLog.h"
#include "MDBalancer.h"
#include "Migrator.h"

#include "SnapServer.h"
#include "SnapClient.h"

#include "InoTable.h"

#include "common/HeartbeatMap.h"

#include "common/perf_counters.h"

#include "common/Timer.h"

#include "events/ESession.h"
#include "events/ESubtreeMap.h"

#include "messages/MMDSMap.h"
#include "messages/MMDSBeacon.h"

#include "messages/MGenericMessage.h"

#include "messages/MMonCommand.h"
#include "messages/MCommand.h"
#include "messages/MCommandReply.h"

#include "auth/AuthAuthorizeHandler.h"
#include "auth/KeyRing.h"

#include "common/config.h"

#include "perfglue/cpu_profiler.h"
#include "perfglue/heap_profiler.h"


#define dout_subsys ceph_subsys_mds
#undef dout_prefix
#define dout_prefix *_dout << "mds." << whoami << '.' << incarnation << ' '


// cons/des
MDS::MDS(const std::string &n, Messenger *m, MonClient *mc) : 
  MDSRank(mds_lock, clog, timer, beacon, mdsmap, &finisher, this, m, mc),
  Dispatcher(m->cct),
  mds_lock("MDS::mds_lock"),
  stopping(false),
  timer(m->cct, mds_lock),
  beacon(m->cct, mc, n),
  authorize_handler_cluster_registry(new AuthAuthorizeHandlerRegistry(m->cct,
								      m->cct->_conf->auth_supported.empty() ?
								      m->cct->_conf->auth_cluster_required :
								      m->cct->_conf->auth_supported)),
  authorize_handler_service_registry(new AuthAuthorizeHandlerRegistry(m->cct,
								      m->cct->_conf->auth_supported.empty() ?
								      m->cct->_conf->auth_service_required :
								      m->cct->_conf->auth_supported)),
  name(n),
  messenger(m),
  monc(mc),
  log_client(m->cct, messenger, &mc->monmap, LogClient::NO_FLAGS),
  finisher(cct),
  asok_hook(NULL)
{
  orig_argc = 0;
  orig_argv = NULL;

  last_tid = 0;

  clog = log_client.create_channel();

  monc->set_messenger(messenger);

  mdsmap = new MDSMap;

  objecter = new Objecter(m->cct, messenger, monc, NULL, 0, 0);
  objecter->unset_honor_osdmap_full();

  mdcache = new MDCache(this);
  mdlog = new MDLog(this);
  balancer = new MDBalancer(this, messenger);

  inotable = new InoTable(this);
  snapserver = new SnapServer(this);
  snapclient = new SnapClient(this);

  server = new Server(this);
  locker = new Locker(this, mdcache);

  dispatch_depth = 0;

  // clients
  last_client_mdsmap_bcast = 0;
  
  // tick
  tick_event = 0;

  last_state = want_state = state = MDSMap::STATE_BOOT;

  logger = 0;
  mlogger = 0;
  op_tracker.set_complaint_and_threshold(m->cct->_conf->mds_op_complaint_time,
                                         m->cct->_conf->mds_op_log_threshold);
  op_tracker.set_history_size_and_duration(m->cct->_conf->mds_op_history_size,
                                           m->cct->_conf->mds_op_history_duration);
}

MDS::~MDS() {
  Mutex::Locker lock(mds_lock);

  delete authorize_handler_service_registry;
  delete authorize_handler_cluster_registry;

  if (mdcache) { delete mdcache; mdcache = NULL; }
  if (mdlog) { delete mdlog; mdlog = NULL; }
  if (balancer) { delete balancer; balancer = NULL; }
  if (inotable) { delete inotable; inotable = NULL; }
  if (snapserver) { delete snapserver; snapserver = NULL; }
  if (snapclient) { delete snapclient; snapclient = NULL; }
  if (mdsmap) { delete mdsmap; mdsmap = 0; }

  if (server) { delete server; server = 0; }
  if (locker) { delete locker; locker = 0; }

  if (objecter) { delete objecter; objecter = 0; }

  if (logger) {
    g_ceph_context->get_perfcounters_collection()->remove(logger);
    delete logger;
    logger = 0;
  }
  if (mlogger) {
    g_ceph_context->get_perfcounters_collection()->remove(mlogger);
    delete mlogger;
    mlogger = 0;
  }
  
  if (messenger)
    delete messenger;
}

class MDSSocketHook : public AdminSocketHook {
  MDS *mds;
public:
  MDSSocketHook(MDS *m) : mds(m) {}
  bool call(std::string command, cmdmap_t& cmdmap, std::string format,
	    bufferlist& out) {
    stringstream ss;
    bool r = mds->asok_command(command, cmdmap, format, ss);
    out.append(ss);
    return r;
  }
};

bool MDS::asok_command(string command, cmdmap_t& cmdmap, string format,
		    ostream& ss)
{
  dout(1) << "asok_command: " << command << " (starting...)" << dendl;

  Formatter *f = Formatter::create(format, "json-pretty", "json-pretty");
  if (command == "status") {

    const OSDMap *osdmap = objecter->get_osdmap_read();
    const epoch_t osd_epoch = osdmap->get_epoch();
    objecter->put_osdmap_read();

    f->open_object_section("status");
    f->dump_stream("cluster_fsid") << monc->get_fsid();
    f->dump_unsigned("whoami", whoami);
    f->dump_string("state", ceph_mds_state_name(get_state()));
    f->dump_unsigned("mdsmap_epoch", mdsmap->get_epoch());
    f->dump_unsigned("osdmap_epoch", osd_epoch);
    f->dump_unsigned("osdmap_epoch_barrier", get_osd_epoch_barrier());
    f->close_section(); // status
  } else {
    if (whoami < 0) {
      dout(1) << "Can't run that command on an inactive MDS!" << dendl;
      f->dump_string("error", "mds_not_active");
    } else if (command == "dump_ops_in_flight" ||
	       command == "ops") {
      if (!op_tracker.tracking_enabled) {
        ss << "op_tracker tracking is not enabled";
      } else {
        op_tracker.dump_ops_in_flight(f);
      }
    } else if (command == "dump_historic_ops") {
      if (!op_tracker.tracking_enabled) {
	ss << "op_tracker tracking is not enabled";
      } else {
        op_tracker.dump_historic_ops(f);
      }
    } else if (command == "osdmap barrier") {
      int64_t target_epoch = 0;
      bool got_val = cmd_getval(g_ceph_context, cmdmap, "target_epoch", target_epoch);
      
      if (!got_val) {
	ss << "no target epoch given";
	delete f;
	return true;
      }
      
      mds_lock.Lock();
      set_osd_epoch_barrier(target_epoch);
      mds_lock.Unlock();
      
      C_SaferCond cond;
      bool already_got = objecter->wait_for_map(target_epoch, &cond);
      if (!already_got) {
	dout(4) << __func__ << ": waiting for OSD epoch " << target_epoch << dendl;
	cond.wait();
      }
    } else if (command == "session ls") {
      mds_lock.Lock();
      
      heartbeat_reset();
      
      // Dump sessions, decorated with recovery/replay status
      f->open_array_section("sessions");
      const ceph::unordered_map<entity_name_t, Session*> session_map = sessionmap.get_sessions();
      for (ceph::unordered_map<entity_name_t,Session*>::const_iterator p = session_map.begin();
	   p != session_map.end();
	   ++p)  {
	if (!p->first.is_client()) {
	  continue;
	}
	
	Session *s = p->second;
	
	f->open_object_section("session");
	f->dump_int("id", p->first.num());
	
	f->dump_int("num_leases", s->leases.size());
	f->dump_int("num_caps", s->caps.size());
	
	f->dump_string("state", s->get_state_name());
	f->dump_int("replay_requests", is_clientreplay() ? s->get_request_count() : 0);
	f->dump_unsigned("completed_requests", s->get_num_completed_requests());
	f->dump_bool("reconnecting", server->waiting_for_reconnect(p->first.num()));
	f->dump_stream("inst") << s->info.inst;
	f->open_object_section("client_metadata");
	for (map<string, string>::const_iterator i = s->info.client_metadata.begin();
	     i != s->info.client_metadata.end(); ++i) {
	  f->dump_string(i->first.c_str(), i->second);
	}
	f->close_section(); // client_metadata
	f->close_section(); //session
      }
      f->close_section(); //sessions
      
      mds_lock.Unlock();
    } else if (command == "session evict") {
      std::string client_id;
      const bool got_arg = cmd_getval(g_ceph_context, cmdmap, "client_id", client_id);
      assert(got_arg == true);
      
      mds_lock.Lock();
      Session *session = sessionmap.get_session(entity_name_t(CEPH_ENTITY_TYPE_CLIENT,
							      strtol(client_id.c_str(), 0, 10)));
      if (session) {
	C_SaferCond on_safe;
	server->kill_session(session, &on_safe);
	
	mds_lock.Unlock();
	on_safe.wait();
      } else {
	dout(15) << "session " << session << " not in sessionmap!" << dendl;
	mds_lock.Unlock();
      }
    } else if (command == "scrub_path") {
      string path;
      cmd_getval(g_ceph_context, cmdmap, "path", path);
      command_scrub_path(f, path);
    } else if (command == "flush_path") {
      string path;
      cmd_getval(g_ceph_context, cmdmap, "path", path);
      command_flush_path(f, path);
    } else if (command == "flush journal") {
      command_flush_journal(f);
    } else if (command == "get subtrees") {
      command_get_subtrees(f);
    } else if (command == "export dir") {
      string path;
      if(!cmd_getval(g_ceph_context, cmdmap, "path", path)) {
	ss << "malformed path";
        delete f;
        return true;
      }
      int64_t rank;
      if(!cmd_getval(g_ceph_context, cmdmap, "rank", rank)) {
	ss << "malformed rank";
        delete f;
        return true;
      }
      command_export_dir(f, path, (mds_rank_t)rank);
    } else if (command == "dump cache") {
      string path;
      if(!cmd_getval(g_ceph_context, cmdmap, "path", path)) {
        mdcache->dump_cache(f);
      } else {
        mdcache->dump_cache(path);
      }
    } else if (command == "force_readonly") {
      mds_lock.Lock();
      mdcache->force_readonly();
      mds_lock.Unlock();
    } else if (command == "dirfrag split") {
      Mutex::Locker l(mds_lock);
      command_dirfrag_split(cmdmap, ss);
    } else if (command == "dirfrag merge") {
      Mutex::Locker l(mds_lock);
      command_dirfrag_merge(cmdmap, ss);
    } else if (command == "dirfrag ls") {
      Mutex::Locker l(mds_lock);
      command_dirfrag_ls(cmdmap, ss, f);
    }
  }
  f->flush(ss);
  delete f;
  
  dout(1) << "asok_command: " << command << " (complete)" << dendl;
  
  return true;
}

void MDS::command_scrub_path(Formatter *f, const string& path)
{
  C_SaferCond scond;
  {
    Mutex::Locker l(mds_lock);
    mdcache->scrub_dentry(path, f, &scond);
  }
  scond.wait();
  // scrub_dentry() finishers will dump the data for us; we're done!
}

void MDS::command_flush_path(Formatter *f, const string& path)
{
  C_SaferCond scond;
  {
    Mutex::Locker l(mds_lock);
    mdcache->flush_dentry(path, &scond);
  }
  int r = scond.wait();
  f->open_object_section("results");
  f->dump_int("return_code", r);
  f->close_section(); // results
}

/**
 * Wrapper around _command_flush_journal that
 * handles serialization of result
 */
void MDS::command_flush_journal(Formatter *f)
{
  assert(f != NULL);

  std::stringstream ss;
  const int r = _command_flush_journal(&ss);
  f->open_object_section("result");
  f->dump_string("message", ss.str());
  f->dump_int("return_code", r);
  f->close_section();
}

/**
 * Implementation of "flush journal" asok command.
 *
 * @param ss
 * Optionally populate with a human readable string describing the
 * reason for any unexpected return status.
 */
int MDS::_command_flush_journal(std::stringstream *ss)
{
  assert(ss != NULL);

  Mutex::Locker l(mds_lock);

  if (mdcache->is_readonly()) {
    dout(5) << __func__ << ": read-only FS" << dendl;
    return -EROFS;
  }

  if (!is_active()) {
    dout(5) << __func__ << ": MDS not active, no-op" << dendl;
    return 0;
  }

  // I need to seal off the current segment, and then mark all previous segments
  // for expiry
  mdlog->start_new_segment();
  int r = 0;

  // Flush initially so that all the segments older than our new one
  // will be elegible for expiry
  {
    C_SaferCond mdlog_flushed;
    mdlog->flush();
    mdlog->wait_for_safe(new MDSInternalContextWrapper(this, &mdlog_flushed));
    mds_lock.Unlock();
    r = mdlog_flushed.wait();
    mds_lock.Lock();
    if (r != 0) {
      *ss << "Error " << r << " (" << cpp_strerror(r) << ") while flushing journal";
      return r;
    }
  }

  // Because we may not be the last wait_for_safe context on MDLog, and
  // subsequent contexts might wake up in the middle of our later trim_all
  // and interfere with expiry (by e.g. marking dirs/dentries dirty
  // on previous log segments), we run a second wait_for_safe here.
  // See #10368
  {
    C_SaferCond mdlog_cleared;
    mdlog->wait_for_safe(new MDSInternalContextWrapper(this, &mdlog_cleared));
    mds_lock.Unlock();
    r = mdlog_cleared.wait();
    mds_lock.Lock();
    if (r != 0) {
      *ss << "Error " << r << " (" << cpp_strerror(r) << ") while flushing journal";
      return r;
    }
  }

  // Put all the old log segments into expiring or expired state
  dout(5) << __func__ << ": beginning segment expiry" << dendl;
  r = mdlog->trim_all();
  if (r != 0) {
    *ss << "Error " << r << " (" << cpp_strerror(r) << ") while trimming log";
    return r;
  }

  // Attach contexts to wait for all expiring segments to expire
  MDSGatherBuilder expiry_gather(g_ceph_context);

  const std::set<LogSegment*> &expiring_segments = mdlog->get_expiring_segments();
  for (std::set<LogSegment*>::const_iterator i = expiring_segments.begin();
       i != expiring_segments.end(); ++i) {
    (*i)->wait_for_expiry(expiry_gather.new_sub());
  }
  dout(5) << __func__ << ": waiting for " << expiry_gather.num_subs_created()
          << " segments to expire" << dendl;

  if (expiry_gather.has_subs()) {
    C_SaferCond cond;
    expiry_gather.set_finisher(new MDSInternalContextWrapper(this, &cond));
    expiry_gather.activate();

    // Drop mds_lock to allow progress until expiry is complete
    mds_lock.Unlock();
    int r = cond.wait();
    mds_lock.Lock();

    assert(r == 0);  // MDLog is not allowed to raise errors via wait_for_expiry
  }

  dout(5) << __func__ << ": expiry complete, expire_pos/trim_pos is now " << std::hex <<
    mdlog->get_journaler()->get_expire_pos() << "/" <<
    mdlog->get_journaler()->get_trimmed_pos() << dendl;

  // Now everyone I'm interested in is expired
  mdlog->trim_expired_segments();

  dout(5) << __func__ << ": trim complete, expire_pos/trim_pos is now " << std::hex <<
    mdlog->get_journaler()->get_expire_pos() << "/" <<
    mdlog->get_journaler()->get_trimmed_pos() << dendl;

  // Flush the journal header so that readers will start from after the flushed region
  C_SaferCond wrote_head;
  mdlog->get_journaler()->write_head(&wrote_head);
  mds_lock.Unlock();  // Drop lock to allow messenger dispatch progress
  r = wrote_head.wait();
  mds_lock.Lock();
  if (r != 0) {
      *ss << "Error " << r << " (" << cpp_strerror(r) << ") while writing header";
      return r;
  }

  dout(5) << __func__ << ": write_head complete, all done!" << dendl;

  return 0;
}


void MDS::command_get_subtrees(Formatter *f)
{
  assert(f != NULL);

  std::list<CDir*> subtrees;
  mdcache->list_subtrees(subtrees);

  f->open_array_section("subtrees");
  for (std::list<CDir*>::iterator i = subtrees.begin(); i != subtrees.end(); ++i) {
    const CDir *dir = *i;

    f->open_object_section("subtree");
    {
      f->dump_bool("is_auth", dir->is_auth());
      f->dump_int("auth_first", dir->get_dir_auth().first);
      f->dump_int("auth_second", dir->get_dir_auth().second);
      f->open_object_section("dir");
      dir->dump(f);
      f->close_section();
    }
    f->close_section();
  }
  f->close_section();
}


void MDS::command_export_dir(Formatter *f,
    const std::string &path,
    mds_rank_t target)
{
  int r = _command_export_dir(path, target);
  f->open_object_section("results");
  f->dump_int("return_code", r);
  f->close_section(); // results
}

int MDS::_command_export_dir(
    const std::string &path,
    mds_rank_t target)
{
  filepath fp(path.c_str());

  if (target == whoami || !mdsmap->is_up(target) || !mdsmap->is_in(target)) {
    derr << "bad MDS target " << target << dendl;
    return -ENOENT;
  }

  CInode *in = mdcache->cache_traverse(fp);
  if (!in) {
    derr << "Bath path '" << path << "'" << dendl;
    return -ENOENT;
  }
  CDir *dir = in->get_dirfrag(frag_t());
  if (!dir || !(dir->is_auth())) {
    derr << "bad export_dir path dirfrag frag_t() or dir not auth" << dendl;
    return -EINVAL;
  }

  mdcache->migrator->export_dir(dir, target);
  return 0;
}

CDir *MDS::_command_dirfrag_get(
    const cmdmap_t &cmdmap,
    std::ostream &ss)
{
  std::string path;
  bool got = cmd_getval(g_ceph_context, cmdmap, "path", path);
  if (!got) {
    ss << "missing path argument";
    return NULL;
  }

  std::string frag_str;
  if (!cmd_getval(g_ceph_context, cmdmap, "frag", frag_str)) {
    ss << "missing frag argument";
    return NULL;
  }

  CInode *in = mdcache->cache_traverse(filepath(path.c_str()));
  if (!in) {
    // TODO really we should load something in if it's not in cache,
    // but the infrastructure is harder, and we might still be unable
    // to act on it if someone else is auth.
    ss << "directory '" << path << "' inode not in cache";
    return NULL;
  }

  frag_t fg;

  if (!fg.parse(frag_str.c_str())) {
    ss << "frag " << frag_str << " failed to parse";
    return NULL;
  }

  CDir *dir = in->get_dirfrag(fg);
  if (!dir) {
    ss << "frag 0x" << std::hex << in->ino() << "/" << fg << " not in cache ("
          "use `dirfrag ls` to see if it should exist)";
    return NULL;
  }

  if (!dir->is_auth()) {
    ss << "frag " << dir->dirfrag() << " not auth (auth = "
       << dir->authority() << ")";
    return NULL;
  }

  return dir;
}

bool MDS::command_dirfrag_split(
    cmdmap_t cmdmap,
    std::ostream &ss)
{
  int64_t by = 0;
  if (!cmd_getval(g_ceph_context, cmdmap, "bits", by)) {
    ss << "missing bits argument";
    return false;
  }

  if (by <= 0) {
    ss << "must split by >0 bits";
    return false;
  }

  CDir *dir = _command_dirfrag_get(cmdmap, ss);
  if (!dir) {
    return false;
  }

  mdcache->split_dir(dir, by);

  return true;
}

bool MDS::command_dirfrag_merge(
    cmdmap_t cmdmap,
    std::ostream &ss)
{
  std::string path;
  bool got = cmd_getval(g_ceph_context, cmdmap, "path", path);
  if (!got) {
    ss << "missing path argument";
    return false;
  }

  std::string frag_str;
  if (!cmd_getval(g_ceph_context, cmdmap, "frag", frag_str)) {
    ss << "missing frag argument";
    return false;
  }

  CInode *in = mdcache->cache_traverse(filepath(path.c_str()));
  if (!in) {
    ss << "directory '" << path << "' inode not in cache";
    return false;
  }

  frag_t fg;
  if (!fg.parse(frag_str.c_str())) {
    ss << "frag " << frag_str << " failed to parse";
    return false;
  }

  mdcache->merge_dir(in, fg);

  return true;
}

bool MDS::command_dirfrag_ls(
    cmdmap_t cmdmap,
    std::ostream &ss,
    Formatter *f)
{
  std::string path;
  bool got = cmd_getval(g_ceph_context, cmdmap, "path", path);
  if (!got) {
    ss << "missing path argument";
    return false;
  }

  CInode *in = mdcache->cache_traverse(filepath(path.c_str()));
  if (!in) {
    ss << "directory inode not in cache";
    return false;
  }

  f->open_array_section("frags");
  std::list<frag_t> frags;
  // NB using get_leaves_under instead of get_dirfrags to give
  // you the list of what dirfrags may exist, not which are in cache
  in->dirfragtree.get_leaves_under(frag_t(), frags);
  for (std::list<frag_t>::iterator i = frags.begin();
       i != frags.end(); ++i) {
    f->open_object_section("frag");
    f->dump_int("value", i->value());
    f->dump_int("bits", i->bits());
    std::ostringstream frag_str;
    frag_str << std::hex << i->value() << "/" << std::dec << i->bits();
    f->dump_string("str", frag_str.str());
    f->close_section();
  }
  f->close_section();

  return true;
}

void MDS::set_up_admin_socket()
{
  int r;
  AdminSocket *admin_socket = g_ceph_context->get_admin_socket();
  asok_hook = new MDSSocketHook(this);
  r = admin_socket->register_command("status", "status", asok_hook,
				     "high-level status of MDS");
  assert(r == 0);
  r = admin_socket->register_command("dump_ops_in_flight",
				     "dump_ops_in_flight", asok_hook,
				     "show the ops currently in flight");
  assert(r == 0);
  r = admin_socket->register_command("ops",
				     "ops", asok_hook,
				     "show the ops currently in flight");
  assert(r == 0);
  r = admin_socket->register_command("dump_historic_ops", "dump_historic_ops",
				     asok_hook,
				     "show slowest recent ops");
  assert(r == 0);
  r = admin_socket->register_command("scrub_path",
                                     "scrub_path name=path,type=CephString",
                                     asok_hook,
                                     "scrub an inode and output results");
  assert(r == 0);
  r = admin_socket->register_command("flush_path",
                                     "flush_path name=path,type=CephString",
                                     asok_hook,
                                     "flush an inode (and its dirfrags)");
  assert(r == 0);
  r = admin_socket->register_command("export dir",
                                     "export dir "
                                     "name=path,type=CephString "
                                     "name=rank,type=CephInt",
                                     asok_hook,
                                     "migrate a subtree to named MDS");
  assert(r == 0);
  r = admin_socket->register_command("dump cache",
                                     "dump cache name=path,type=CephString,req=false",
                                     asok_hook,
                                     "dump metadata cache (optionally to a file)");
  assert(r == 0);
  r = admin_socket->register_command("session evict",
				     "session evict name=client_id,type=CephString",
				     asok_hook,
				     "Evict a CephFS client");
  assert(r == 0);
  r = admin_socket->register_command("osdmap barrier",
				     "osdmap barrier name=target_epoch,type=CephInt",
				     asok_hook,
				     "Wait until the MDS has this OSD map epoch");
  assert(r == 0);
  r = admin_socket->register_command("session ls",
				     "session ls",
				     asok_hook,
				     "Enumerate connected CephFS clients");
  assert(r == 0);
  r = admin_socket->register_command("flush journal",
				     "flush journal",
				     asok_hook,
				     "Flush the journal to the backing store");
  assert(r == 0);
  r = admin_socket->register_command("force_readonly",
				     "force_readonly",
				     asok_hook,
				     "Force MDS to read-only mode");
  assert(r == 0);
  r = admin_socket->register_command("get subtrees",
				     "get subtrees",
				     asok_hook,
				     "Return the subtree map");
  assert(r == 0);
  r = admin_socket->register_command("dirfrag split",
				     "dirfrag split "
                                     "name=path,type=CephString,req=true "
                                     "name=frag,type=CephString,req=true "
                                     "name=bits,type=CephInt,req=true ",
				     asok_hook,
				     "Fragment directory by path");
  assert(r == 0);
  r = admin_socket->register_command("dirfrag merge",
				     "dirfrag merge "
                                     "name=path,type=CephString,req=true "
                                     "name=frag,type=CephString,req=true",
				     asok_hook,
				     "De-fragment directory by path");
  assert(r == 0);
  r = admin_socket->register_command("dirfrag ls",
				     "dirfrag ls "
                                     "name=path,type=CephString,req=true",
				     asok_hook,
				     "List fragments in directory");
  assert(r == 0);
}

void MDS::clean_up_admin_socket()
{
  AdminSocket *admin_socket = g_ceph_context->get_admin_socket();
  admin_socket->unregister_command("status");
  admin_socket->unregister_command("dump_ops_in_flight");
  admin_socket->unregister_command("ops");
  admin_socket->unregister_command("dump_historic_ops");
  admin_socket->unregister_command("scrub_path");
  admin_socket->unregister_command("flush_path");
  admin_socket->unregister_command("session evict");
  admin_socket->unregister_command("session ls");
  admin_socket->unregister_command("flush journal");
  admin_socket->unregister_command("force_readonly");
  delete asok_hook;
  asok_hook = NULL;
}

const char** MDS::get_tracked_conf_keys() const
{
  static const char* KEYS[] = {
    "mds_op_complaint_time", "mds_op_log_threshold",
    "mds_op_history_size", "mds_op_history_duration",
    // clog & admin clog
    "clog_to_monitors",
    "clog_to_syslog",
    "clog_to_syslog_facility",
    "clog_to_syslog_level",
    NULL
  };
  return KEYS;
}

void MDS::handle_conf_change(const struct md_config_t *conf,
			     const std::set <std::string> &changed)
{
  if (changed.count("mds_op_complaint_time") ||
      changed.count("mds_op_log_threshold")) {
    op_tracker.set_complaint_and_threshold(conf->mds_op_complaint_time,
                                           conf->mds_op_log_threshold);
  }
  if (changed.count("mds_op_history_size") ||
      changed.count("mds_op_history_duration")) {
    op_tracker.set_history_size_and_duration(conf->mds_op_history_size,
                                             conf->mds_op_history_duration);
  }
  if (changed.count("clog_to_monitors") ||
      changed.count("clog_to_syslog") ||
      changed.count("clog_to_syslog_level") ||
      changed.count("clog_to_syslog_facility")) {
    update_log_config();
  }
}

void MDS::update_log_config()
{
  map<string,string> log_to_monitors;
  map<string,string> log_to_syslog;
  map<string,string> log_channel;
  map<string,string> log_prio;
  if (parse_log_client_options(g_ceph_context, log_to_monitors, log_to_syslog,
			       log_channel, log_prio) == 0)
    clog->update_config(log_to_monitors, log_to_syslog,
			log_channel, log_prio);
  derr << "log_to_monitors " << log_to_monitors << dendl;
}

void MDS::create_logger()
{
  dout(10) << "create_logger" << dendl;
  {
    PerfCountersBuilder mds_plb(g_ceph_context, "mds", l_mds_first, l_mds_last);

    mds_plb.add_u64_counter(l_mds_request, "request", "Requests");
    mds_plb.add_u64_counter(l_mds_reply, "reply", "Replies");
    mds_plb.add_time_avg(l_mds_reply_latency, "reply_latency",
        "Reply latency", "rlat");
    mds_plb.add_u64_counter(l_mds_forward, "forward", "Forwarding request");
    
    mds_plb.add_u64_counter(l_mds_dir_fetch, "dir_fetch", "Directory fetch");
    mds_plb.add_u64_counter(l_mds_dir_commit, "dir_commit", "Directory commit");
    mds_plb.add_u64_counter(l_mds_dir_split, "dir_split", "Directory split");

    mds_plb.add_u64(l_mds_inode_max, "inode_max", "Max inodes, cache size");
    mds_plb.add_u64(l_mds_inodes, "inodes", "Inodes", "inos");
    mds_plb.add_u64(l_mds_inodes_top, "inodes_top", "Inodes on top");
    mds_plb.add_u64(l_mds_inodes_bottom, "inodes_bottom", "Inodes on bottom");
    mds_plb.add_u64(l_mds_inodes_pin_tail, "inodes_pin_tail", "Inodes on pin tail");  
    mds_plb.add_u64(l_mds_inodes_pinned, "inodes_pinned", "Inodes pinned");
    mds_plb.add_u64(l_mds_inodes_expired, "inodes_expired", "Inodes expired");
    mds_plb.add_u64(l_mds_inodes_with_caps, "inodes_with_caps", "Inodes with capabilities");
    mds_plb.add_u64(l_mds_caps, "caps", "Capabilities", "caps");
    mds_plb.add_u64(l_mds_subtrees, "subtrees", "Subtrees");
    
    mds_plb.add_u64_counter(l_mds_traverse, "traverse", "Traverses"); 
    mds_plb.add_u64_counter(l_mds_traverse_hit, "traverse_hit", "Traverse hits");
    mds_plb.add_u64_counter(l_mds_traverse_forward, "traverse_forward", "Traverse forwards");
    mds_plb.add_u64_counter(l_mds_traverse_discover, "traverse_discover", "Traverse directory discovers");
    mds_plb.add_u64_counter(l_mds_traverse_dir_fetch, "traverse_dir_fetch", "Traverse incomplete directory content fetchings");
    mds_plb.add_u64_counter(l_mds_traverse_remote_ino, "traverse_remote_ino", "Traverse remote dentries");
    mds_plb.add_u64_counter(l_mds_traverse_lock, "traverse_lock", "Traverse locks");
    
    mds_plb.add_u64(l_mds_load_cent, "load_cent", "Load per cent");
    mds_plb.add_u64(l_mds_dispatch_queue_len, "q", "Dispatch queue length");
    
    mds_plb.add_u64_counter(l_mds_exported, "exported", "Exports");
    mds_plb.add_u64_counter(l_mds_exported_inodes, "exported_inodes", "Exported inodes");
    mds_plb.add_u64_counter(l_mds_imported, "imported", "Imports");
    mds_plb.add_u64_counter(l_mds_imported_inodes, "imported_inodes", "Imported inodes");
    logger = mds_plb.create_perf_counters();
    g_ceph_context->get_perfcounters_collection()->add(logger);
  }

  {
    PerfCountersBuilder mdm_plb(g_ceph_context, "mds_mem", l_mdm_first, l_mdm_last);
    mdm_plb.add_u64(l_mdm_ino, "ino", "Inodes");
    mdm_plb.add_u64_counter(l_mdm_inoa, "ino+", "Inodes opened");
    mdm_plb.add_u64_counter(l_mdm_inos, "ino-", "Inodes closed");
    mdm_plb.add_u64(l_mdm_dir, "dir", "Directories");
    mdm_plb.add_u64_counter(l_mdm_dira, "dir+", "Directories opened");
    mdm_plb.add_u64_counter(l_mdm_dirs, "dir-", "Directories closed");
    mdm_plb.add_u64(l_mdm_dn, "dn", "Dentries");
    mdm_plb.add_u64_counter(l_mdm_dna, "dn+", "Dentries opened");
    mdm_plb.add_u64_counter(l_mdm_dns, "dn-", "Dentries closed");
    mdm_plb.add_u64(l_mdm_cap, "cap", "Capabilities");
    mdm_plb.add_u64_counter(l_mdm_capa, "cap+", "Capabilities added");
    mdm_plb.add_u64_counter(l_mdm_caps, "cap-", "Capabilities removed");
    mdm_plb.add_u64(l_mdm_rss, "rss", "RSS");
    mdm_plb.add_u64(l_mdm_heap, "heap", "Heap size");
    mdm_plb.add_u64(l_mdm_malloc, "malloc", "Malloc size");
    mdm_plb.add_u64(l_mdm_buf, "buf", "Buffer size");
    mlogger = mdm_plb.create_perf_counters();
    g_ceph_context->get_perfcounters_collection()->add(mlogger);
  }

  mdlog->create_logger();
  server->create_logger();
  mdcache->register_perfcounters();
}
int MDS::init(MDSMap::DaemonState wanted_state)
{
  dout(10) << sizeof(MDSCacheObject) << "\tMDSCacheObject" << dendl;
  dout(10) << sizeof(CInode) << "\tCInode" << dendl;
  dout(10) << sizeof(elist<void*>::item) << "\t elist<>::item   *7=" << 7*sizeof(elist<void*>::item) << dendl;
  dout(10) << sizeof(inode_t) << "\t inode_t " << dendl;
  dout(10) << sizeof(nest_info_t) << "\t  nest_info_t " << dendl;
  dout(10) << sizeof(frag_info_t) << "\t  frag_info_t " << dendl;
  dout(10) << sizeof(SimpleLock) << "\t SimpleLock   *5=" << 5*sizeof(SimpleLock) << dendl;
  dout(10) << sizeof(ScatterLock) << "\t ScatterLock  *3=" << 3*sizeof(ScatterLock) << dendl;
  dout(10) << sizeof(CDentry) << "\tCDentry" << dendl;
  dout(10) << sizeof(elist<void*>::item) << "\t elist<>::item" << dendl;
  dout(10) << sizeof(SimpleLock) << "\t SimpleLock" << dendl;
  dout(10) << sizeof(CDir) << "\tCDir " << dendl;
  dout(10) << sizeof(elist<void*>::item) << "\t elist<>::item   *2=" << 2*sizeof(elist<void*>::item) << dendl;
  dout(10) << sizeof(fnode_t) << "\t fnode_t " << dendl;
  dout(10) << sizeof(nest_info_t) << "\t  nest_info_t *2" << dendl;
  dout(10) << sizeof(frag_info_t) << "\t  frag_info_t *2" << dendl;
  dout(10) << sizeof(Capability) << "\tCapability " << dendl;
  dout(10) << sizeof(xlist<void*>::item) << "\t xlist<>::item   *2=" << 2*sizeof(xlist<void*>::item) << dendl;

  objecter->init();

  messenger->add_dispatcher_tail(objecter);
  messenger->add_dispatcher_tail(&beacon);
  messenger->add_dispatcher_tail(this);

  // get monmap
  monc->set_messenger(messenger);

  monc->set_want_keys(CEPH_ENTITY_TYPE_MON | CEPH_ENTITY_TYPE_OSD | CEPH_ENTITY_TYPE_MDS);
  monc->init();

  finisher.start();

  // tell monc about log_client so it will know about mon session resets
  monc->set_log_client(&log_client);
  update_log_config();
  
  int r = monc->authenticate();
  if (r < 0) {
    derr << "ERROR: failed to authenticate: " << cpp_strerror(-r) << dendl;
    mds_lock.Lock();
    suicide();
    mds_lock.Unlock();
    return r;
  }
  while (monc->wait_auth_rotating(30.0) < 0) {
    derr << "unable to obtain rotating service keys; retrying" << dendl;
  }
  objecter->start();

  mds_lock.Lock();
  if (want_state == CEPH_MDS_STATE_DNE) {
    mds_lock.Unlock();
    return 0;
  }

  monc->sub_want("mdsmap", 0, 0);
  monc->renew_subs();

  mds_lock.Unlock();

  // verify that osds support tmap2omap
  while (true) {
    objecter->maybe_request_map();
    objecter->wait_for_osd_map();
    const OSDMap *osdmap = objecter->get_osdmap_read();
    uint64_t osd_features = osdmap->get_up_osd_features();
    if (osd_features & CEPH_FEATURE_OSD_TMAP2OMAP) {
      objecter->put_osdmap_read();
      break;
    }
    if (osdmap->get_num_up_osds() > 0) {
        derr << "*** one or more OSDs do not support TMAP2OMAP; upgrade OSDs before starting MDS (or downgrade MDS) ***" << dendl;
    } else {
        derr << "*** no OSDs are up as of epoch " << osdmap->get_epoch() << ", waiting" << dendl;
    }
    objecter->put_osdmap_read();
    sleep(10);
  }

  mds_lock.Lock();
  if (want_state == MDSMap::STATE_DNE) {
    suicide();  // we could do something more graceful here
  }

  timer.init();

  if (wanted_state==MDSMap::STATE_BOOT && g_conf->mds_standby_replay) {
    wanted_state = MDSMap::STATE_STANDBY_REPLAY;
  }

  // starting beacon.  this will induce an MDSMap from the monitor
  want_state = wanted_state;
  if (wanted_state==MDSMap::STATE_STANDBY_REPLAY ||
      wanted_state==MDSMap::STATE_ONESHOT_REPLAY) {
    g_conf->set_val_or_die("mds_standby_replay", "true");
    g_conf->apply_changes(NULL);
    if ( wanted_state == MDSMap::STATE_ONESHOT_REPLAY &&
        (g_conf->mds_standby_for_rank == -1) &&
        g_conf->mds_standby_for_name.empty()) {
      // uh-oh, must specify one or the other!
      dout(0) << "Specified oneshot replay mode but not an MDS!" << dendl;
      suicide();
    }
    want_state = MDSMap::STATE_BOOT;
    standby_type = wanted_state;
  }

  standby_for_rank = mds_rank_t(g_conf->mds_standby_for_rank);
  standby_for_name.assign(g_conf->mds_standby_for_name);

  if (wanted_state == MDSMap::STATE_STANDBY_REPLAY &&
      standby_for_rank == -1) {
    if (standby_for_name.empty())
      standby_for_rank = MDSMap::MDS_STANDBY_ANY;
    else
      standby_for_rank = MDSMap::MDS_STANDBY_NAME;
  } else if (standby_type == MDSMap::STATE_NULL && !standby_for_name.empty())
    standby_for_rank = MDSMap::MDS_MATCHED_ACTIVE;

  beacon.init(mdsmap, want_state, standby_for_rank, standby_for_name);
  whoami = MDS_RANK_NONE;
  messenger->set_myname(entity_name_t::MDS(whoami));
  
  // schedule tick
  reset_tick();

  // Start handler for finished_queue
  progress_thread.create();

  create_logger();
  set_up_admin_socket();
  g_conf->add_observer(this);

  mds_lock.Unlock();

  return 0;
}

void MDS::reset_tick()
{
  // cancel old
  if (tick_event) timer.cancel_event(tick_event);

  // schedule
  tick_event = new C_MDS_Tick(this);
  timer.add_event_after(g_conf->mds_tick_interval, tick_event);
}

void MDS::tick()
{
  heartbeat_reset();

  tick_event = 0;

  // reschedule
  reset_tick();

  if (beacon.is_laggy()) {
    dout(5) << "tick bailing out since we seem laggy" << dendl;
    return;
  } else {
    // Wake up thread in case we use to be laggy and have waiting_for_nolaggy
    // messages to progress.
    progress_thread.signal();
  }

  // make sure mds log flushes, trims periodically
  mdlog->flush();

  if (is_active() || is_stopping()) {
    mdcache->trim();
    mdcache->trim_client_leases();
    mdcache->check_memory_usage();
    mdlog->trim();  // NOT during recovery!
  }

  // log
  utime_t now = ceph_clock_now(g_ceph_context);
  mds_load_t load = balancer->get_load(now);
  
  if (logger) {
    logger->set(l_mds_load_cent, 100 * load.mds_load());
    logger->set(l_mds_dispatch_queue_len, messenger->get_dispatch_queue_len());
    logger->set(l_mds_subtrees, mdcache->num_subtrees());

    mdcache->log_stat();
  }

  // ...
  if (is_clientreplay() || is_active() || is_stopping()) {
    locker->tick();
    server->find_idle_sessions();
  }
  
  if (is_reconnect())
    server->reconnect_tick();
  
  if (is_active()) {
    balancer->tick();
    mdcache->find_stale_fragment_freeze();
    mdcache->migrator->find_stale_export_freeze();
    if (snapserver)
      snapserver->check_osd_map(false);
  }

  // Expose ourselves to Beacon to update health indicators
  beacon.notify_health(this);

  check_ops_in_flight();
}

void MDS::check_ops_in_flight()
{
  vector<string> warnings;
  if (op_tracker.check_ops_in_flight(warnings)) {
    for (vector<string>::iterator i = warnings.begin();
        i != warnings.end();
        ++i) {
      clog->warn() << *i;
    }
  }
  return;
}

/* This function DOES put the passed message before returning*/
void MDS::handle_command(MCommand *m)
{
  Session *session = static_cast<Session *>(m->get_connection()->get_priv());
  assert(session != NULL);

  int r = 0;
  cmdmap_t cmdmap;
  std::stringstream ss;
  std::string outs;
  bufferlist outbl;
  Context *run_after = NULL;


  if (!session->auth_caps.allow_all()) {
    dout(1) << __func__
      << ": received command from client without `tell` capability: "
      << m->get_connection()->peer_addr << dendl;

    ss << "permission denied";
    r = -EPERM;
  } else if (m->cmd.empty()) {
    ss << "no command given";
    outs = ss.str();
  } else if (!cmdmap_from_json(m->cmd, &cmdmap, ss)) {
    r = -EINVAL;
    outs = ss.str();
  } else {
    r = _handle_command(cmdmap, m->get_data(), &outbl, &outs, &run_after);
  }

  MCommandReply *reply = new MCommandReply(r, outs);
  reply->set_tid(m->get_tid());
  reply->set_data(outbl);
  m->get_connection()->send_message(reply);

  if (run_after) {
    run_after->complete(0);
  }

  m->put();
}


struct MDSCommand {
  string cmdstring;
  string helpstring;
  string module;
  string perm;
  string availability;
} mds_commands[] = {

#define COMMAND(parsesig, helptext, module, perm, availability) \
  {parsesig, helptext, module, perm, availability},

COMMAND("injectargs " \
	"name=injected_args,type=CephString,n=N",
	"inject configuration arguments into running MDS",
	"mds", "*", "cli,rest")
COMMAND("exit",
	"Terminate this MDS",
	"mds", "*", "cli,rest")
COMMAND("respawn",
	"Restart this MDS",
	"mds", "*", "cli,rest")
COMMAND("session kill " \
        "name=session_id,type=CephInt",
	"End a client session",
	"mds", "*", "cli,rest")
COMMAND("cpu_profiler " \
	"name=arg,type=CephChoices,strings=status|flush",
	"run cpu profiling on daemon", "mds", "rw", "cli,rest")
COMMAND("heap " \
	"name=heapcmd,type=CephChoices,strings=dump|start_profiler|stop_profiler|release|stats", \
	"show heap usage info (available only if compiled with tcmalloc)", \
	"mds", "*", "cli,rest")
};

// FIXME: reinstate issue_caps, try_eval,
//  *if* it makes sense to do so (or should these be admin socket things?)

/* This function DOES put the passed message before returning*/
void MDS::handle_command(MMonCommand *m)
{
  bufferlist outbl;
  _handle_command_legacy(m->cmd);
  m->put();
}

int MDS::_handle_command(
    const cmdmap_t &cmdmap,
    bufferlist const &inbl,
    bufferlist *outbl,
    std::string *outs,
    Context **run_later)
{
  assert(outbl != NULL);
  assert(outs != NULL);

  class SuicideLater : public Context
  {
    MDS *mds;

    public:
    SuicideLater(MDS *mds_) : mds(mds_) {}
    void finish(int r) {
      // Wait a little to improve chances of caller getting
      // our response before seeing us disappear from mdsmap
      sleep(1);

      mds->suicide();
    }
  };


  class RespawnLater : public Context
  {
    MDS *mds;

    public:

    RespawnLater(MDS *mds_) : mds(mds_) {}
    void finish(int r) {
      // Wait a little to improve chances of caller getting
      // our response before seeing us disappear from mdsmap
      sleep(1);

      mds->respawn();
    }
  };

  std::stringstream ds;
  std::stringstream ss;
  std::string prefix;
  cmd_getval(cct, cmdmap, "prefix", prefix);

  int r = 0;

  if (prefix == "get_command_descriptions") {
    int cmdnum = 0;
    JSONFormatter *f = new JSONFormatter();
    f->open_object_section("command_descriptions");
    for (MDSCommand *cp = mds_commands;
	 cp < &mds_commands[ARRAY_SIZE(mds_commands)]; cp++) {

      ostringstream secname;
      secname << "cmd" << setfill('0') << std::setw(3) << cmdnum;
      dump_cmddesc_to_json(f, secname.str(), cp->cmdstring, cp->helpstring,
			   cp->module, cp->perm, cp->availability);
      cmdnum++;
    }
    f->close_section();	// command_descriptions

    f->flush(ds);
    delete f;
  } else if (prefix == "injectargs") {
    vector<string> argsvec;
    cmd_getval(cct, cmdmap, "injected_args", argsvec);

    if (argsvec.empty()) {
      r = -EINVAL;
      ss << "ignoring empty injectargs";
      goto out;
    }
    string args = argsvec.front();
    for (vector<string>::iterator a = ++argsvec.begin(); a != argsvec.end(); ++a)
      args += " " + *a;
    cct->_conf->injectargs(args, &ss);
  } else if (prefix == "exit") {
    // We will send response before executing
    ss << "Exiting...";
    *run_later = new SuicideLater(this);
  }
  else if (prefix == "respawn") {
    // We will send response before executing
    ss << "Respawning...";
    *run_later = new RespawnLater(this);
  } else if (prefix == "session kill") {
    // FIXME harmonize `session kill` with admin socket session evict
    int64_t session_id = 0;
    bool got = cmd_getval(cct, cmdmap, "session_id", session_id);
    assert(got);
    Session *session = sessionmap.get_session(entity_name_t(CEPH_ENTITY_TYPE_CLIENT, session_id));

    if (session) {
      server->kill_session(session, NULL);
    } else {
      r = -ENOENT;
      ss << "session '" << session_id << "' not found";
    }
  } else if (prefix == "heap") {
    if (!ceph_using_tcmalloc()) {
      r = -EOPNOTSUPP;
      ss << "could not issue heap profiler command -- not using tcmalloc!";
    } else {
      string heapcmd;
      cmd_getval(cct, cmdmap, "heapcmd", heapcmd);
      vector<string> heapcmd_vec;
      get_str_vec(heapcmd, heapcmd_vec);
      ceph_heap_profiler_handle_command(heapcmd_vec, ds);
    }
  } else if (prefix == "cpu_profiler") {
    string arg;
    cmd_getval(cct, cmdmap, "arg", arg);
    vector<string> argvec;
    get_str_vec(arg, argvec);
    cpu_profiler_handle_command(argvec, ds);
  } else {
    std::ostringstream ss;
    ss << "unrecognized command! " << prefix;
    r = -EINVAL;
  }

out:
  *outs = ss.str();
  outbl->append(ds);
  return r;
}

/**
 * Legacy "mds tell", takes a simple array of args
 */
int MDS::_handle_command_legacy(std::vector<std::string> args)
{
  dout(10) << "handle_command args: " << args << dendl;
  if (args[0] == "injectargs") {
    if (args.size() < 2) {
      derr << "Ignoring empty injectargs!" << dendl;
    }
    else {
      std::ostringstream oss;
      mds_lock.Unlock();
      g_conf->injectargs(args[1], &oss);
      mds_lock.Lock();
      derr << "injectargs:" << dendl;
      derr << oss.str() << dendl;
    }
  }
  else if (args[0] == "dumpcache") {
    if (args.size() > 1)
      mdcache->dump_cache(args[1].c_str());
    else
      mdcache->dump_cache();
  }
  else if (args[0] == "exit") {
    suicide();
  }
  else if (args[0] == "respawn") {
    respawn();
  }
  else if (args[0] == "session" && args[1] == "kill") {
    Session *session = sessionmap.get_session(entity_name_t(CEPH_ENTITY_TYPE_CLIENT,
							    strtol(args[2].c_str(), 0, 10)));
    if (session)
      server->kill_session(session, NULL);
    else
      dout(15) << "session " << session << " not in sessionmap!" << dendl;
  } else if (args[0] == "issue_caps") {
    long inum = strtol(args[1].c_str(), 0, 10);
    CInode *in = mdcache->get_inode(inodeno_t(inum));
    if (in) {
      bool r = locker->issue_caps(in);
      dout(20) << "called issue_caps on inode "  << inum
	       << " with result " << r << dendl;
    } else dout(15) << "inode " << inum << " not in mdcache!" << dendl;
  } else if (args[0] == "try_eval") {
    long inum = strtol(args[1].c_str(), 0, 10);
    int mask = strtol(args[2].c_str(), 0, 10);
    CInode * ino = mdcache->get_inode(inodeno_t(inum));
    if (ino) {
      locker->try_eval(ino, mask);
      dout(20) << "try_eval(" << inum << ", " << mask << ")" << dendl;
    } else dout(15) << "inode " << inum << " not in mdcache!" << dendl;
  } else if (args[0] == "fragment_dir") {
    if (args.size() == 4) {
      filepath fp(args[1].c_str());
      CInode *in = mdcache->cache_traverse(fp);
      if (in) {
	frag_t fg;
	if (fg.parse(args[2].c_str())) {
	  CDir *dir = in->get_dirfrag(fg);
	  if (dir) {
	    if (dir->is_auth()) {
	      int by = atoi(args[3].c_str());
	      if (by)
		mdcache->split_dir(dir, by);
	      else
		dout(0) << "need to split by >0 bits" << dendl;
	    } else dout(0) << "dir " << dir->dirfrag() << " not auth" << dendl;
	  } else dout(0) << "dir " << in->ino() << " " << fg << " dne" << dendl;
	} else dout(0) << " frag " << args[2] << " does not parse" << dendl;
      } else dout(0) << "path " << fp << " not found" << dendl;
    } else dout(0) << "bad syntax" << dendl;
  } else if (args[0] == "merge_dir") {
    if (args.size() == 3) {
      filepath fp(args[1].c_str());
      CInode *in = mdcache->cache_traverse(fp);
      if (in) {
	frag_t fg;
	if (fg.parse(args[2].c_str())) {
	  mdcache->merge_dir(in, fg);
	} else dout(0) << " frag " << args[2] << " does not parse" << dendl;
      } else dout(0) << "path " << fp << " not found" << dendl;
    } else dout(0) << "bad syntax" << dendl;
  } else if (args[0] == "export_dir") {
    if (args.size() == 3) {
      filepath fp(args[1].c_str());
      mds_rank_t target = mds_rank_t(atoi(args[2].c_str()));
      if (target != whoami && mdsmap->is_up(target) && mdsmap->is_in(target)) {
	CInode *in = mdcache->cache_traverse(fp);
	if (in) {
	  CDir *dir = in->get_dirfrag(frag_t());
	  if (dir && dir->is_auth()) {
	    mdcache->migrator->export_dir(dir, target);
	  } else dout(0) << "bad export_dir path dirfrag frag_t() or dir not auth" << dendl;
	} else dout(0) << "bad export_dir path" << dendl;
      } else dout(0) << "bad export_dir target syntax" << dendl;
    } else dout(0) << "bad export_dir syntax" << dendl;
  } 
  else if (args[0] == "cpu_profiler") {
    ostringstream ss;
    cpu_profiler_handle_command(args, ss);
    clog->info() << ss.str();
  }
  else if (args[0] == "heap") {
    if (!ceph_using_tcmalloc())
      clog->info() << "tcmalloc not enabled, can't use heap profiler commands\n";
    else {
      ostringstream ss;
      vector<std::string> cmdargs;
      cmdargs.insert(cmdargs.begin(), args.begin()+1, args.end());
      ceph_heap_profiler_handle_command(cmdargs, ss);
      clog->info() << ss.str();
    }
  } else {
    dout(0) << "unrecognized command! " << args << dendl;
  }

  return 0;
}

/* This function deletes the passed message before returning. */
void MDS::handle_mds_map(MMDSMap *m)
{
  version_t epoch = m->get_epoch();
  dout(5) << "handle_mds_map epoch " << epoch << " from " << m->get_source() << dendl;

  // note source's map version
  if (m->get_source().is_mds() && 
      peer_mdsmap_epoch[mds_rank_t(m->get_source().num())] < epoch) {
    dout(15) << " peer " << m->get_source()
	     << " has mdsmap epoch >= " << epoch
	     << dendl;
    peer_mdsmap_epoch[mds_rank_t(m->get_source().num())] = epoch;
  }

  // is it new?
  if (epoch <= mdsmap->get_epoch()) {
    dout(5) << " old map epoch " << epoch << " <= " << mdsmap->get_epoch() 
	    << ", discarding" << dendl;
    m->put();
    return;
  }

  // keep old map, for a moment
  MDSMap *oldmap = mdsmap;
  int oldwhoami = whoami;
  MDSMap::DaemonState oldstate = state;
  entity_addr_t addr;

  // decode and process
  mdsmap = new MDSMap;
  mdsmap->decode(m->get_encoded());

  monc->sub_got("mdsmap", mdsmap->get_epoch());

  // Update Beacon early, so that if any of the below code for handling
  // state changes wants to send a beacon, it reflects the latest epoch.
  beacon.notify_mdsmap(mdsmap);

  // verify compatset
  CompatSet mdsmap_compat(get_mdsmap_compat_set_all());
  dout(10) << "     my compat " << mdsmap_compat << dendl;
  dout(10) << " mdsmap compat " << mdsmap->compat << dendl;
  if (!mdsmap_compat.writeable(mdsmap->compat)) {
    dout(0) << "handle_mds_map mdsmap compatset " << mdsmap->compat
	    << " not writeable with daemon features " << mdsmap_compat
	    << ", killing myself" << dendl;
    suicide();
    goto out;
  }

  // see who i am
  addr = messenger->get_myaddr();
  state = mdsmap->get_state_gid(mds_gid_t(monc->get_global_id()));
  incarnation = mdsmap->get_inc_gid(mds_gid_t(monc->get_global_id()));
  whoami = mdsmap->get_rank_gid(mds_gid_t(monc->get_global_id()));
  if (whoami == MDS_RANK_NONE && (
      state == MDSMap::STATE_STANDBY_REPLAY || state == MDSMap::STATE_ONESHOT_REPLAY)) {
    whoami = mdsmap->get_mds_info_gid(mds_gid_t(monc->get_global_id())).standby_for_rank;
  }

  dout(10) << "map says i am " << addr << " mds." << whoami << "." << incarnation
	   << " state " << ceph_mds_state_name(state) << dendl;

  // Once I hold a rank it can't be taken away without
  // restarting this daemon
  if (whoami != oldwhoami && oldwhoami != MDS_RANK_NONE) {
    derr << "Invalid rank transition " << oldwhoami << "->" << whoami << dendl;
    respawn();
  }

  // Validate state transitions while I hold a rank
  {
    bool state_valid = true;
    if (whoami != MDS_RANK_NONE && state != oldstate) {
      if (oldstate == MDSMap::STATE_REPLAY) {
        if (state != MDSMap::STATE_RESOLVE && state != MDSMap::STATE_RECONNECT) {
          state_valid = false;
        }
      } else if (oldstate == MDSMap::STATE_REJOIN) {
        if (state != MDSMap::STATE_ACTIVE
            && state != MDSMap::STATE_CLIENTREPLAY
            && state != MDSMap::STATE_STOPPED) {
          state_valid = false;
        }
      } else if (oldstate >= MDSMap::STATE_RECONNECT && oldstate < MDSMap::STATE_ACTIVE) {
        // Once I have entered replay, the only allowable transitions are to
        // the next state along in the sequence.
        if (state != oldstate + 1) {
          state_valid = false;
        }
      }
    }

    if (!state_valid) {
      derr << "Invalid state transition " << ceph_mds_state_name(oldstate)
        << "->" << ceph_mds_state_name(state) << dendl;
      respawn();
    }
  }

  // mark down any failed peers
  for (map<mds_gid_t,MDSMap::mds_info_t>::const_iterator p = oldmap->get_mds_info().begin();
       p != oldmap->get_mds_info().end();
       ++p) {
    if (mdsmap->get_mds_info().count(p->first) == 0) {
      dout(10) << " peer mds gid " << p->first << " removed from map" << dendl;
      messenger->mark_down(p->second.addr);
    }
  }

  if (state != oldstate)
    last_state = oldstate;

  if (state == MDSMap::STATE_STANDBY) {
    state = MDSMap::STATE_STANDBY;
    set_want_state(state);
    dout(1) << "handle_mds_map standby" << dendl;

    if (standby_type) // we want to be in standby_replay or oneshot_replay!
      request_state(standby_type);

    goto out;
  } else if (state == MDSMap::STATE_STANDBY_REPLAY) {
    if (standby_type != MDSMap::STATE_NULL && standby_type != MDSMap::STATE_STANDBY_REPLAY) {
      set_want_state(standby_type);
      beacon.send();
      state = oldstate;
      goto out;
    }
  }

  if (whoami == MDS_RANK_NONE) {
    if (want_state == MDSMap::STATE_STANDBY) {
      dout(10) << "dropped out of mdsmap, try to re-add myself" << dendl;
      state = MDSMap::STATE_BOOT;
      set_want_state(state);
      goto out;
    } else if (want_state == MDSMap::STATE_BOOT) {
      dout(10) << "not in map yet" << dendl;
    } else {
      // did i get kicked by someone else?
      if (g_conf->mds_enforce_unique_name) {
        if (mds_gid_t existing = mdsmap->find_mds_gid_by_name(name)) {
          MDSMap::mds_info_t& i = mdsmap->get_info_gid(existing);
          if (i.global_id > monc->get_global_id()) {
            dout(1) << "handle_mds_map i (" << addr
                    << ") dne in the mdsmap, new instance has larger gid " << i.global_id
                    << ", suicide" << dendl;
            // Call suicide() rather than respawn() because if someone else
            // has taken our ID, we don't want to keep restarting and
            // fighting them for the ID.
            suicide();
            goto out;
          }
        }
      }

      dout(1) << "handle_mds_map i (" << addr
          << ") dne in the mdsmap, respawning myself" << dendl;
      respawn();
    }
    goto out;
  }

  if (oldwhoami != whoami || oldstate != state) {
    // update messenger.
    if (state == MDSMap::STATE_STANDBY_REPLAY || state == MDSMap::STATE_ONESHOT_REPLAY) {
      dout(1) << "handle_mds_map i am now mds." << monc->get_global_id() << "." << incarnation
	      << "replaying mds." << whoami << "." << incarnation << dendl;
      messenger->set_myname(entity_name_t::MDS(monc->get_global_id()));
    } else {
      dout(1) << "handle_mds_map i am now mds." << whoami << "." << incarnation << dendl;
      messenger->set_myname(entity_name_t::MDS(whoami));
    }
  }

  // tell objecter my incarnation
  if (objecter->get_client_incarnation() != incarnation)
    objecter->set_client_incarnation(incarnation);

  // for debug
  if (g_conf->mds_dump_cache_on_map)
    mdcache->dump_cache();

  // did it change?
  if (oldstate != state) {
    dout(1) << "handle_mds_map state change "
	    << ceph_mds_state_name(oldstate) << " --> "
	    << ceph_mds_state_name(state) << dendl;
    set_want_state(state);

    if (oldstate == MDSMap::STATE_STANDBY_REPLAY) {
        dout(10) << "Monitor activated us! Deactivating replay loop" << dendl;
        assert (state == MDSMap::STATE_REPLAY);
    } else {
      // did i just recover?
      if ((is_active() || is_clientreplay()) &&
          (oldstate == MDSMap::STATE_CREATING ||
	   oldstate == MDSMap::STATE_REJOIN ||
	   oldstate == MDSMap::STATE_RECONNECT))
        recovery_done(oldstate);

      if (is_active()) {
        active_start();
      } else if (is_any_replay()) {
        replay_start();
      } else if (is_resolve()) {
        resolve_start();
      } else if (is_reconnect()) {
        reconnect_start();
      } else if (is_rejoin()) {
	rejoin_start();
      } else if (is_clientreplay()) {
        clientreplay_start();
      } else if (is_creating()) {
        boot_create();
      } else if (is_starting()) {
        boot_start();
      } else if (is_stopping()) {
        assert(oldstate == MDSMap::STATE_ACTIVE);
        stopping_start();
      }
    }
  }
  
  // RESOLVE
  // is someone else newly resolving?
  if (is_resolve() || is_reconnect() || is_rejoin() ||
      is_clientreplay() || is_active() || is_stopping()) {
    if (!oldmap->is_resolving() && mdsmap->is_resolving()) {
      set<mds_rank_t> resolve;
      mdsmap->get_mds_set(resolve, MDSMap::STATE_RESOLVE);
      dout(10) << " resolve set is " << resolve << dendl;
      calc_recovery_set();
      mdcache->send_resolves();
    }
  }
  
  // REJOIN
  // is everybody finally rejoining?
  if (is_rejoin() || is_clientreplay() || is_active() || is_stopping()) {
    // did we start?
    if (!oldmap->is_rejoining() && mdsmap->is_rejoining())
      rejoin_joint_start();

    // did we finish?
    if (g_conf->mds_dump_cache_after_rejoin &&
	oldmap->is_rejoining() && !mdsmap->is_rejoining()) 
      mdcache->dump_cache();      // for DEBUG only

    if (oldstate >= MDSMap::STATE_REJOIN) {
      // ACTIVE|CLIENTREPLAY|REJOIN => we can discover from them.
      set<mds_rank_t> olddis, dis;
      oldmap->get_mds_set(olddis, MDSMap::STATE_ACTIVE);
      oldmap->get_mds_set(olddis, MDSMap::STATE_CLIENTREPLAY);
      oldmap->get_mds_set(olddis, MDSMap::STATE_REJOIN);
      mdsmap->get_mds_set(dis, MDSMap::STATE_ACTIVE);
      mdsmap->get_mds_set(dis, MDSMap::STATE_CLIENTREPLAY);
      mdsmap->get_mds_set(dis, MDSMap::STATE_REJOIN);
      for (set<mds_rank_t>::iterator p = dis.begin(); p != dis.end(); ++p)
	if (*p != whoami &&            // not me
	    olddis.count(*p) == 0) {  // newly so?
	  mdcache->kick_discovers(*p);
	  mdcache->kick_open_ino_peers(*p);
	}
    }
  }

  if (oldmap->is_degraded() && !mdsmap->is_degraded() && state >= MDSMap::STATE_ACTIVE)
    dout(1) << "cluster recovered." << dendl;

  // did someone go active?
  if (oldstate >= MDSMap::STATE_CLIENTREPLAY &&
      (is_clientreplay() || is_active() || is_stopping())) {
    set<mds_rank_t> oldactive, active;
    oldmap->get_mds_set(oldactive, MDSMap::STATE_ACTIVE);
    oldmap->get_mds_set(oldactive, MDSMap::STATE_CLIENTREPLAY);
    mdsmap->get_mds_set(active, MDSMap::STATE_ACTIVE);
    mdsmap->get_mds_set(active, MDSMap::STATE_CLIENTREPLAY);
    for (set<mds_rank_t>::iterator p = active.begin(); p != active.end(); ++p) 
      if (*p != whoami &&            // not me
	  oldactive.count(*p) == 0)  // newly so?
	handle_mds_recovery(*p);
  }

  // did someone fail?
  //   new down?
  {
    set<mds_rank_t> olddown, down;
    oldmap->get_down_mds_set(&olddown);
    mdsmap->get_down_mds_set(&down);
    for (set<mds_rank_t>::iterator p = down.begin(); p != down.end(); ++p) {
      if (olddown.count(*p) == 0) {
        messenger->mark_down(oldmap->get_inst(*p).addr);
        handle_mds_failure(*p);
      }
    }
  }

  // did someone fail?
  //   did their addr/inst change?
  {
    set<mds_rank_t> up;
    mdsmap->get_up_mds_set(up);
    for (set<mds_rank_t>::iterator p = up.begin(); p != up.end(); ++p) {
      if (oldmap->have_inst(*p) &&
         oldmap->get_inst(*p) != mdsmap->get_inst(*p)) {
        messenger->mark_down(oldmap->get_inst(*p).addr);
        handle_mds_failure(*p);
      }
    }
  }

  if (is_clientreplay() || is_active() || is_stopping()) {
    // did anyone stop?
    set<mds_rank_t> oldstopped, stopped;
    oldmap->get_stopped_mds_set(oldstopped);
    mdsmap->get_stopped_mds_set(stopped);
    for (set<mds_rank_t>::iterator p = stopped.begin(); p != stopped.end(); ++p) 
      if (oldstopped.count(*p) == 0)      // newly so?
	mdcache->migrator->handle_mds_failure_or_stop(*p);
  }

  if (!is_any_replay())
    balancer->try_rebalance();

  {
    map<epoch_t,list<MDSInternalContextBase*> >::iterator p = waiting_for_mdsmap.begin();
    while (p != waiting_for_mdsmap.end() && p->first <= mdsmap->get_epoch()) {
      list<MDSInternalContextBase*> ls;
      ls.swap(p->second);
      waiting_for_mdsmap.erase(p++);
      finish_contexts(g_ceph_context, ls);
    }
  }

  if (is_active()) {
    // Before going active, set OSD epoch barrier to latest (so that
    // we don't risk handing out caps to clients with old OSD maps that
    // might not include barriers from the previous incarnation of this MDS)
    const OSDMap *osdmap = objecter->get_osdmap_read();
    const epoch_t osd_epoch = osdmap->get_epoch();
    objecter->put_osdmap_read();
    set_osd_epoch_barrier(osd_epoch);
  }

  mdcache->notify_mdsmap_changed();

 out:
  m->put();
  delete oldmap;
}

void MDS::bcast_mds_map()
{
  dout(7) << "bcast_mds_map " << mdsmap->get_epoch() << dendl;

  // share the map with mounted clients
  set<Session*> clients;
  sessionmap.get_client_session_set(clients);
  for (set<Session*>::const_iterator p = clients.begin();
       p != clients.end();
       ++p) 
    (*p)->connection->send_message(new MMDSMap(monc->get_fsid(), mdsmap));
  last_client_mdsmap_bcast = mdsmap->get_epoch();
}



void MDS::handle_mds_recovery(mds_rank_t who) 
{
  dout(5) << "handle_mds_recovery mds." << who << dendl;
  
  mdcache->handle_mds_recovery(who);

  if (mdsmap->get_tableserver() == whoami) {
    snapserver->handle_mds_recovery(who);
  }

  queue_waiters(waiting_for_active_peer[who]);
  waiting_for_active_peer.erase(who);
}

void MDS::handle_mds_failure(mds_rank_t who)
{
  if (who == whoami) {
    dout(5) << "handle_mds_failure for myself; not doing anything" << dendl;
    return;
  }
  dout(5) << "handle_mds_failure mds." << who << dendl;

  mdcache->handle_mds_failure(who);

  snapclient->handle_mds_failure(who);
}

void MDS::handle_signal(int signum)
{
  assert(signum == SIGINT || signum == SIGTERM);
  derr << "*** got signal " << sys_siglist[signum] << " ***" << dendl;
  {
    Mutex::Locker l(mds_lock);
    if (stopping) {
      return;
    }
    suicide();
  }
}

void MDS::damaged()
{
  assert(whoami != MDS_RANK_NONE);
  assert(mds_lock.is_locked_by_me());

  set_want_state(MDSMap::STATE_DAMAGED);
  monc->flush_log();  // Flush any clog error from before we were called
  beacon.notify_health(this);  // Include latest status in our swan song
  beacon.send_and_wait(g_conf->mds_mon_shutdown_timeout);

  // It's okay if we timed out and the mon didn't get our beacon, because
  // another daemon (or ourselves after respawn) will eventually take the
  // rank and report DAMAGED again when it hits same problem we did.

  respawn();  // Respawn into standby in case mon has other work for us
}

void MDS::suicide(bool fast)
{
  assert(mds_lock.is_locked());
  // It should never be possible to suicide to get called twice, because
  // anyone picking up mds_lock checks if stopping is true and drops
  // out if it is.
  assert(stopping == false);
  stopping = true;

  set_want_state(MDSMap::STATE_DNE); // whatever.

  if (!fast && !mdsmap->is_dne_gid(mds_gid_t(monc->get_global_id()))) {
    // Notify the MDSMonitor that we're dying, so that it doesn't have to
    // wait for us to go laggy.  Only do this if we're actually in the
    // MDSMap, because otherwise the MDSMonitor will drop our message.
    beacon.send_and_wait(1);
  }

  dout(1) << "suicide.  wanted " << ceph_mds_state_name(want_state)
	  << ", now " << ceph_mds_state_name(state) << dendl;

  mdlog->shutdown();

  finisher.stop(); // no flushing

  // stop timers
  beacon.shutdown();
  if (tick_event) {
    timer.cancel_event(tick_event);
    tick_event = 0;
  }
  timer.cancel_all_events();
  //timer.join();
  timer.shutdown();
  
  clean_up_admin_socket();

  // shut down cache
  mdcache->shutdown();

  if (objecter->initialized.read())
    objecter->shutdown();

  monc->shutdown();

  op_tracker.on_shutdown();

  progress_thread.shutdown();

  // shut down messenger
  messenger->shutdown();

  // Workaround unclean shutdown: HeartbeatMap will assert if
  // worker is not removed (as we do in ~MDS), but ~MDS is not
  // always called after suicide.
  if (hb) {
    cct->get_heartbeat_map()->remove_worker(hb);
    hb = NULL;
  }
}

void MDS::respawn()
{
  dout(1) << "respawn" << dendl;

  char *new_argv[orig_argc+1];
  dout(1) << " e: '" << orig_argv[0] << "'" << dendl;
  for (int i=0; i<orig_argc; i++) {
    new_argv[i] = (char *)orig_argv[i];
    dout(1) << " " << i << ": '" << orig_argv[i] << "'" << dendl;
  }
  new_argv[orig_argc] = NULL;

  /* Determine the path to our executable, try to read
   * linux-specific /proc/ path first */
  char exe_path[PATH_MAX];
  ssize_t exe_path_bytes = readlink("/proc/self/exe", exe_path,
				    sizeof(exe_path) - 1);
  if (exe_path_bytes < 0) {
    /* Print CWD for the user's interest */
    char buf[PATH_MAX];
    char *cwd = getcwd(buf, sizeof(buf));
    assert(cwd);
    dout(1) << " cwd " << cwd << dendl;

    /* Fall back to a best-effort: just running in our CWD */
    strncpy(exe_path, orig_argv[0], sizeof(exe_path) - 1);
  } else {
    exe_path[exe_path_bytes] = '\0';
  }

  dout(1) << " exe_path " << exe_path << dendl;

  unblock_all_signals(NULL);
  execv(exe_path, new_argv);

  dout(0) << "respawn execv " << orig_argv[0]
	  << " failed with " << cpp_strerror(errno) << dendl;

  // We have to assert out here, because suicide() returns, and callers
  // to respawn expect it never to return.
  assert(0);
}



bool MDS::ms_dispatch(Message *m)
{
  Mutex::Locker l(mds_lock);
  if (stopping) {
    return false;
  }

  heartbeat_reset();

  // Drop out early if shutting down
  if (want_state == CEPH_MDS_STATE_DNE) {
    dout(10) << " stopping, discarding " << *m << dendl;
    m->put();
    return true;
  }

  // First see if it's a daemon message
  const bool handled_core = handle_core_message(m); 
  if (handled_core) {
    return true;
  }

  // Not core, try it as a rank message
  return handle_rank_message(m);
}

bool MDS::ms_get_authorizer(int dest_type, AuthAuthorizer **authorizer, bool force_new)
{
  dout(10) << "MDS::ms_get_authorizer type=" << ceph_entity_type_name(dest_type) << dendl;

  /* monitor authorization is being handled on different layer */
  if (dest_type == CEPH_ENTITY_TYPE_MON)
    return true;

  if (force_new) {
    if (monc->wait_auth_rotating(10) < 0)
      return false;
  }

  *authorizer = monc->auth->build_authorizer(dest_type);
  return *authorizer != NULL;
}


/*
 * high priority messages we always process
 */
bool MDS::handle_core_message(Message *m)
{
  switch (m->get_type()) {
  case CEPH_MSG_MON_MAP:
    ALLOW_MESSAGES_FROM(CEPH_ENTITY_TYPE_MON);
    m->put();
    break;

    // MDS
  case CEPH_MSG_MDS_MAP:
    ALLOW_MESSAGES_FROM(CEPH_ENTITY_TYPE_MON | CEPH_ENTITY_TYPE_MDS);
    handle_mds_map(static_cast<MMDSMap*>(m));
    break;

    // misc
  case MSG_MON_COMMAND:
    ALLOW_MESSAGES_FROM(CEPH_ENTITY_TYPE_MON);
    handle_command(static_cast<MMonCommand*>(m));
    break;    

    // OSD
  case MSG_COMMAND:
    handle_command(static_cast<MCommand*>(m));
    break;
  case CEPH_MSG_OSD_MAP:
    ALLOW_MESSAGES_FROM(CEPH_ENTITY_TYPE_MON | CEPH_ENTITY_TYPE_OSD);

    if (is_active() && snapserver) {
      snapserver->check_osd_map(true);
    }

    server->handle_osd_map();

    // By default the objecter only requests OSDMap updates on use,
    // we would like to always receive the latest maps in order to
    // apply policy based on the FULL flag.
    objecter->maybe_request_map();

    break;

  default:
    return false;
  }
  return true;
}






void MDS::ms_handle_connect(Connection *con) 
{
}

bool MDS::ms_handle_reset(Connection *con) 
{
  if (con->get_peer_type() != CEPH_ENTITY_TYPE_CLIENT)
    return false;

  Mutex::Locker l(mds_lock);
  if (stopping) {
    return false;
  }
  dout(5) << "ms_handle_reset on " << con->get_peer_addr() << dendl;
  if (want_state == CEPH_MDS_STATE_DNE)
    return false;

  Session *session = static_cast<Session *>(con->get_priv());
  if (session) {
    if (session->is_closed()) {
      dout(3) << "ms_handle_reset closing connection for session " << session->info.inst << dendl;
      con->mark_down();
      con->set_priv(NULL);
    }
    session->put();
  } else {
    con->mark_down();
  }
  return false;
}


void MDS::ms_handle_remote_reset(Connection *con) 
{
  if (con->get_peer_type() != CEPH_ENTITY_TYPE_CLIENT)
    return;

  Mutex::Locker l(mds_lock);
  if (stopping) {
    return;
  }

  dout(5) << "ms_handle_remote_reset on " << con->get_peer_addr() << dendl;
  if (want_state == CEPH_MDS_STATE_DNE)
    return;

  Session *session = static_cast<Session *>(con->get_priv());
  if (session) {
    if (session->is_closed()) {
      dout(3) << "ms_handle_remote_reset closing connection for session " << session->info.inst << dendl;
      con->mark_down();
      con->set_priv(NULL);
    }
    session->put();
  }
}

bool MDS::ms_verify_authorizer(Connection *con, int peer_type,
			       int protocol, bufferlist& authorizer_data, bufferlist& authorizer_reply,
			       bool& is_valid, CryptoKey& session_key)
{
  Mutex::Locker l(mds_lock);
  if (stopping) {
    return false;
  }
  if (want_state == CEPH_MDS_STATE_DNE)
    return false;

  AuthAuthorizeHandler *authorize_handler = 0;
  switch (peer_type) {
  case CEPH_ENTITY_TYPE_MDS:
    authorize_handler = authorize_handler_cluster_registry->get_handler(protocol);
    break;
  default:
    authorize_handler = authorize_handler_service_registry->get_handler(protocol);
  }
  if (!authorize_handler) {
    dout(0) << "No AuthAuthorizeHandler found for protocol " << protocol << dendl;
    is_valid = false;
    return true;
  }

  AuthCapsInfo caps_info;
  EntityName name;
  uint64_t global_id;

  is_valid = authorize_handler->verify_authorizer(cct, monc->rotating_secrets,
						  authorizer_data, authorizer_reply, name, global_id, caps_info, session_key);

  if (is_valid) {
    // wire up a Session* to this connection, and add it to the session map
    entity_name_t n(con->get_peer_type(), global_id);
    Session *s = sessionmap.get_session(n);
    if (!s) {
      s = new Session;
      s->info.inst.addr = con->get_peer_addr();
      s->info.inst.name = n;
      dout(10) << " new session " << s << " for " << s->info.inst << " con " << con << dendl;
      con->set_priv(s);
      s->connection = con;
    } else {
      dout(10) << " existing session " << s << " for " << s->info.inst << " existing con " << s->connection
	       << ", new/authorizing con " << con << dendl;
      con->set_priv(s->get());



      // Wait until we fully accept the connection before setting
      // s->connection.  In particular, if there are multiple incoming
      // connection attempts, they will all get their authorizer
      // validated, but some of them may "lose the race" and get
      // dropped.  We only want to consider the winner(s).  See
      // ms_handle_accept().  This is important for Sessions we replay
      // from the journal on recovery that don't have established
      // messenger state; we want the con from only the winning
      // connect attempt(s).  (Normal reconnects that don't follow MDS
      // recovery are reconnected to the existing con by the
      // messenger.)
    }

    if (caps_info.allow_all) {
        // Flag for auth providers that don't provide cap strings
        s->auth_caps.set_allow_all();
    }

    bufferlist::iterator p = caps_info.caps.begin();
    string auth_cap_str;
    try {
      ::decode(auth_cap_str, p);

      dout(10) << __func__ << ": parsing auth_cap_str='" << auth_cap_str << "'" << dendl;
      std::ostringstream errstr;
      if (!s->auth_caps.parse(auth_cap_str, &errstr)) {
        dout(1) << __func__ << ": auth cap parse error: " << errstr.str()
          << " parsing '" << auth_cap_str << "'" << dendl;
      }
    } catch (buffer::error& e) {
      // Assume legacy auth, defaults to:
      //  * permit all filesystem ops
      //  * permit no `tell` ops
      dout(1) << __func__ << ": cannot decode auth caps bl of length " << caps_info.caps.length() << dendl;
    }
  }

  return true;  // we made a decision (see is_valid)
}


void MDS::ms_handle_accept(Connection *con)
{
  Mutex::Locker l(mds_lock);
  if (stopping) {
    return;
  }

  Session *s = static_cast<Session *>(con->get_priv());
  dout(10) << "ms_handle_accept " << con->get_peer_addr() << " con " << con << " session " << s << dendl;
  if (s) {
    if (s->connection != con) {
      dout(10) << " session connection " << s->connection << " -> " << con << dendl;
      s->connection = con;

      // send out any queued messages
      while (!s->preopen_out_queue.empty()) {
	con->send_message(s->preopen_out_queue.front());
	s->preopen_out_queue.pop_front();
      }
    }
    s->put();
  }
}

