#pragma once

//
// Search Engine — Fielded Search + Query DSL
//
// Data flow:
//   QueryNode tree  →  query_eval()  →  MatchAgg  →  insert-sort into results[]
//
// The worker is a pure interpreter: it evaluates a QueryNode tree against
// each opaque corpus entry.  Field selection, weighting, boolean combination,
// corpus data, and entry layout are all provided by the caller.
//

#include "utils.h"
#include "fuzzy.h"
#include <windows.h>

//
// Constants
//

#define SEARCH_MAX_RESULTS 200
#define SEARCH_QUERY_BUF   512
#define SEARCH_SCRATCH_MB  1
#define SEARCH_MAX_WORKERS 8

//
// Worker thread parameter — packed into SearchState so it outlives search_start.
//

typedef struct SearchState SearchState;

typedef struct
{
    SearchState* state;
    i32 worker_id;
} SearchWorkerParam;

//
// Field descriptor
//

// Extract a field string from an opaque entry pointer.
// Returned String points into the entry — no arena allocation needed.
typedef String (*FieldExtractFn)(const void* entry);

// Optional per-entry score adjustment (called by the worker after query_eval).
// entry — opaque corpus entry pointer (same as passed to query_eval)
// raw_score — score returned by query_eval (lower = better match)
// Returns adjusted score.  If NULL, no adjustment is applied.
typedef f32 (*ScoreAdjustFn)(const void* entry, f32 raw_score);

typedef struct
{
    const char* name; // field name, e.g. "key" / "text" / "tag"
    FieldExtractFn extract; // field extraction function
    float weight; // score multiplier, 1.0f = default
} FieldDef;

//
// Query DSL — a minimal tree of search terms
//

typedef enum
{
    QN_TERM, // search a term in a set of fields
    QN_AND, // both children must match
    QN_OR, // either child matches
} QueryNodeKind;

typedef struct QueryNode QueryNode;
struct QueryNode
{
    QueryNodeKind kind;

    // QN_TERM
    String term;
    const FieldDef* fields;
    i32 field_count;

    // QN_AND / QN_OR
    QueryNode* left;
    QueryNode* right;
};

//
// Aggregated match result for one corpus entry
//

typedef struct
{
    float score;
    FuzzyRange ranges[FUZZY_MAX_RANGES]; // from the best-scoring field
    i32 range_count;
    b32 matched;

    // Text the ranges refer to (points into the corpus entry)
    const u8* ref_text;
    isize ref_text_len;
} MatchAgg;

//
// DSL evaluator (pure function, scratch for temporaries)
//

// Recursively evaluate a QueryNode against one opaque corpus entry.
//   QN_TERM: iterates fields, calls fuzzy_match(), takes best score * field.weight.
//   QN_AND:  both children must be matched, score = l.score + r.score.
//   QN_OR:   either child may match, score = min(l.score, r.score) (lower is better).
MatchAgg query_eval(const QueryNode* node, const void* entry, Arena* scratch);

//
// Convenience constructors (arena-allocated, no manual free)
//

QueryNode* qn_term(Arena* a, String term, const FieldDef* fields, i32 field_count);
QueryNode* qn_and(Arena* a, QueryNode* l, QueryNode* r);
QueryNode* qn_or(Arena* a, QueryNode* l, QueryNode* r);

//
// Search result
//

typedef struct
{
    const void* entry; // opaque pointer to the matched corpus entry

    String key; // stable display label (points into caller-owned corpus)
    String text; // matched text (points into caller-owned corpus)
    f32 score;
    i32 range_count;
    FuzzyRange ranges[FUZZY_MAX_RANGES]; // byte offsets into `text`
} SearchResult;

//
// Shared search state (UI ↔ Worker)
//

// Two SRWLOCKs:
//   query_lock  — protects query_buf / query_len (worker snapshots, UI publishes)
//   results_lock — protects results[] / result_count
//
// Protocol (latest-state-wins):
//   UI:  AcquireSRWLockExclusive(query_lock) → write query_buf/query_len →
//        InterlockedIncrement(version) → ReleaseSRWLockExclusive
//   Worker: acquires query_lock shared → reads version + snapshots query →
//           searches → checks version again → if still current, publishes
//           under results_lock
//
// Ranges in SearchResult are UTF-8 byte offsets into `text`,
// NOT codepoint indices — required for CJK highlight slicing.

struct SearchState
{
    // Corpus (read-only after init, opaque — search.c walks via stride arithmetic)
    const void* corpus;
    i32 corpus_size;
    isize entry_stride;

    // Query input (UI writes, worker snapshots — query_lock)
    byte query_buf[SEARCH_QUERY_BUF];
    isize query_len;
    volatile LONG query_version;
    SRWLOCK query_lock;

    // Search output (worker writes, UI reads — results_lock)
    SearchResult results[SEARCH_MAX_RESULTS];
    i32 result_count;
    volatile LONG published_version;
    SRWLOCK results_lock;

    // Worker control
    HANDLE wake_event;
    HANDLE thread_handle;
    volatile LONG running;

    // Active field configuration (set by search_init)
    const FieldDef* active_fields;
    i32 active_field_count;

    // Result-key extraction (set by search_init)
    FieldExtractFn key_extract;

    // Optional per-entry score adjuster (e.g. frequency weighting).
    // Set by caller after search_init.  Default NULL (no adjustment).
    ScoreAdjustFn score_adjust;

    //
    // Multi-thread worker pool (coordinator + K parallel workers)
    //
    // Coordinator builds QueryNode tree, slices corpus, wakes each worker
    // via its own worker_events[i], waits for done_event, version‑checks,
    // K‑way merges, and publishes.
    // Workers process their pre‑assigned slice with per‑entry checkpoint
    // version‑abort.  Per‑worker event eliminates token‑theft and done_count
    // mismatch bugs inherent in semaphore‑based broadcast.
    //

    HANDLE worker_handles[SEARCH_MAX_WORKERS];
    HANDLE worker_events[SEARCH_MAX_WORKERS]; // coordinator → worker[i]: one shot per round (auto‑reset)
    HANDLE done_event; // last worker → coordinator: all‑done signal
    volatile LONG done_count; // workers finished this round
    volatile LONG search_round; // generation counter: invalidates stale workers
    volatile LONG query_version_snapshot; // stable version ref for worker checkpoint
    const QueryNode* active_query_tree; // coordinator builds; workers read‑only

    struct
    {
        i32 start;
        i32 end;
    } slices[SEARCH_MAX_WORKERS]; // coordinator writes per round; workers read own slot

    // Per‑worker result buffers — heap‑allocated in search_init, freed in search_stop.
    // DO NOT stack‑allocate SearchState: sizeof(SearchResult[K][200]) ~ 900 KB.
    SearchResult (*worker_results)[SEARCH_MAX_RESULTS];
    i32 worker_result_counts[SEARCH_MAX_WORKERS];

    SearchWorkerParam worker_params[SEARCH_MAX_WORKERS]; // thread startup params (outsurvives stack)

    i32 worker_count; // actual worker count (set in search_init)
};

//
// API
//

// Initialise search state.  Returns False if any required parameter is
// NULL/zero; the state is left zero-filled in that case.
// Caller owns corpus memory, field definitions, and all extraction functions —
// search.c has zero knowledge of entry layout or data contents.
b32 search_init(SearchState* state, const void* corpus, i32 corpus_size, isize entry_stride, const FieldDef* fields,
                i32 field_count, FieldExtractFn key_extract);

// Launch the background worker thread.  Returns False if the thread could not
// be created; the state remains valid for a subsequent retry.
b32 search_start(SearchState* state);

// Signal the worker to exit and join the thread.  Idempotent.
void search_stop(SearchState* state);

// UI thread: publish a new query.  Wakes the worker asynchronously.
void search_set_query(SearchState* state, String query);

// UI thread: copy current results (up to max_count) into `out`.
// Returns the number of results copied.  Non-blocking.
i32 search_get_results(SearchState* state, SearchResult* out, i32 max_count);
