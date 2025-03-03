// vim: expandtab:ts=4:sw=4

#include <iostream>
#include <iomanip>
#include <vector>
#include <memory>
#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <cassert>
#include <atomic>
#include <mutex>

#include <sys/time.h>
#include <signal.h>
#ifdef __APPLE__
/* macOS */
#include <dispatch/dispatch.h>
#else
/* Linux */
#include <semaphore.h>
#include <sys/syscall.h> /* for SYS_gettid */
#endif

#include "vernier.hh"

#include "ruby/ruby.h"
#include "ruby/debug.h"
#include "ruby/thread.h"

#undef assert
#define assert RUBY_ASSERT_ALWAYS

# define PTR2NUM(x)   (rb_int2inum((intptr_t)(void *)(x)))

// Internal TracePoint events we'll monitor during profiling
#define RUBY_INTERNAL_EVENTS \
  RUBY_INTERNAL_EVENT_GC_START | \
  RUBY_INTERNAL_EVENT_GC_END_MARK | \
  RUBY_INTERNAL_EVENT_GC_END_SWEEP | \
  RUBY_INTERNAL_EVENT_GC_ENTER | \
  RUBY_INTERNAL_EVENT_GC_EXIT

#define RUBY_NORMAL_EVENTS \
  RUBY_EVENT_THREAD_BEGIN | \
  RUBY_EVENT_THREAD_END

#define sym(name) ID2SYM(rb_intern_const(name))

// HACK: This isn't public, but the objspace ext uses it
extern "C" size_t rb_obj_memsize_of(VALUE);

using namespace std;

static VALUE rb_mVernier;
static VALUE rb_cVernierResult;
static VALUE rb_mVernierMarkerType;
static VALUE rb_cVernierCollector;

static const char *gvl_event_name(rb_event_flag_t event) {
    switch (event) {
      case RUBY_INTERNAL_THREAD_EVENT_STARTED:
        return "started";
      case RUBY_INTERNAL_THREAD_EVENT_READY:
        return "ready";
      case RUBY_INTERNAL_THREAD_EVENT_RESUMED:
        return "resumed";
      case RUBY_INTERNAL_THREAD_EVENT_SUSPENDED:
        return "suspended";
      case RUBY_INTERNAL_THREAD_EVENT_EXITED:
        return "exited";
    }
    return "no-event";
}

class TimeStamp {
    static const uint64_t nanoseconds_per_second = 1000000000;
    uint64_t value_ns;

    TimeStamp(uint64_t value_ns) : value_ns(value_ns) {}

    public:
    TimeStamp() : value_ns(0) {}

    static TimeStamp Now() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return TimeStamp(ts.tv_sec * nanoseconds_per_second + ts.tv_nsec);
    }

    static TimeStamp Zero() {
        return TimeStamp(0);
    }

    // SleepUntil a specified timestamp
    // Highly accurate manual sleep time
    static void SleepUntil(const TimeStamp &target_time) {
        if (target_time.zero()) return;
        struct timespec ts = target_time.timespec();

        int res;
        do {
            // do nothing until it's time :)
            sleep(0);
        } while (target_time > TimeStamp::Now());
    }

    static TimeStamp from_seconds(uint64_t s) {
        return TimeStamp::from_milliseconds(s * 1000);
    }

    static TimeStamp from_milliseconds(uint64_t ms) {
        return TimeStamp::from_microseconds(ms * 1000);
    }

    static TimeStamp from_microseconds(uint64_t us) {
        return TimeStamp::from_nanoseconds(us * 1000);
    }

    static TimeStamp from_nanoseconds(uint64_t ns) {
        return TimeStamp(ns);
    }

    TimeStamp operator-(const TimeStamp &other) const {
        TimeStamp result = *this;
        return result -= other;
    }

    TimeStamp &operator-=(const TimeStamp &other) {
        if (value_ns > other.value_ns) {
            value_ns = value_ns - other.value_ns;
        } else {
            // underflow
            value_ns = 0;
        }
        return *this;
    }

    TimeStamp operator+(const TimeStamp &other) const {
        TimeStamp result = *this;
        return result += other;
    }

    TimeStamp &operator+=(const TimeStamp &other) {
        uint64_t new_value = value_ns + other.value_ns;
        value_ns = new_value;
        return *this;
    }

    bool operator<(const TimeStamp &other) const {
        return value_ns < other.value_ns;
    }

    bool operator<=(const TimeStamp &other) const {
        return value_ns <= other.value_ns;
    }

    bool operator>(const TimeStamp &other) const {
        return value_ns > other.value_ns;
    }

    bool operator>=(const TimeStamp &other) const {
        return value_ns >= other.value_ns;
    }

    bool operator==(const TimeStamp &other) const {
        return value_ns == other.value_ns;
    }

    bool operator!=(const TimeStamp &other) const {
        return value_ns != other.value_ns;
    }

    uint64_t nanoseconds() const {
        return value_ns;
    }

    uint64_t microseconds() const {
        return value_ns / 1000;
    }

    bool zero() const {
        return value_ns == 0;
    }

    struct timespec timespec() const {
        struct timespec ts;
        ts.tv_sec = nanoseconds() / nanoseconds_per_second;
        ts.tv_nsec = (nanoseconds() % nanoseconds_per_second);
        return ts;
    }
};

std::ostream& operator<<(std::ostream& os, const TimeStamp& info) {
    os << info.nanoseconds() << "ns";
    return os;
}

struct FrameInfo {
    static const char *label_cstr(VALUE frame) {
        VALUE label = rb_profile_frame_full_label(frame);
        return StringValueCStr(label);
    }

    static const char *file_cstr(VALUE frame) {
        VALUE file = rb_profile_frame_absolute_path(frame);
        if (NIL_P(file))
            file = rb_profile_frame_path(frame);
        if (NIL_P(file)) {
            return "";
        } else {
            return StringValueCStr(file);
        }
    }

    static int first_lineno_int(VALUE frame) {
        VALUE first_lineno = rb_profile_frame_first_lineno(frame);
        return NIL_P(first_lineno) ? 0 : FIX2INT(first_lineno);
    }

    FrameInfo(VALUE frame) :
        label(label_cstr(frame)),
        file(file_cstr(frame)),
        first_lineno(first_lineno_int(frame)) { }

    std::string label;
    std::string file;
    int first_lineno;
};

bool operator==(const FrameInfo& lhs, const FrameInfo& rhs) noexcept {
    return
        lhs.label == rhs.label &&
        lhs.file == rhs.file &&
        lhs.first_lineno == rhs.first_lineno;
}

struct Frame {
    VALUE frame;
    int line;

    FrameInfo info() const {
        return FrameInfo(frame);
    }
};

bool operator==(const Frame& lhs, const Frame& rhs) noexcept {
    return lhs.frame == rhs.frame && lhs.line == rhs.line;
}

bool operator!=(const Frame& lhs, const Frame& rhs) noexcept {
    return !(lhs == rhs);
}

namespace std {
    template<>
    struct hash<Frame>
    {
        std::size_t operator()(Frame const& s) const noexcept
        {
            return s.frame ^ s.line;
        }
    };
}

// A basic semaphore built on sem_wait/sem_post
// post() is guaranteed to be async-signal-safe
class SamplerSemaphore {
#ifdef __APPLE__
    dispatch_semaphore_t sem;
#else
    sem_t sem;
#endif

    public:

    SamplerSemaphore(unsigned int value = 0) {
#ifdef __APPLE__
        sem = dispatch_semaphore_create(value);
#else
        sem_init(&sem, 0, value);
#endif
    };

    ~SamplerSemaphore() {
#ifdef __APPLE__
        dispatch_release(sem);
#else
        sem_destroy(&sem);
#endif
    };

    void wait() {
#ifdef __APPLE__
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
#else
        // Use sem_timedwait so that we get a crash instead of a deadlock for
        // easier debugging
        auto ts = (TimeStamp::Now() + TimeStamp::from_seconds(5)).timespec();

        int ret;
        do {
            ret = sem_wait(&sem);
        } while (ret && errno == EINTR);
        assert(ret == 0);
#endif
    }

    void post() {
#ifdef __APPLE__
        dispatch_semaphore_signal(sem);
#else
        sem_post(&sem);
#endif
    }
};

struct RawSample {
    constexpr static int MAX_LEN = 2048;
    VALUE frames[MAX_LEN];
    int lines[MAX_LEN];
    int len;
    bool gc;

    RawSample() : len(0), gc(false) { }

    int size() const {
        return len;
    }

    Frame frame(int i) const {
        int idx = len - i - 1;
        if (idx < 0) throw std::out_of_range("out of range");
        const Frame frame = {frames[idx], lines[idx]};
        return frame;
    }

    void sample() {
        clear();

        if (!ruby_native_thread_p()) {
            return;
        }

        if (rb_during_gc()) {
          gc = true;
        } else {
          len = rb_profile_frames(0, MAX_LEN, frames, lines);
        }
    }

    void clear() {
        len = 0;
        gc = false;
    }

    bool empty() const {
        return len == 0;
    }
};

// Based very loosely on the design of Gecko's SigHandlerCoordinator
// This is used for communication between the profiler thread and the signal
// handlers in the observed thread.
struct LiveSample {
    RawSample sample;

    SamplerSemaphore sem_complete;

    // Wait for a sample to be collected by the signal handler on another thread
    void wait() {
        sem_complete.wait();
    }

    int size() const {
        return sample.size();
    }

    Frame frame(int i) const {
        return sample.frame(i);
    }

    // Called from a signal handler in the observed thread in order to take a
    // sample and signal to the proifiler thread that the sample is ready.
    //
    // CRuby doesn't guarantee that rb_profile_frames can be used as
    // async-signal-safe but in practice it seems to be.
    // sem_post is safe in an async-signal-safe context.
    void sample_current_thread() {
        sample.sample();
        sem_complete.post();
    }
};

struct FrameList {
    std::unordered_map<std::string, int> string_to_idx;
    std::vector<std::string> string_list;

    int string_index(const std::string str) {
        auto it = string_to_idx.find(str);
        if (it == string_to_idx.end()) {
            int idx = string_list.size();
            string_list.push_back(str);

            auto result = string_to_idx.insert({str, idx});
            it = result.first;
        }

        return it->second;
    }

    struct FrameWithInfo {
        Frame frame;
        FrameInfo info;
    };

    std::unordered_map<Frame, int> frame_to_idx;
    std::vector<Frame> frame_list;
    std::vector<FrameWithInfo> frame_with_info_list;
    int frame_index(const Frame frame) {
        auto it = frame_to_idx.find(frame);
        if (it == frame_to_idx.end()) {
            int idx = frame_list.size();
            frame_list.push_back(frame);
            auto result = frame_to_idx.insert({frame, idx});
            it = result.first;
        }
        return it->second;
    }

    struct StackNode {
        std::unordered_map<Frame, int> children;
        Frame frame;
        int parent;
        int index;

        StackNode(Frame frame, int index, int parent) : frame(frame), index(index), parent(parent) {}

        // root
        StackNode() : frame(Frame{0, 0}), index(-1), parent(-1) {}
    };

    StackNode root_stack_node;
    vector<StackNode> stack_node_list;

    int stack_index(const RawSample &stack) {
        if (stack.empty()) {
            throw std::runtime_error("empty stack");
        }

        StackNode *node = &root_stack_node;
        for (int i = 0; i < stack.size(); i++) {
            Frame frame = stack.frame(i);
            node = next_stack_node(node, frame);
        }
        return node->index;
    }

    StackNode *next_stack_node(StackNode *node, Frame frame) {
        auto search = node->children.find(frame);
        if (search == node->children.end()) {
            // insert a new node
            int next_node_idx = stack_node_list.size();
            node->children[frame] = next_node_idx;
            stack_node_list.emplace_back(
                    frame,
                    next_node_idx,
                    node->index
                    );
            return &stack_node_list[next_node_idx];
        } else {
            int node_idx = search->second;
            return &stack_node_list[node_idx];
        }
    }

    // Converts Frames from stacks other tables. "Symbolicates" the frames
    // which allocates.
    void finalize() {
        for (const auto &stack_node : stack_node_list) {
            frame_index(stack_node.frame);
        }
        for (const auto &frame : frame_list) {
            frame_with_info_list.push_back(FrameWithInfo{frame, frame.info()});
        }
    }

    void mark_frames() {
        for (auto stack_node: stack_node_list) {
            rb_gc_mark(stack_node.frame.frame);
        }
    }

    void clear() {
        string_list.clear();
        frame_list.clear();
        stack_node_list.clear();
        frame_with_info_list.clear();

        string_to_idx.clear();
        frame_to_idx.clear();
        root_stack_node.children.clear();
    }

    void write_result(VALUE result) {
        FrameList &frame_list = *this;

        VALUE stack_table = rb_hash_new();
        rb_ivar_set(result, rb_intern("@stack_table"), stack_table);
        VALUE stack_table_parent = rb_ary_new();
        VALUE stack_table_frame = rb_ary_new();
        rb_hash_aset(stack_table, sym("parent"), stack_table_parent);
        rb_hash_aset(stack_table, sym("frame"), stack_table_frame);
        for (const auto &stack : frame_list.stack_node_list) {
            VALUE parent_val = stack.parent == -1 ? Qnil : INT2NUM(stack.parent);
            rb_ary_push(stack_table_parent, parent_val);
            rb_ary_push(stack_table_frame, INT2NUM(frame_list.frame_index(stack.frame)));
        }

        VALUE frame_table = rb_hash_new();
        rb_ivar_set(result, rb_intern("@frame_table"), frame_table);
        VALUE frame_table_func = rb_ary_new();
        VALUE frame_table_line = rb_ary_new();
        rb_hash_aset(frame_table, sym("func"), frame_table_func);
        rb_hash_aset(frame_table, sym("line"), frame_table_line);
        //for (const auto &frame : frame_list.frame_list) {
        for (int i = 0; i < frame_list.frame_with_info_list.size(); i++) {
            const auto &frame = frame_list.frame_with_info_list[i];
            rb_ary_push(frame_table_func, INT2NUM(i));
            rb_ary_push(frame_table_line, INT2NUM(frame.frame.line));
        }

        // TODO: dedup funcs before this step
        VALUE func_table = rb_hash_new();
        rb_ivar_set(result, rb_intern("@func_table"), func_table);
        VALUE func_table_name = rb_ary_new();
        VALUE func_table_filename = rb_ary_new();
        VALUE func_table_first_line = rb_ary_new();
        rb_hash_aset(func_table, sym("name"), func_table_name);
        rb_hash_aset(func_table, sym("filename"), func_table_filename);
        rb_hash_aset(func_table, sym("first_line"), func_table_first_line);
        for (const auto &frame : frame_list.frame_with_info_list) {
            const std::string label = frame.info.label;
            const std::string filename = frame.info.file;
            const int first_line = frame.info.first_lineno;

            rb_ary_push(func_table_name, rb_str_new(label.c_str(), label.length()));
            rb_ary_push(func_table_filename, rb_str_new(filename.c_str(), filename.length()));
            rb_ary_push(func_table_first_line, INT2NUM(first_line));
        }
    }
};

class SampleTranslator {
    public:
        int last_stack_index;

        Frame frames[RawSample::MAX_LEN];
        int frame_indexes[RawSample::MAX_LEN];
        int len;

        SampleTranslator() : len(0), last_stack_index(-1) {
        }

        int translate(FrameList &frame_list, const RawSample &sample) {
            int i = 0;
            for (; i < len && i < sample.size(); i++) {
                if (frames[i] != sample.frame(i)) {
                    break;
                }
            }

            FrameList::StackNode *node = i == 0 ? &frame_list.root_stack_node : &frame_list.stack_node_list[frame_indexes[i - 1]];

            for (; i < sample.size(); i++) {
                Frame frame = sample.frame(i);
                node = frame_list.next_stack_node(node, frame);

                frames[i] = frame;
                frame_indexes[i] = node->index;
            }
            len = i;

            last_stack_index = node->index;
            return last_stack_index;
        }
};

typedef uint64_t native_thread_id_t;
static native_thread_id_t get_native_thread_id() {
#ifdef __APPLE__
    uint64_t thread_id;
    int e = pthread_threadid_np(pthread_self(), &thread_id);
    if (e != 0) rb_syserr_fail(e, "pthread_threadid_np");
    return thread_id;
#else
    // gettid() is only available as of glibc 2.30
    pid_t tid = syscall(SYS_gettid);
    return tid;
#endif
}


class Marker {
    public:
    enum Type {
        MARKER_GVL_THREAD_STARTED,
        MARKER_GVL_THREAD_EXITED,

        MARKER_GC_START,
        MARKER_GC_END_MARK,
        MARKER_GC_END_SWEEP,
        MARKER_GC_ENTER,
        MARKER_GC_EXIT,
        MARKER_GC_PAUSE,

        MARKER_THREAD_RUNNING,
        MARKER_THREAD_STALLED,
        MARKER_THREAD_SUSPENDED,

        MARKER_MAX,
    };

    // Must match phase types from Gecko
    enum Phase {
      INSTANT,
      INTERVAL,
      INTERVAL_START,
      INTERVAL_END
    };

    Type type;
    Phase phase;
    TimeStamp timestamp;
    TimeStamp finish;
    // VALUE ruby_thread_id;
    //native_thread_id_t thread_id;
    int stack_index = -1;

    VALUE to_array() {
        VALUE record[6] = {0};
        record[0] = Qnil; // FIXME
        record[1] = INT2NUM(type);
        record[2] = INT2NUM(phase);
        record[3] = ULL2NUM(timestamp.nanoseconds());

        if (phase == Marker::Phase::INTERVAL) {
            record[4] = ULL2NUM(finish.nanoseconds());
        }
        else {
            record[4] = Qnil;
        }
        record[5] = stack_index == -1 ? Qnil : INT2NUM(stack_index);

        return rb_ary_new_from_values(6, record);
    }
};

class MarkerTable {
    public:
        std::vector<Marker> list;
        std::mutex mutex;

        void record_interval(Marker::Type type, TimeStamp from, TimeStamp to, int stack_index = -1) {
            const std::lock_guard<std::mutex> lock(mutex);

            list.push_back({ type, Marker::INTERVAL, from, to, stack_index });
        }

        void record(Marker::Type type, int stack_index = -1) {
            const std::lock_guard<std::mutex> lock(mutex);

            list.push_back({ type, Marker::INSTANT, TimeStamp::Now(), TimeStamp(), stack_index });
        }
};

class GCMarkerTable: public MarkerTable {
    TimeStamp last_gc_entry;

    public:
        void record_gc_entered() {
          last_gc_entry = TimeStamp::Now();
        }

        void record_gc_leave() {
          list.push_back({ Marker::MARKER_GC_PAUSE, Marker::INTERVAL, last_gc_entry, TimeStamp::Now(), -1 });
        }
};

enum Category{
	CATEGORY_NORMAL,
	CATEGORY_IDLE
};

class SampleList {
    public:

        std::vector<int> stacks;
        std::vector<TimeStamp> timestamps;
        std::vector<native_thread_id_t> threads;
        std::vector<Category> categories;
        std::vector<int> weights;

        size_t size() {
            return stacks.size();
        }

        bool empty() {
            return size() == 0;
        }

        void record_sample(int stack_index, TimeStamp time, native_thread_id_t thread_id, Category category) {
            if (
                    !empty() &&
                    stacks.back() == stack_index &&
                    threads.back() == thread_id &&
                    categories.back() == category)
            {
                // We don't compare timestamps for de-duplication
                weights.back() += 1;
            } else {
                stacks.push_back(stack_index);
                timestamps.push_back(time);
                threads.push_back(thread_id);
                categories.push_back(category);
                weights.push_back(1);
            }
        }

        void write_result(VALUE result) const {
            VALUE samples = rb_ary_new();
            rb_hash_aset(result, sym("samples"), samples);
            for (auto& stack_index: this->stacks) {
                rb_ary_push(samples, INT2NUM(stack_index));
            }

            VALUE weights = rb_ary_new();
            rb_hash_aset(result, sym("weights"), weights);
            for (auto& weight: this->weights) {
                rb_ary_push(weights, INT2NUM(weight));
            }

            VALUE timestamps = rb_ary_new();
            rb_hash_aset(result, sym("timestamps"), timestamps);
            for (auto& timestamp: this->timestamps) {
                rb_ary_push(timestamps, ULL2NUM(timestamp.nanoseconds()));
            }

            VALUE sample_categories = rb_ary_new();
            rb_hash_aset(result, sym("sample_categories"), sample_categories);
            for (auto& cat: this->categories) {
                rb_ary_push(sample_categories, INT2NUM(cat));
            }
        }
};

class Thread {
    public:
        SampleList samples;

        enum State {
            STARTED,
            RUNNING,
            READY,
            SUSPENDED,
            STOPPED
        };

        VALUE ruby_thread;
        VALUE ruby_thread_id;
        pthread_t pthread_id;
        native_thread_id_t native_tid;
        State state;

        TimeStamp state_changed_at;
        TimeStamp started_at;
        TimeStamp stopped_at;

        int stack_on_suspend_idx;
        SampleTranslator translator;

        MarkerTable *markers;

	std::string name;

	// FIXME: don't use pthread at start
        Thread(State state, pthread_t pthread_id, VALUE ruby_thread) : pthread_id(pthread_id), ruby_thread(ruby_thread), state(state), stack_on_suspend_idx(-1) {
            name = Qnil;
            ruby_thread_id = rb_obj_id(ruby_thread);
	    //ruby_thread_id = ULL2NUM(ruby_thread);
            native_tid = get_native_thread_id();
            started_at = state_changed_at = TimeStamp::Now();
            name = "";
            markers = new MarkerTable();

            if (state == State::STARTED) {
                markers->record(Marker::Type::MARKER_GVL_THREAD_STARTED);
            }
        }

        void set_state(State new_state) {
            if (state == Thread::State::STOPPED) {
                return;
            }
            if (new_state == Thread::State::SUSPENDED && state == new_state) {
                // on Ruby 3.2 (only?) we may see duplicate suspended states
                return;
            }

            TimeStamp from = state_changed_at;
            auto now = TimeStamp::Now();

            if (started_at.zero()) {
                started_at = now;
            }

            switch (new_state) {
                case State::STARTED:
                    markers->record(Marker::Type::MARKER_GVL_THREAD_STARTED);
                    return; // no mutation of current state
                    break;
                case State::RUNNING:
                    assert(state == State::READY || state == State::RUNNING);
                    pthread_id = pthread_self();
                    native_tid = get_native_thread_id();

                    // If the GVL is immediately ready, and we measure no times
                    // stalled, skip emitting the interval.
                    if (from != now) {
                        markers->record_interval(Marker::Type::MARKER_THREAD_STALLED, from, now);
                    }
                    break;
                case State::READY:
                    // The ready state means "I would like to do some work, but I can't
                    // do it right now either because I blocked on IO and now I want the GVL back,
                    // or because the VM timer put me to sleep"
                    //
                    // Threads can be preempted, which means they will have been in "Running"
                    // state, and then the VM was like "no I need to stop you from working,
                    // so I'll put you in the 'ready' (or stalled) state"
                    assert(state == State::STARTED || state == State::SUSPENDED || state == State::RUNNING);
                    if (state == State::SUSPENDED) {
                        markers->record_interval(Marker::Type::MARKER_THREAD_SUSPENDED, from, now, stack_on_suspend_idx);
                    }
                    else if (state == State::RUNNING) {
                        markers->record_interval(Marker::Type::MARKER_THREAD_RUNNING, from, now);
                    }
                    break;
                case State::SUSPENDED:
                    // We can go from RUNNING or STARTED to SUSPENDED
                    assert(state == State::RUNNING || state == State::STARTED || state == State::SUSPENDED);
                    markers->record_interval(Marker::Type::MARKER_THREAD_RUNNING, from, now);
                    break;
                case State::STOPPED:
                    // We can go from RUNNING or STARTED or SUSPENDED to STOPPED
                    assert(state == State::RUNNING || state == State::STARTED || state == State::SUSPENDED);
                    markers->record_interval(Marker::Type::MARKER_THREAD_RUNNING, from, now);
                    markers->record(Marker::Type::MARKER_GVL_THREAD_EXITED);

                    stopped_at = now;
                    capture_name();

                    break;
            }

            state = new_state;
            state_changed_at = now;
        }

        bool running() {
            return state != State::STOPPED;
        }

        void capture_name() {
            //char buf[128];
            //int rc = pthread_getname_np(pthread_id, buf, sizeof(buf));
            //if (rc == 0)
            //    name = std::string(buf);
        }

        void mark() {
        }
};

class ThreadTable {
    public:
        FrameList &frame_list;

        std::vector<Thread> list;
        std::mutex mutex;

        ThreadTable(FrameList &frame_list) : frame_list(frame_list) {
        }

        void mark() {
            for (auto &thread : list) {
                thread.mark();
            }
        }

        void started(VALUE th) {
            //list.push_back(Thread{pthread_self(), Thread::State::SUSPENDED});
            set_state(Thread::State::STARTED, th);
        }

        void ready(VALUE th) {
            set_state(Thread::State::READY, th);
        }

        void resumed(VALUE th) {
            set_state(Thread::State::RUNNING, th);
        }

        void suspended(VALUE th) {
            set_state(Thread::State::SUSPENDED, th);
        }

        void stopped(VALUE th) {
            set_state(Thread::State::STOPPED, th);
        }

    private:
        void set_state(Thread::State new_state, VALUE th) {
            const std::lock_guard<std::mutex> lock(mutex);

            //cerr << "set state=" << new_state << " thread=" << gettid() << endl;

            pid_t native_tid = get_native_thread_id();
            pthread_t pthread_id = pthread_self();

            //fprintf(stderr, "th %p (tid: %i) from %s to %s\n", (void *)th, native_tid, gvl_event_name(state), gvl_event_name(new_state));

            for (auto &thread : list) {
                if (thread_equal(th, thread.ruby_thread)) {
                    if (new_state == Thread::State::SUSPENDED) {

                        RawSample sample;
                        sample.sample();

                        thread.stack_on_suspend_idx = thread.translator.translate(frame_list, sample);
                        //cerr << gettid() << " suspended! Stack size:" << thread.stack_on_suspend.size() << endl;
                    }

                    thread.set_state(new_state);

                    if (thread.state == Thread::State::RUNNING) {
                        thread.pthread_id = pthread_self();
                        thread.native_tid = get_native_thread_id();
                    } else {
                        thread.pthread_id = 0;
                        thread.native_tid = 0;
                    }


                    return;
                }
            }

            //fprintf(stderr, "NEW THREAD: th: %p, state: %i\n", th, new_state);
            list.emplace_back(new_state, pthread_self(), th);
        }

        bool thread_equal(VALUE a, VALUE b) {
            return a == b;
        }
};

class BaseCollector {
    protected:

    virtual void reset() {
        frame_list.clear();
    }

    public:
    bool running = false;
    FrameList frame_list;

    TimeStamp started_at;

    virtual ~BaseCollector() {}

    virtual bool start() {
        if (running) {
            return false;
        }

        started_at = TimeStamp::Now();

        running = true;
        return true;
    }

    virtual VALUE stop() {
        if (!running) {
            rb_raise(rb_eRuntimeError, "collector not running");
        }
        running = false;

        return Qnil;
    }

    void write_meta(VALUE result) {
        VALUE meta = rb_hash_new();
        rb_ivar_set(result, rb_intern("@meta"), meta);
        rb_hash_aset(meta, sym("started_at"), ULL2NUM(started_at.nanoseconds()));

    }

    virtual VALUE build_collector_result() {
        VALUE result = rb_obj_alloc(rb_cVernierResult);

        write_meta(result);

        return result;
    }

    virtual void sample() {
        rb_raise(rb_eRuntimeError, "collector doesn't support manual sampling");
    };

    virtual void mark() {
        frame_list.mark_frames();
    };

    virtual VALUE get_markers() {
        return rb_ary_new();
    };
};

class CustomCollector : public BaseCollector {
    SampleList samples;

    void sample() {
        RawSample sample;
        sample.sample();
        int stack_index = frame_list.stack_index(sample);

	native_thread_id_t thread_id = 0;
        samples.record_sample(stack_index, TimeStamp::Now(), thread_id, CATEGORY_NORMAL);
    }

    VALUE stop() {
        BaseCollector::stop();

        frame_list.finalize();

        VALUE result = build_collector_result();

        reset();

        return result;
    }

    VALUE build_collector_result() {
        VALUE result = BaseCollector::build_collector_result();

        VALUE threads = rb_hash_new();
        rb_ivar_set(result, rb_intern("@threads"), threads);

	VALUE thread_hash = rb_hash_new();
	samples.write_result(thread_hash);

	rb_hash_aset(threads, ULL2NUM(0), thread_hash);
	rb_hash_aset(thread_hash, sym("tid"), ULL2NUM(0));

        frame_list.write_result(result);

        return result;
    }
};

class RetainedCollector : public BaseCollector {
    void reset() {
        object_frames.clear();
        object_list.clear();

        BaseCollector::reset();
    }

    void record(VALUE obj) {
        RawSample sample;
        sample.sample();
        int stack_index = frame_list.stack_index(sample);

        object_list.push_back(obj);
        object_frames.emplace(obj, stack_index);
    }

    std::unordered_map<VALUE, int> object_frames;
    std::vector<VALUE> object_list;

    VALUE tp_newobj = Qnil;
    VALUE tp_freeobj = Qnil;

    static void newobj_i(VALUE tpval, void *data) {
        RetainedCollector *collector = static_cast<RetainedCollector *>(data);
        rb_trace_arg_t *tparg = rb_tracearg_from_tracepoint(tpval);
        VALUE obj = rb_tracearg_object(tparg);

        collector->record(obj);
    }

    static void freeobj_i(VALUE tpval, void *data) {
        RetainedCollector *collector = static_cast<RetainedCollector *>(data);
        rb_trace_arg_t *tparg = rb_tracearg_from_tracepoint(tpval);
        VALUE obj = rb_tracearg_object(tparg);

        collector->object_frames.erase(obj);
    }

    public:

    bool start() {
        if (!BaseCollector::start()) {
            return false;
        }

        tp_newobj = rb_tracepoint_new(0, RUBY_INTERNAL_EVENT_NEWOBJ, newobj_i, this);
        tp_freeobj = rb_tracepoint_new(0, RUBY_INTERNAL_EVENT_FREEOBJ, freeobj_i, this);

        rb_tracepoint_enable(tp_newobj);
        rb_tracepoint_enable(tp_freeobj);

        return true;
    }

    VALUE stop() {
        BaseCollector::stop();

        // GC before we start turning stacks into strings
        rb_gc();

        // Stop tracking any more new objects, but we'll continue tracking free'd
        // objects as we may be able to free some as we remove our own references
        // to stack frames.
        rb_tracepoint_disable(tp_newobj);
        tp_newobj = Qnil;

        frame_list.finalize();

        // We should have collected info for all our frames, so no need to continue
        // marking them
        // FIXME: previously here we cleared the list of frames so we would stop
        // marking them. Maybe now we should set a flag so that we stop marking them

        // GC again
        rb_gc();

        rb_tracepoint_disable(tp_freeobj);
        tp_freeobj = Qnil;

        VALUE result = build_collector_result();

        reset();

        return result;
    }

    VALUE build_collector_result() {
        RetainedCollector *collector = this;
        FrameList &frame_list = collector->frame_list;

        VALUE result = BaseCollector::build_collector_result();

        VALUE threads = rb_hash_new();
        rb_ivar_set(result, rb_intern("@threads"), threads);
        VALUE thread_hash = rb_hash_new();
        rb_hash_aset(threads, ULL2NUM(0), thread_hash);

        rb_hash_aset(thread_hash, sym("tid"), ULL2NUM(0));
        VALUE samples = rb_ary_new();
        rb_hash_aset(thread_hash, sym("samples"), samples);
        VALUE weights = rb_ary_new();
        rb_hash_aset(thread_hash, sym("weights"), weights);

        rb_hash_aset(thread_hash, sym("name"), rb_str_new_cstr("retained memory"));
        rb_hash_aset(thread_hash, sym("started_at"), ULL2NUM(collector->started_at.nanoseconds()));

        for (auto& obj: collector->object_list) {
            const auto search = collector->object_frames.find(obj);
            if (search != collector->object_frames.end()) {
                int stack_index = search->second;

                rb_ary_push(samples, INT2NUM(stack_index));
                rb_ary_push(weights, INT2NUM(rb_obj_memsize_of(obj)));
            }
        }

        frame_list.write_result(result);

        return result;
    }

    void mark() {
        // We don't mark the objects, but we MUST mark the frames, otherwise they
        // can be garbage collected.
        // When we stop collection we will stringify the remaining frames, and then
        // clear them from the set, allowing them to be removed from out output.
        frame_list.mark_frames();

        rb_gc_mark(tp_newobj);
        rb_gc_mark(tp_freeobj);
    }
};

class GlobalSignalHandler {
    static LiveSample *live_sample;

    public:
        static GlobalSignalHandler *get_instance() {
            static GlobalSignalHandler instance;
            return &instance;
        }

        void install() {
            const std::lock_guard<std::mutex> lock(mutex);
            count++;

            if (count == 1) setup_signal_handler();
        }

        void uninstall() {
            const std::lock_guard<std::mutex> lock(mutex);
            count--;

            if (count == 0) clear_signal_handler();
        }

        void record_sample(LiveSample &sample, pthread_t pthread_id) {
            const std::lock_guard<std::mutex> lock(mutex);

            assert(pthread_id);

            live_sample = &sample;
            if (pthread_kill(pthread_id, SIGPROF)) {
                rb_bug("pthread_kill failed");
            }
            sample.wait();
            live_sample = NULL;
        }

    private:
        std::mutex mutex;
        int count;

        static void signal_handler(int sig, siginfo_t* sinfo, void* ucontext) {
            assert(live_sample);
            live_sample->sample_current_thread();
        }

        void setup_signal_handler() {
            struct sigaction sa;
            sa.sa_sigaction = signal_handler;
            sa.sa_flags = SA_RESTART | SA_SIGINFO;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGPROF, &sa, NULL);
        }

        void clear_signal_handler() {
            struct sigaction sa;
            sa.sa_handler = SIG_IGN;
            sa.sa_flags = SA_RESTART;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGPROF, &sa, NULL);
        }
};
LiveSample *GlobalSignalHandler::live_sample;

class TimeCollector : public BaseCollector {
    GCMarkerTable gc_markers;
    ThreadTable threads;

    pthread_t sample_thread;

    atomic_bool running;
    SamplerSemaphore thread_stopped;

    TimeStamp interval;

    public:
    TimeCollector(TimeStamp interval) : interval(interval), threads(frame_list) {
    }

    private:

    void record_sample(const RawSample &sample, TimeStamp time, Thread &thread, Category category) {
        if (!sample.empty()) {
            int stack_index = thread.translator.translate(frame_list, sample);
            thread.samples.record_sample(
                    stack_index,
                    time,
                    thread.native_tid,
                    category
                    );
        }
    }

    VALUE get_markers() {
        VALUE list = rb_ary_new();
        VALUE main_thread = rb_thread_main();
        VALUE main_thread_id = rb_obj_id(main_thread);

        for (auto& marker: this->gc_markers.list) {
            VALUE ary = marker.to_array();

            RARRAY_ASET(ary, 0, main_thread_id);
            rb_ary_push(list, ary);
        }
        for (auto &thread : threads.list) {
            for (auto& marker: thread.markers->list) {
                VALUE ary = marker.to_array();
                RARRAY_ASET(ary, 0, thread.ruby_thread_id);
                rb_ary_push(list, ary);
            }
        }

        return list;
    }

    void sample_thread_run() {
        LiveSample sample;

        TimeStamp next_sample_schedule = TimeStamp::Now();
        while (running) {
            TimeStamp sample_start = TimeStamp::Now();

            threads.mutex.lock();
            for (auto &thread : threads.list) {
                //if (thread.state == Thread::State::RUNNING) {
                //if (thread.state == Thread::State::RUNNING || (thread.state == Thread::State::SUSPENDED && thread.stack_on_suspend_idx < 0)) {
                if (thread.state == Thread::State::RUNNING) {
                    //fprintf(stderr, "sampling %p on tid:%i\n", thread.ruby_thread, thread.native_tid);
                    GlobalSignalHandler::get_instance()->record_sample(sample, thread.pthread_id);

                    if (sample.sample.gc) {
                        // fprintf(stderr, "skipping GC sample\n");
                    } else {
                        record_sample(sample.sample, sample_start, thread, CATEGORY_NORMAL);
                    }
                } else if (thread.state == Thread::State::SUSPENDED) {
                    thread.samples.record_sample(
                            thread.stack_on_suspend_idx,
                            sample_start,
                            thread.native_tid,
                            CATEGORY_IDLE);
                } else {
                }
            }

            threads.mutex.unlock();

            TimeStamp sample_complete = TimeStamp::Now();

            next_sample_schedule += interval;

            // If sampling falls behind, restart, and check in another interval
            if (next_sample_schedule < sample_complete) {
                next_sample_schedule = sample_complete + interval;
            }

            TimeStamp::SleepUntil(next_sample_schedule);
        }

        thread_stopped.post();
    }

    static void *sample_thread_entry(void *arg) {
#if HAVE_PTHREAD_SETNAME_NP
#ifdef __APPLE__
        pthread_setname_np("Vernier profiler");
#else
        pthread_setname_np(pthread_self(), "Vernier profiler");
#endif
#endif
        TimeCollector *collector = static_cast<TimeCollector *>(arg);
        collector->sample_thread_run();
        return NULL;
    }

    static void internal_thread_event_cb(rb_event_flag_t event, VALUE data, VALUE self, ID mid, VALUE klass) {
        TimeCollector *collector = static_cast<TimeCollector *>((void *)NUM2ULL(data));

        switch (event) {
            case RUBY_EVENT_THREAD_BEGIN:
                collector->threads.started(self);
                break;
            case RUBY_EVENT_THREAD_END:
                collector->threads.stopped(self);
                break;
        }
    }

    static void internal_gc_event_cb(rb_event_flag_t event, VALUE data, VALUE self, ID mid, VALUE klass) {
        TimeCollector *collector = static_cast<TimeCollector *>((void *)NUM2ULL(data));

        switch (event) {
            case RUBY_INTERNAL_EVENT_GC_START:
                collector->gc_markers.record(Marker::Type::MARKER_GC_START);
                break;
            case RUBY_INTERNAL_EVENT_GC_END_MARK:
                collector->gc_markers.record(Marker::Type::MARKER_GC_END_MARK);
                break;
            case RUBY_INTERNAL_EVENT_GC_END_SWEEP:
                collector->gc_markers.record(Marker::Type::MARKER_GC_END_SWEEP);
                break;
            case RUBY_INTERNAL_EVENT_GC_ENTER:
                collector->gc_markers.record_gc_entered();
                break;
            case RUBY_INTERNAL_EVENT_GC_EXIT:
                collector->gc_markers.record_gc_leave();
                break;
        }
    }

    static void internal_thread_event_cb(rb_event_flag_t event, const rb_internal_thread_event_data_t *event_data, void *data) {
        TimeCollector *collector = static_cast<TimeCollector *>(data);
        VALUE thread = Qnil;

#if HAVE_RB_INTERNAL_THREAD_EVENT_DATA_T_THREAD
        thread = event_data->thread;
#else
        // We may arrive here when starting a thread with
        // RUBY_INTERNAL_THREAD_EVENT_READY before the thread is actually set up.
        if (!ruby_native_thread_p()) return;

        thread = rb_thread_current();
#endif

        auto native_tid = get_native_thread_id();
        //cerr << "internal thread event" << event << " at " << TimeStamp::Now() << endl;
        //fprintf(stderr, "(%i) th %p to %s\n", native_tid, (void *)thread, gvl_event_name(event));


        switch (event) {
            case RUBY_INTERNAL_THREAD_EVENT_STARTED:
                collector->threads.started(thread);
                break;
            case RUBY_INTERNAL_THREAD_EVENT_EXITED:
                collector->threads.stopped(thread);
                break;
            case RUBY_INTERNAL_THREAD_EVENT_READY:
                collector->threads.ready(thread);
                break;
            case RUBY_INTERNAL_THREAD_EVENT_RESUMED:
                collector->threads.resumed(thread);
                break;
            case RUBY_INTERNAL_THREAD_EVENT_SUSPENDED:
                collector->threads.suspended(thread);
                break;

        }
    }

    rb_internal_thread_event_hook_t *thread_hook;

    bool start() {
        if (!BaseCollector::start()) {
            return false;
        }

        GlobalSignalHandler::get_instance()->install();

        running = true;

        int ret = pthread_create(&sample_thread, NULL, &sample_thread_entry, this);
        if (ret != 0) {
            perror("pthread_create");
            rb_bug("pthread_create");
        }

        // Set the state of the current Ruby thread to RUNNING, which we know it
        // is as it must have held the GVL to start the collector. We want to
        // have at least one thread in our thread list because it's possible
        // that the profile might be such that we don't get any thread switch
        // events and we need at least one
        this->threads.resumed(rb_thread_current());

        thread_hook = rb_internal_thread_add_event_hook(internal_thread_event_cb, RUBY_INTERNAL_THREAD_EVENT_MASK, this);
        rb_add_event_hook(internal_gc_event_cb, RUBY_INTERNAL_EVENTS, PTR2NUM((void *)this));
        rb_add_event_hook(internal_thread_event_cb, RUBY_NORMAL_EVENTS, PTR2NUM((void *)this));

        return true;
    }

    VALUE stop() {
        BaseCollector::stop();

        running = false;
        thread_stopped.wait();

        GlobalSignalHandler::get_instance()->uninstall();

        rb_internal_thread_remove_event_hook(thread_hook);
        rb_remove_event_hook(internal_gc_event_cb);
        rb_remove_event_hook(internal_thread_event_cb);

        // capture thread names
        for (auto& thread: this->threads.list) {
            if (thread.running()) {
                thread.capture_name();
            }
        }

        frame_list.finalize();

        VALUE result = build_collector_result();

        reset();

        return result;
    }

    VALUE build_collector_result() {
        VALUE result = BaseCollector::build_collector_result();

        VALUE threads = rb_hash_new();
        rb_ivar_set(result, rb_intern("@threads"), threads);

        for (const auto& thread: this->threads.list) {
            VALUE hash = rb_hash_new();
            thread.samples.write_result(hash);

            rb_hash_aset(threads, thread.ruby_thread_id, hash);
            rb_hash_aset(hash, sym("tid"), ULL2NUM(thread.native_tid));
            rb_hash_aset(hash, sym("started_at"), ULL2NUM(thread.started_at.nanoseconds()));
            if (!thread.stopped_at.zero()) {
                rb_hash_aset(hash, sym("stopped_at"), ULL2NUM(thread.stopped_at.nanoseconds()));
            }
            rb_hash_aset(hash, sym("name"), rb_str_new(thread.name.data(), thread.name.length()));

        }

        frame_list.write_result(result);

        return result;
    }

    void mark() {
        frame_list.mark_frames();
        threads.mark();

        //for (int i = 0; i < queued_length; i++) {
        //    rb_gc_mark(queued_frames[i]);
        //}

        // FIXME: How can we best mark buffered or pending frames?
    }
};

static void
collector_mark(void *data) {
    BaseCollector *collector = static_cast<BaseCollector *>(data);
    collector->mark();
}

static void
collector_free(void *data) {
    BaseCollector *collector = static_cast<BaseCollector *>(data);
    delete collector;
}

static const rb_data_type_t rb_collector_type = {
    .wrap_struct_name = "vernier/collector",
    .function = {
        //.dmemsize = rb_collector_memsize,
        .dmark = collector_mark,
        .dfree = collector_free,
    },
};

static BaseCollector *get_collector(VALUE obj) {
    BaseCollector *collector;
    TypedData_Get_Struct(obj, BaseCollector, &rb_collector_type, collector);
    return collector;
}

static VALUE
collector_start(VALUE self) {
    auto *collector = get_collector(self);

    if (!collector->start()) {
        rb_raise(rb_eRuntimeError, "already running");
    }

    return Qtrue;
}

static VALUE
collector_stop(VALUE self) {
    auto *collector = get_collector(self);

    VALUE result = collector->stop();
    return result;
}

static VALUE
markers(VALUE self) {
    auto *collector = get_collector(self);

    return collector->get_markers();
}

static VALUE
collector_sample(VALUE self) {
    auto *collector = get_collector(self);

    collector->sample();
    return Qtrue;
}

static VALUE collector_new(VALUE self, VALUE mode, VALUE options) {
    BaseCollector *collector;
    if (mode == sym("retained")) {
        collector = new RetainedCollector();
    } else if (mode == sym("custom")) {
        collector = new CustomCollector();
    } else if (mode == sym("wall")) {
        VALUE intervalv = rb_hash_aref(options, sym("interval"));
        TimeStamp interval;
        if (NIL_P(intervalv)) {
            interval = TimeStamp::from_microseconds(500);
        } else {
            interval = TimeStamp::from_microseconds(NUM2UINT(intervalv));
        }
        collector = new TimeCollector(interval);
    } else {
        rb_raise(rb_eArgError, "invalid mode");
    }
    VALUE obj = TypedData_Wrap_Struct(self, &rb_collector_type, collector);
    rb_funcall(obj, rb_intern("initialize"), 1, mode);
    return obj;
}

static void
Init_consts(VALUE rb_mVernierMarkerPhase) {
#define MARKER_CONST(name) \
    rb_define_const(rb_mVernierMarkerType, #name, INT2NUM(Marker::Type::MARKER_##name))

    MARKER_CONST(GVL_THREAD_STARTED);
    MARKER_CONST(GVL_THREAD_EXITED);

    MARKER_CONST(GC_START);
    MARKER_CONST(GC_END_MARK);
    MARKER_CONST(GC_END_SWEEP);
    MARKER_CONST(GC_ENTER);
    MARKER_CONST(GC_EXIT);
    MARKER_CONST(GC_PAUSE);

    MARKER_CONST(THREAD_RUNNING);
    MARKER_CONST(THREAD_STALLED);
    MARKER_CONST(THREAD_SUSPENDED);

#undef MARKER_CONST

#define PHASE_CONST(name) \
    rb_define_const(rb_mVernierMarkerPhase, #name, INT2NUM(Marker::Phase::name))

    PHASE_CONST(INSTANT);
    PHASE_CONST(INTERVAL);
    PHASE_CONST(INTERVAL_START);
    PHASE_CONST(INTERVAL_END);
#undef PHASE_CONST
}

extern "C" void
Init_vernier(void)
{
  rb_mVernier = rb_define_module("Vernier");
  rb_cVernierResult = rb_define_class_under(rb_mVernier, "Result", rb_cObject);
  VALUE rb_mVernierMarker = rb_define_module_under(rb_mVernier, "Marker");
  VALUE rb_mVernierMarkerPhase = rb_define_module_under(rb_mVernierMarker, "Phase");
  rb_mVernierMarkerType = rb_define_module_under(rb_mVernierMarker, "Type");

  rb_cVernierCollector = rb_define_class_under(rb_mVernier, "Collector", rb_cObject);
  rb_undef_alloc_func(rb_cVernierCollector);
  rb_define_singleton_method(rb_cVernierCollector, "_new", collector_new, 2);
  rb_define_method(rb_cVernierCollector, "start", collector_start, 0);
  rb_define_method(rb_cVernierCollector, "sample", collector_sample, 0);
  rb_define_private_method(rb_cVernierCollector, "finish",  collector_stop, 0);
  rb_define_private_method(rb_cVernierCollector, "markers",  markers, 0);

  Init_consts(rb_mVernierMarkerPhase);

  //static VALUE gc_hook = Data_Wrap_Struct(rb_cObject, collector_mark, NULL, &_collector);
  //rb_global_variable(&gc_hook);
}
