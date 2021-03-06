#ifndef FISH_TOPIC_MONITOR_H
#define FISH_TOPIC_MONITOR_H

#include <array>
#include <atomic>
#include <bitset>
#include <condition_variable>
#include <limits>
#include <numeric>

#include "common.h"
#include "io.h"

/** Topic monitoring support. Topics are conceptually "a thing that can happen." For example,
 delivery of a SIGINT, a child process exits, etc. It is possible to post to a topic, which means
 that that thing happened.

 Associated with each topic is a current generation, which is a 64 bit value. When you query a
 topic, you get back a generation. If on the next query the generation has increased, then it
 indicates someone posted to the topic.

 For example, if you are monitoring a child process, you can query the sigchld topic. If it has
 increased since your last query, it is possible that your child process has exited.

 Topic postings may be coalesced. That is there may be two posts to a given topic, yet the
 generation only increases by 1. The only guarantee is that after a topic post, the current
 generation value is larger than any value previously queried.

 Tying this all together is the topic_monitor_t. This provides the current topic generations, and
 also provides the ability to perform a blocking wait for any topic to change in a particular topic
 set. This is the real power of topics: you can wait for a sigchld signal OR a thread exit.
 */

/// A generation is a counter incremented every time the value of a topic changes.
/// It is 64 bit so it will never wrap.
using generation_t = uint64_t;

/// A generation value which indicates the topic is not of interest.
constexpr generation_t invalid_generation = std::numeric_limits<generation_t>::max();

/// The list of topics which may be observed.
enum class topic_t : uint8_t {
    sighupint,      // Corresponds to both SIGHUP and SIGINT signals.
    sigchld,        // Corresponds to SIGCHLD signal.
    internal_exit,  // Corresponds to an internal process exit.
};

/// Helper to return all topics, allowing easy iteration.
inline std::array<topic_t, 3> all_topics() {
    return {{topic_t::sighupint, topic_t::sigchld, topic_t::internal_exit}};
}

/// Simple value type containing the values for a topic.
/// This should be kept in sync with topic_t.
class generation_list_t {
   public:
    generation_list_t() = default;

    generation_t sighupint{0};
    generation_t sigchld{0};
    generation_t internal_exit{0};

    /// \return the value for a topic.
    generation_t &at(topic_t topic) {
        switch (topic) {
            case topic_t::sigchld:
                return sigchld;
            case topic_t::sighupint:
                return sighupint;
            case topic_t::internal_exit:
                return internal_exit;
        }
        DIE("Unreachable");
    }

    generation_t at(topic_t topic) const {
        switch (topic) {
            case topic_t::sighupint:
                return sighupint;
            case topic_t::sigchld:
                return sigchld;
            case topic_t::internal_exit:
                return internal_exit;
        }
        DIE("Unreachable");
    }

    /// \return ourselves as an array.
    std::array<generation_t, 3> as_array() const { return {{sighupint, sigchld, internal_exit}}; }

    /// Set the value of \p topic to the smaller of our value and the value in \p other.
    void set_min_from(topic_t topic, const generation_list_t &other) {
        if (this->at(topic) > other.at(topic)) {
            this->at(topic) = other.at(topic);
        }
    }

    /// \return whether a topic is valid.
    bool is_valid(topic_t topic) const { return this->at(topic) != invalid_generation; }

    /// \return whether any topic is valid.
    bool any_valid() const {
        bool valid = false;
        for (auto gen : as_array()) {
            if (gen != invalid_generation) valid = true;
        }
        return valid;
    }

    bool operator==(const generation_list_t &rhs) const {
        return sighupint == rhs.sighupint && sigchld == rhs.sigchld &&
               internal_exit == rhs.internal_exit;
    }

    bool operator!=(const generation_list_t &rhs) const { return !(*this == rhs); }

    /// return a string representation for debugging.
    wcstring describe() const;

    /// Generation list containing invalid generations only.
    static generation_list_t invalids() {
        return generation_list_t(invalid_generation, invalid_generation, invalid_generation);
    }

   private:
    generation_list_t(generation_t sighupint, generation_t sigchld, generation_t internal_exit)
        : sighupint(sighupint), sigchld(sigchld), internal_exit(internal_exit) {}
};

/// The topic monitor class. This permits querying the current generation values for topics,
/// optionally blocking until they increase.
class topic_monitor_t {
   private:
    using topic_bitmask_t = uint8_t;

    // Some stuff that needs to be protected by the same lock.
    struct data_t {
        /// The current values.
        generation_list_t current{};

        /// Whether there is a thread currently reading from the notifier pipe.
        bool has_reader{false};
    };
    owning_lock<data_t> data_{};

    /// Condition variable for broadcasting notifications.
    /// This is associated with data_'s mutex.
    std::condition_variable data_notifier_{};

    /// The set of topics which have pending increments.
    /// This is managed via atomics.
    std::atomic<topic_bitmask_t> pending_updates_{};

    /// Self-pipes used to communicate changes.
    /// The writer is a signal handler.
    /// "The reader" refers to a thread that wants to wait for changes. Only one thread can be the
    /// reader at a given time.
    autoclose_pipes_t pipes_;

    /// Apply any pending updates to the data.
    /// This accepts data because it must be locked.
    /// \return the updated generation list.
    generation_list_t updated_gens_in_data(acquired_lock<data_t> &data);

    /// Given a list of input generations, attempt to update them to something newer.
    /// If \p gens is older, then just return those by reference, and directly return false (not
    /// becoming the reader).
    /// If \p gens is current and there is not a reader, then do not update \p gens and return true,
    /// indicating we should become the reader. Now it is our responsibility to read from the pipes
    /// and notify on a change via the condition variable.
    /// If \p gens is current, and there is already a reader, then wait until the reader notifies us
    /// and try again.
    bool try_update_gens_maybe_becoming_reader(generation_list_t *gens);

    /// Wait for some entry in the list of generations to change.
    /// \return the new gens.
    generation_list_t await_gens(const generation_list_t &input_gens);

    /// \return the current generation list, opportunistically applying any pending updates.
    generation_list_t updated_gens();

    /// Helper to convert a topic to a bitmask containing just that topic.
    static topic_bitmask_t topic_to_bit(topic_t t) { return 1 << static_cast<topic_bitmask_t>(t); }

   public:
    topic_monitor_t();
    ~topic_monitor_t();

    /// topic_monitors should not be copied, and there should be no reason to move one.
    void operator=(const topic_monitor_t &) = delete;
    topic_monitor_t(const topic_monitor_t &) = delete;
    void operator=(topic_monitor_t &&) = delete;
    topic_monitor_t(topic_monitor_t &&) = delete;

    /// The principal topic_monitor. This may be fetched from a signal handler.
    static topic_monitor_t &principal();

    /// Post to a topic, potentially from a signal handler.
    void post(topic_t topic);

    /// Access the current generations.
    generation_list_t current_generations() { return updated_gens(); }

    /// Access the generation for a topic.
    generation_t generation_for_topic(topic_t topic) { return current_generations().at(topic); }

    /// For each valid topic in \p gens, check to see if the current topic is larger than
    /// the value in \p gens.
    /// If \p wait is set, then wait if there are no changes; otherwise return immediately.
    /// \return true if some topic changed, false if none did.
    /// On a true return, this updates the generation list \p gens.
    bool check(generation_list_t *gens, bool wait);
};

#endif
