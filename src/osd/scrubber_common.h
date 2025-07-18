// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#pragma once

#include <iosfwd>
#include <set>
#include <string>
#include <string_view>

#include <fmt/ranges.h>

#include "common/ceph_time.h"
#include "common/fmt_common.h"
#include "common/scrub_types.h"
#include "include/random.h" // for ceph::util::generate_random_number()
#include "include/types.h"
#include "messages/MOSDScrubReserve.h"
#include "os/ObjectStore.h"
#include "osd/osd_perf_counters.h" // for osd_counter_idx_t

#include "ECUtil.h"
#include "OpRequest.h"

namespace ceph {
class Formatter;
}

struct PGPool;
using ScrubClock = ceph::coarse_real_clock;
using ScrubTimePoint = ScrubClock::time_point;

namespace Scrub {
  class ReplicaReservations;
  struct ReplicaActive;
  class ScrubJob;
  struct SchedEntry;
}

/// reservation-related data sent by the primary to the replicas,
/// and used to match the responses to the requests
struct AsyncScrubResData {
  spg_t pgid;
  pg_shard_t from;
  epoch_t request_epoch;
  MOSDScrubReserve::reservation_nonce_t nonce;
  AsyncScrubResData(
      spg_t pgid,
      pg_shard_t from,
      epoch_t request_epoch,
      MOSDScrubReserve::reservation_nonce_t nonce)
      : pgid{pgid}
      , from{from}
      , request_epoch{request_epoch}
      , nonce{nonce}
  {}
  template <typename FormatContext>
  auto fmt_print_ctx(FormatContext& ctx) const
  {
    return fmt::format_to(
	ctx.out(), "pg[{}],f:{},ep:{},n:{}", pgid, from, request_epoch, nonce);
  }
};


/// Facilitating scrub-related object access to private PG data
class ScrubberPasskey {
private:
  friend class Scrub::ReplicaReservations;
  friend struct Scrub::ReplicaActive;
  friend class PrimaryLogScrub;
  friend class PgScrubber;
  friend class ScrubBackend;
  ScrubberPasskey() {}
  ScrubberPasskey(const ScrubberPasskey&) = default;
  ScrubberPasskey& operator=(const ScrubberPasskey&) = delete;
};

/// randomly returns true with probability equal to the passed parameter
static inline bool random_bool_with_probability(double probability) {
  return (ceph::util::generate_random_number<double>(0.0, 1.0) < probability);
}

namespace Scrub {

/// high/low OP priority
enum class scrub_prio_t : bool { low_priority = false, high_priority = true };

/// Identifies a specific scrub activation within an interval,
/// see ScrubPGgIF::m_current_token
using act_token_t = uint32_t;

/// "environment" preconditions affecting which PGs are eligible for scrubbing
/// (note: struct size should be kept small, as it is copied around)
struct OSDRestrictions {
  /// high local OSD concurrency. Thus - only high priority scrubs are allowed
  bool max_concurrency_reached{false};

  /// rolled a dice, and decided not to scrub in this tick
  bool random_backoff_active{false};

  /// the CPU load is high. No regular scrubs are allowed.
  bool cpu_overloaded:1{false};

  /// outside of allowed scrubbing hours/days
  bool restricted_time:1{false};

  /// the OSD is performing a recovery & osd_scrub_during_recovery is 'false'
  bool recovery_in_progress:1{false};
};
static_assert(sizeof(Scrub::OSDRestrictions) <= sizeof(uint32_t));

/// concise passing of PG state affecting scrub to the
/// scrubber at the initiation of a scrub
struct ScrubPGPreconds {
  bool allow_shallow{true};
  bool allow_deep{true};
  bool can_autorepair{false};
};
static_assert(sizeof(Scrub::ScrubPGPreconds) <= sizeof(uint32_t));

/// possible outcome when trying to select a PG and scrub it
enum class schedule_result_t {
  scrub_initiated,	    // successfully started a scrub
  target_specific_failure,  // failed to scrub this specific target
  osd_wide_failure	    // failed to scrub any target
};

/// a collection of the basic scheduling information of a scrub target:
/// target time to scrub, and the 'not before'.
struct scrub_schedule_t {
  /**
   * the time at which we are allowed to start the scrub. Never
   * decreasing after 'scheduled_at' is set.
   */
  utime_t not_before{utime_t::max()};

  /**
   * the 'scheduled_at' is the time at which we intended the scrub to be scheduled.
   * For periodic (regular) scrubs, it is set to the time of the last scrub
   * plus the scrub interval (plus some randomization). Priority scrubs
   * have their own specific rules for the target time. E.g.:
   * - for operator-initiated scrubs: 'target' is set to 'scrub_must_stamp';
   * - same for re-scrubbing (deep scrub after a shallow scrub that ended with
   *   errors;
   * - when requesting a scrub after a repair (the highest priority scrub):
   *   the target is set to '0' (beginning of time);
   */
  utime_t scheduled_at{utime_t::max()};

  std::partial_ordering operator<=>(const scrub_schedule_t& rhs) const
  {
    // when compared - the 'not_before' is ignored, assuming
    // we never compare jobs with different eligibility status.
    return scheduled_at <=> rhs.scheduled_at;
  };

  bool operator==(const scrub_schedule_t& rhs) const = default;
};

}  // namespace Scrub

namespace fmt {
template <>
struct formatter<Scrub::ScrubPGPreconds> {
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

  template <typename FormatContext>
  auto format(const Scrub::ScrubPGPreconds& conds, FormatContext& ctx) const
  {
    return fmt::format_to(
	ctx.out(), "allowed(shallow/deep):{:1}/{:1},can-autorepair:{:1}",
	conds.allow_shallow, conds.allow_deep, conds.can_autorepair);
  }
};

template <>
struct formatter<Scrub::OSDRestrictions> {
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

  template <typename FormatContext>
  auto format(const Scrub::OSDRestrictions& conds, FormatContext& ctx) const {
    return fmt::format_to(
	ctx.out(), "<{}.{}.{}.{}.{}>",
	conds.max_concurrency_reached ? "max-scrubs" : "",
	conds.random_backoff_active ? "backoff" : "",
	conds.cpu_overloaded ? "high-load" : "",
	conds.restricted_time ? "time-restrict" : "",
	conds.recovery_in_progress ? "recovery" : "");
  }
};

template <>
struct formatter<Scrub::scrub_schedule_t> {
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const Scrub::scrub_schedule_t& sc, FormatContext& ctx) const {
    return fmt::format_to(
	ctx.out(), "nb:{:s}(at:{:s})", sc.not_before, sc.scheduled_at);
  }
};

}  // namespace fmt

namespace Scrub {

/**
 * the result of the last attempt to schedule a scrub for a specific PG.
 * The enum value itself is mostly used for logging purposes.
 */
enum class delay_cause_t {
  none,		    ///< scrub attempt was successful
  replicas,	    ///< failed to reserve replicas
  flags,	    ///< noscrub or nodeep-scrub
  pg_state,	    ///< not active+clean
  snap_trimming,    ///< snap-trimming is in progress
  restricted_time,  ///< time restrictions or busy CPU
  local_resources,  ///< too many scrubbing PGs
  aborted,	    ///< scrub was aborted w/ unspecified reason
  interval,	    ///< the interval had ended mid-scrub
  scrub_params,     ///< the specific scrub type is not allowed
};
}  // namespace Scrub

namespace fmt {
// clang-format off
template <>
struct formatter<Scrub::delay_cause_t> : ::fmt::formatter<std::string_view> {
  template <typename FormatContext>
  auto format(Scrub::delay_cause_t cause, FormatContext& ctx) const
  {
    using enum Scrub::delay_cause_t;
    std::string_view desc;
    switch (cause) {
      case none:                desc = "ok"; break;
      case replicas:            desc = "replicas"; break;
      case flags:               desc = "noscrub"; break;
      case pg_state:            desc = "pg-state"; break;
      case snap_trimming:       desc = "snap-trim"; break;
      case restricted_time:     desc = "time/load"; break;
      case local_resources:     desc = "local-cnt"; break;
      case aborted:             desc = "aborted"; break;
      case interval:            desc = "interval"; break;
      case scrub_params:        desc = "scrub-mode"; break;
      // better to not have a default case, so that the compiler will warn
    }
    return ::fmt::formatter<string_view>::format(desc, ctx);
  }
};
// clang-format on
}  // namespace fmt


namespace Scrub {

/// PG services used by the scrubber backend
struct PgScrubBeListener {
  virtual ~PgScrubBeListener() = default;

  virtual const PGPool& get_pgpool() const = 0;
  virtual pg_shard_t get_primary() const = 0;
  virtual void force_object_missing(ScrubberPasskey,
                                    const std::set<pg_shard_t>& peer,
                                    const hobject_t& oid,
                                    eversion_t version) = 0;
  virtual const pg_info_t& get_pg_info(ScrubberPasskey) const = 0;

  // query the PG backend for the on-disk size of an object
  virtual uint64_t logical_to_ondisk_size(uint64_t logical_size,
                                 shard_id_t shard_id) const = 0;

  // used to verify our "cleanliness" before scrubbing
  virtual bool is_waiting_for_unreadable_object() const = 0;

  // A non-primary shard is one which can never become primary. It may
  // have an old version and cannot be considered authoritative.
  virtual bool get_is_nonprimary_shard(const pg_shard_t &pg_shard) const = 0;

  // hinfo objects are not used for some EC configurations. Do not raise scrub
  // errors on hinfo if they should not exist.
  virtual bool get_is_hinfo_required() const = 0;

  // If true, the EC optimisations have been enabled.
  virtual bool get_is_ec_optimized() const = 0;

  // If true, EC can decode all shards using the available shards
  virtual bool ec_can_decode(const shard_id_set& available_shards) const = 0;

  // Returns a map of the data + encoded parity shards when supplied with
  // a bufferlist containing the data shards
  virtual shard_id_map<bufferlist> ec_encode_acting_set(
      const bufferlist& in_bl) const = 0;

  // Returns a map of all shards when given a map with missing shards that need
  // to be decoded
  virtual shard_id_map<bufferlist> ec_decode_acting_set(
      const shard_id_map<bufferlist>& shard_map, int chunk_size) const = 0;

  // If true, the EC profile supports passing CRCs through the EC plugin encode
  // and decode functions to get a resulting CRC that is the same as if you were
  // to encode or decode the data and take the CRC of the resulting shards
  virtual bool get_ec_supports_crc_encode_decode() const = 0;

  // Returns the stripe_info_t used by the PG in EC
  virtual ECUtil::stripe_info_t get_ec_sinfo() const = 0;
};

// defining a specific subset of performance counters. Each of the members
// is set to (the index of) the corresponding performance counter.
// Separate sets are used for replicated and erasure-coded pools.
struct ScrubCounterSet {
  osd_counter_idx_t getattr_cnt; ///< get_attr calls count
  osd_counter_idx_t stats_cnt;  ///< stats calls count
  osd_counter_idx_t read_cnt;   ///< read calls count
  osd_counter_idx_t read_bytes;  ///< total bytes read
  osd_counter_idx_t omapgetheader_cnt; ///< omap get header calls count
  osd_counter_idx_t omapgetheader_bytes;  ///< bytes read by omap get header
  osd_counter_idx_t omapget_cnt;  ///< omap get calls count
  osd_counter_idx_t omapget_bytes;  ///< total bytes read by omap get
  osd_counter_idx_t started_cnt; ///< the number of times we started a scrub
  osd_counter_idx_t active_started_cnt; ///< scrubs that got past reservation
  osd_counter_idx_t successful_cnt; ///< successful scrubs count
  osd_counter_idx_t successful_elapsed; ///< time to complete a successful scrub
  osd_counter_idx_t failed_cnt; ///< failed scrubs count
  osd_counter_idx_t failed_elapsed; ///< time from start to failure
  // reservation process related:
  osd_counter_idx_t rsv_successful_cnt; ///< completed reservation processes
  osd_counter_idx_t rsv_successful_elapsed; ///< time to all-reserved
  osd_counter_idx_t rsv_aborted_cnt; ///< failed due to an abort
  osd_counter_idx_t rsv_rejected_cnt; ///< 'rejected' response
  osd_counter_idx_t rsv_skipped_cnt; ///< high-priority. No reservation
  osd_counter_idx_t rsv_failed_elapsed; ///< time for reservation to fail
  osd_counter_idx_t rsv_secondaries_num; ///< number of replicas (EC or rep)
};

}  // namespace Scrub


/**
 *  The interface used by the PG when requesting scrub-related info or services
 */
struct ScrubPgIF {

  virtual ~ScrubPgIF() = default;

  friend std::ostream& operator<<(std::ostream& out, const ScrubPgIF& s) {
    return s.show_concise(out);
  }

  virtual std::ostream& show_concise(std::ostream& out) const = 0;

  // --------------- triggering state-machine events:

  virtual void initiate_regular_scrub(epoch_t epoch_queued) = 0;

  virtual void send_scrub_resched(epoch_t epoch_queued) = 0;

  virtual void active_pushes_notification(epoch_t epoch_queued) = 0;

  virtual void update_applied_notification(epoch_t epoch_queued) = 0;

  virtual void digest_update_notification(epoch_t epoch_queued) = 0;

  virtual void send_scrub_unblock(epoch_t epoch_queued) = 0;

  virtual void send_replica_maps_ready(epoch_t epoch_queued) = 0;

  virtual void send_replica_pushes_upd(epoch_t epoch_queued) = 0;

  virtual void send_start_replica(epoch_t epoch_queued,
				  Scrub::act_token_t token) = 0;

  virtual void send_sched_replica(epoch_t epoch_queued,
				  Scrub::act_token_t token) = 0;

  virtual void send_chunk_free(epoch_t epoch_queued) = 0;

  virtual void send_chunk_busy(epoch_t epoch_queued) = 0;

  virtual void send_local_map_done(epoch_t epoch_queued) = 0;

  virtual void send_get_next_chunk(epoch_t epoch_queued) = 0;

  virtual void send_scrub_is_finished(epoch_t epoch_queued) = 0;

  virtual void send_granted_by_reserver(const AsyncScrubResData& req) = 0;

  virtual void on_applied_when_primary(const eversion_t& applied_version) = 0;

  // --------------------------------------------------

  [[nodiscard]] virtual bool are_callbacks_pending() const = 0;	 // currently
								 // only used
								 // for an
								 // assert

  /**
   * the scrubber is marked 'active':
   * - for the primary: when all replica OSDs grant us the requested resources
   * - for replicas: upon receiving the scrub request from the primary
   */
  [[nodiscard]] virtual bool is_scrub_active() const = 0;

  /**
   * 'true' until after the FSM processes the 'scrub-finished' event,
   * and scrubbing is completely cleaned-up.
   *
   * In other words - holds longer than is_scrub_active(), thus preventing
   * a rescrubbing of the same PG while the previous scrub has not fully
   * terminated.
   */
  [[nodiscard]] virtual bool is_queued_or_active() const = 0;

  /**
   * Manipulate the 'scrubbing request has been queued, or - we are
   * actually scrubbing' Scrubber's flag
   *
   * clear_queued_or_active() will also restart any blocked snaptrimming.
   */
  virtual void set_queued_or_active() = 0;
  virtual void clear_queued_or_active() = 0;

  /// are we waiting for resource reservation grants form our replicas?
  [[nodiscard]] virtual bool is_reserving() const = 0;

  /// handle a message carrying a replica map
  virtual void map_from_replica(OpRequestRef op) = 0;

  virtual void replica_scrub_op(OpRequestRef op) = 0;

  /**
   * attempt to initiate a scrub session.
   * param s_or_d: the scrub level to start. This identifies the specific
   *   target to be scrubbed.
   * @param osd_restrictions limitations on the types of scrubs that can
   *   be initiated on this OSD at this time.
   * @param preconds the PG state re scrubbing at the time of the request,
   *   affecting scrub parameters.
   * @param requested_flags the set of flags that determine the scrub type
   *   and attributes (to be removed in the next iteration).
   * @return the result of the scrub initiation attempt. A success,
   *   or either a failure due to the specific PG, or a failure due to
   *   external reasons.
   */
  virtual Scrub::schedule_result_t start_scrub_session(
      scrub_level_t s_or_d,
      Scrub::OSDRestrictions osd_restrictions,
      Scrub::ScrubPGPreconds pg_cond) = 0;

  virtual void set_op_parameters(Scrub::ScrubPGPreconds pg_cond) = 0;

  /// stop any active scrubbing (on interval end) and unregister from
  /// the OSD scrub queue
  virtual void on_new_interval() = 0;

  /// we are peered as primary, and the PG is active and clean
  /// Scrubber's internal FSM should be ActivePrimary
  virtual void on_primary_active_clean() = 0;

  /// we are peered as a replica
  virtual void on_replica_activate() = 0;

  virtual void handle_query_state(ceph::Formatter* f) = 0;

  virtual pg_scrubbing_status_t get_schedule() const = 0;

  // // perform 'scrub'/'deep_scrub' asok commands

  /// ... by faking the "last scrub" stamps
  virtual void on_operator_periodic_cmd(
    ceph::Formatter* f,
    scrub_level_t scrub_level,
    int64_t offset) = 0;

  /// ... by requesting an "operator initiated" scrub
  virtual void on_operator_forced_scrub(
    ceph::Formatter* f,
    scrub_level_t scrub_level) = 0;

  virtual void dump_scrubber(ceph::Formatter* f) const = 0;

  /**
   * Return true if soid is currently being scrubbed and pending IOs should
   * block. May have a side effect of preempting an in-progress scrub -- will
   * return false in that case.
   *
   * @param soid object to check for ongoing scrub
   * @return boolean whether a request on soid should block until scrub
   * completion
   */
  virtual bool write_blocked_by_scrub(const hobject_t& soid) = 0;

  /// Returns whether any objects in the range [begin, end] are being scrubbed
  virtual bool range_intersects_scrub(const hobject_t& start,
				      const hobject_t& end) = 0;

  /// the op priority, taken from the primary's request message
  virtual Scrub::scrub_prio_t replica_op_priority() const = 0;

  /// the priority of the on-going scrub (used when requeuing events)
  virtual unsigned int scrub_requeue_priority(
    Scrub::scrub_prio_t with_priority) const = 0;
  virtual unsigned int scrub_requeue_priority(
    Scrub::scrub_prio_t with_priority,
    unsigned int suggested_priority) const = 0;

  virtual void add_callback(Context* context) = 0;

  /// add to scrub statistics, but only if the soid is below the scrub start
  virtual void stats_of_handled_objects(const object_stat_sum_t& delta_stats,
					const hobject_t& soid) = 0;

  /**
   * clears both internal scrub state, and some PG-visible flags:
   * - the two scrubbing PG state flags;
   * - primary/replica scrub position (chunk boundaries);
   * - primary/replica interaction state;
   * - the backend state
   * Also runs pending callbacks, and clears the active flags.
   * Does not try to invoke FSM events.
   */
  virtual void clear_pgscrub_state() = 0;

  virtual void cleanup_store(ObjectStore::Transaction* t) = 0;

  virtual bool get_store_errors(const scrub_ls_arg_t& arg,
				scrub_ls_result_t& res_inout) const = 0;

  /**
   * force a periodic 'publish_stats_to_osd()' call, to update scrub-related
   * counters and statistics.
   */
  virtual void update_scrub_stats(
    ceph::coarse_real_clock::time_point now_is) = 0;

  /**
   * Recalculate scrub (both deep & shallow) schedules
   *
   * Dequeues the scrub job, and re-queues it with the new schedule.
   */
  virtual void update_scrub_job() = 0;

  virtual scrub_level_t scrub_requested(
      scrub_level_t scrub_level,
      scrub_type_t scrub_type) = 0;

  /**
   * let the scrubber know that a recovery operation has completed.
   * This might trigger an 'after repair' scrub.
   */
  virtual void recovery_completed() = 0;

  /**
   * m_after_repair_scrub_required is set, and recovery_complete() is
   * expected to trigger a deep scrub
   */
  virtual bool is_after_repair_required() const = 0;


  // --------------- reservations -----------------------------------

  /**
   * route incoming replica-reservations requests/responses to the
   * appropriate handler.
   * As the ReplicaReservations object is to be owned by the ScrubMachine, we
   * send all relevant messages to the ScrubMachine.
   */
  virtual void handle_scrub_reserve_msgs(OpRequestRef op) = 0;

  // --------------- debugging via the asok ------------------------------

  virtual int asok_debug(std::string_view cmd,
			 std::string param,
			 Formatter* f,
			 std::stringstream& ss) = 0;
};
