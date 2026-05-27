//
// Search Engine — Fielded Search + Query DSL
//
// Data flow:
//   QueryNode tree  →  query_eval()  →  MatchAgg  →  insert-sort into results[]
//
// The worker is a pure interpreter: it evaluates a QueryNode tree against
// each corpus entry.  Field selection, weighting, and boolean combination
// are all described by the tree, not hard-coded in the worker.
//

#include "search.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//
// Query DSL evaluator
//

MatchAgg query_eval(const QueryNode* node, const void* entry, Arena* scratch)
{
    switch (node->kind)
    {
        case QN_TERM:
        {
            MatchAgg best = { .score = 1e8f, .matched = False };

            if (node->term.len == 0)
                return best;

            for (i32 fi = 0; fi < node->field_count; fi++)
            {
                const FieldDef* field = &node->fields[fi];
                String field_text = field->extract(entry);
                if (field_text.len == 0)
                    continue;

                FuzzyMatch m = fuzzy_match(node->term, field_text, scratch);
                if (m.score >= 1e8f || m.range_count == 0)
                    continue;

                float weighted = m.score * field->weight;
                if (weighted < best.score)
                {
                    best.score = weighted;
                    best.range_count = m.range_count;
                    best.matched = True;
                    best.ref_text = (const u8*)field_text.data;
                    best.ref_text_len = field_text.len;
                    memcpy(best.ranges, m.ranges, (usize)m.range_count * sizeof(FuzzyRange));
                }
            }
            return best;
        }

        case QN_AND:
        {
            MatchAgg l = query_eval(node->left, entry, scratch);
            if (!l.matched)
                return (MatchAgg){ .score = 1e8f, .matched = False };

            MatchAgg r = query_eval(node->right, entry, scratch);
            if (!r.matched)
                return (MatchAgg){ .score = 1e8f, .matched = False };

            // Pick the better-scoring child's ranges for highlighting
            MatchAgg result;
            result.score = l.score + r.score;
            result.matched = True;
            if (l.score < r.score)
            {
                result.range_count = l.range_count;
                result.ref_text = l.ref_text;
                result.ref_text_len = l.ref_text_len;
                memcpy(result.ranges, l.ranges, (usize)l.range_count * sizeof(FuzzyRange));
            }
            else
            {
                result.range_count = r.range_count;
                result.ref_text = r.ref_text;
                result.ref_text_len = r.ref_text_len;
                memcpy(result.ranges, r.ranges, (usize)r.range_count * sizeof(FuzzyRange));
            }
            return result;
        }

        case QN_OR:
        {
            MatchAgg l = query_eval(node->left, entry, scratch);
            MatchAgg r = query_eval(node->right, entry, scratch);

            if (!l.matched && !r.matched)
                return (MatchAgg){ .score = 1e8f, .matched = False };
            if (!l.matched)
                return r;
            if (!r.matched)
                return l;
            return l.score < r.score ? l : r;
        }
    }

    Assert(0);
    return (MatchAgg){ .score = 1e8f, .matched = False };
}

//
// Convenience constructors (arena-allocated)
//

QueryNode* qn_term(Arena* a, String term, const FieldDef* fields, i32 field_count)
{
    QueryNode* node = arena_push(a, sizeof(QueryNode), _Alignof(QueryNode), 1);
    memset(node, 0, sizeof(*node));
    node->kind = QN_TERM;
    if (term.len > 0)
    {
        u8* buf = arena_push(a, term.len, 1, 1);
        memcpy(buf, term.data, (usize)term.len);
        node->term = (String){ buf, term.len };
    }
    node->fields = fields;
    node->field_count = field_count;
    return node;
}

QueryNode* qn_and(Arena* a, QueryNode* l, QueryNode* r)
{
    QueryNode* node = arena_push(a, sizeof(QueryNode), _Alignof(QueryNode), 1);
    memset(node, 0, sizeof(*node));
    node->kind = QN_AND;
    node->left = l;
    node->right = r;
    return node;
}

QueryNode* qn_or(Arena* a, QueryNode* l, QueryNode* r)
{
    QueryNode* node = arena_push(a, sizeof(QueryNode), _Alignof(QueryNode), 1);
    memset(node, 0, sizeof(*node));
    node->kind = QN_OR;
    node->left = l;
    node->right = r;
    return node;
}

//
// Multi-thread worker pool — coordinator + K parallel workers
//
// Coordinator: waits on wake_event → snapshots query → builds tree →
//   slices corpus → SetEvent(worker_events[i]) per worker → waits done_event →
//   version check → K‑way merge → publish.
//
// Workers (K threads): WaitForSingleObject(worker_events[worker_id]) →
//   process own pre‑assigned slice with checkpoint version‑abort →
//   write results to per‑slot buffer → InterlockedIncrement done_count;
//   last worker SetEvent(done_event).
//
// Arena isolation: coordinator's arena holds the tree (immutable during
// search pass), each worker has its own arena for fuzzy_match DP tables.
//

#define SEARCH_CHECKPOINT_INTERVAL 8

// K‑way merge of per‑worker sorted result arrays.  O(K × max_count).
static i32 merge_k_sorted(SearchState* state, SearchResult* out, i32 max_count)
{
    i32 out_count = 0;
    i32 cursors[SEARCH_MAX_WORKERS] = { 0 };
    i32 k = state->worker_count;

    while (out_count < max_count)
    {
        i32 best_wi = -1;
        f32 best_score = 1e9f;

        for (i32 wi = 0; wi < k; wi++)
        {
            if (cursors[wi] < state->worker_result_counts[wi])
            {
                f32 s = state->worker_results[wi][cursors[wi]].score;
                if (s < best_score)
                {
                    best_score = s;
                    best_wi = wi;
                }
            }
        }

        if (best_wi < 0)
            break;
        out[out_count++] = state->worker_results[best_wi][cursors[best_wi]++];
    }
    return out_count;
}

static DWORD WINAPI search_worker_thread(LPVOID param)
{
    SearchWorkerParam* wp = (SearchWorkerParam*)param;
    SearchState* state = wp->state;
    i32 my_id = wp->worker_id;

    /* Per-worker arena — isolated from coordinator and other workers. */
    Arena scratch = arena_new(MB(SEARCH_SCRATCH_MB));

    while (state->running)
    {
        WaitForSingleObject(state->worker_events[my_id], INFINITE);
        if (!state->running)
            break;

        /* Coordinator directly assigned this worker's slice before signalling.
           No self‑assignment — eliminates token theft and done_count mismatch. */
        i32 my_index = my_id;
        i32 start = state->slices[my_index].start;
        i32 end = state->slices[my_index].end;
        i32 snapshot_version = InterlockedOr(&state->query_version_snapshot, 0);
        const QueryNode* root = state->active_query_tree;
        i32 my_round = InterlockedOr(&state->search_round, 0);

        {
            char dbg[160];
            snprintf(dbg, sizeof(dbg), "[SRCH] worker[%d]: slice [%d..%d) round=%ld root=%d\n",
                (int)my_id, (int)start, (int)end, (long)my_round, root != NULL);
            OutputDebugStringA(dbg);
        }

        if (!root)
        {
            if (InterlockedOr(&state->search_round, 0) == my_round)
            {
                state->worker_result_counts[my_index] = 0;
                if (InterlockedIncrement(&state->done_count) == state->worker_count)
                    SetEvent(state->done_event);
            }
            continue;
        }

        SearchResult local_results[SEARCH_MAX_RESULTS];
        i32 local_count = 0;
        i32 checkpoint = 0;

        for (i32 i = start; i < end; i++)
        {
            scratch.pos = 0;

            /* Checkpoint abort: bail if a newer query arrived mid-search. */
            if (++checkpoint >= SEARCH_CHECKPOINT_INTERVAL)
            {
                checkpoint = 0;
                if (InterlockedOr(&state->query_version, 0) != snapshot_version)
                    goto worker_done;
            }

            const void* entry = (const u8*)state->corpus + (isize)i * state->entry_stride;
            MatchAgg agg = query_eval(root, entry, &scratch);
            if (!agg.matched)
                continue;

            if (state->score_adjust)
                agg.score = state->score_adjust(entry, agg.score);

            String key_str = state->key_extract((const void*)entry);

            i32 pos = 0;
            while (pos < local_count && local_results[pos].score < agg.score)
                pos++;

            if (pos < SEARCH_MAX_RESULTS)
            {
                if (local_count < SEARCH_MAX_RESULTS)
                    local_count++;
                i32 to_move = local_count - 1 - pos;
                if (to_move > 0)
                    memmove(&local_results[pos + 1], &local_results[pos], (usize)to_move * sizeof(SearchResult));

                local_results[pos] = (SearchResult){
                    .entry = entry,
                    .key = key_str,
                    .text = (String){ (u8*)agg.ref_text, agg.ref_text_len },
                    .score = agg.score,
                    .range_count = agg.range_count,
                };
                memcpy(local_results[pos].ranges, agg.ranges, (usize)agg.range_count * sizeof(FuzzyRange));
            }
        }

    worker_done:
        /* Commit results only if still in our round; stale workers discard.
           But ALWAYS signal done_count — coordinator must know workers drained
           before it resets scratch arena / done_count for the next round. */
        if (InterlockedOr(&state->search_round, 0) == my_round)
        {
            state->worker_result_counts[my_index] = local_count;
            if (local_count > 0)
                memcpy(state->worker_results[my_index], local_results, (usize)local_count * sizeof(SearchResult));
        }
        else
        {
            state->worker_result_counts[my_index] = 0;
        }

        {
            LONG dc = InterlockedOr(&state->done_count, 0);
            LONG sr = InterlockedOr(&state->search_round, 0);
            char dbg[160];
            snprintf(dbg, sizeof(dbg), "[SRCH] worker[%d]: DONE local=%d round_chk=%d(dc_was=%ld sr=%ld mr=%ld)\n",
                (int)my_id, (int)local_count, sr == my_round, (long)dc, (long)sr, (long)my_round);
            OutputDebugStringA(dbg);
        }

        if (InterlockedIncrement(&state->done_count) == state->worker_count)
            SetEvent(state->done_event);
    }

    arena_release(&scratch);
    return 0;
}

// Coordinator thread: receives queries, builds the QueryNode tree,
// dispatches work to the worker pool, merges results, and publishes.
//
// The version check runs BEFORE any blocking wait, so when the inner
// WaitForMultipleObjects consumes wake_event, the outer loop sees
// got_new==True on re-entry and skips the blocking Wait.
static DWORD WINAPI search_coordinator(LPVOID param)
{
    SearchState* state = (SearchState*)param;
    i32 last_version = 0;
    Arena scratch = arena_new(MB(SEARCH_SCRATCH_MB));

    while (state->running)
    {
        /* Check for new query FIRST.  Only block when there genuinely
           is no pending work. */
        byte local_query[SEARCH_QUERY_BUF];
        isize local_len = 0;
        b32 got_new = False;

        AcquireSRWLockShared(&state->query_lock);
        {
            i32 cur = state->query_version;
            if (cur != last_version)
            {
                last_version = cur;
                local_len = state->query_len;
                if (local_len > 0)
                    memcpy(local_query, state->query_buf, (usize)local_len);
                got_new = True;
            }
        }
        ReleaseSRWLockShared(&state->query_lock);

        if (got_new)
        {
            char dbg[160];
            snprintf(dbg, sizeof(dbg), "[SRCH] coord: GOT_NEW last_ver=%ld cur_ver=%ld len=%lld\n",
                (long)last_version, (long)InterlockedOr(&state->query_version, 0), (long long)local_len);
            OutputDebugStringA(dbg);
        }

        if (!got_new)
        {
            WaitForSingleObject(state->wake_event, INFINITE);
            if (!state->running)
                break;
            continue;
        }

        if (local_len == 0)
        {
            {
                char dbg[128];
                snprintf(dbg, sizeof(dbg), "[SRCH] coord: EMPTY query pub_ver=%ld\n", (long)last_version);
                OutputDebugStringA(dbg);
            }
            AcquireSRWLockExclusive(&state->results_lock);
            state->result_count = 0;
            state->published_version = last_version;
            ReleaseSRWLockExclusive(&state->results_lock);
            continue;
        }

        String query = { local_query, local_len };
        scratch.pos = 0;
        QueryNode* root = qn_term(&scratch, query, state->active_fields, state->active_field_count);

        i32 total = state->corpus_size;
        i32 k = state->worker_count;
        i32 base = 0;
        for (i32 wi = 0; wi < k; wi++)
        {
            i32 remaining = total - base;
            i32 workers_left = k - wi;
            i32 chunk = remaining / workers_left;
            state->slices[wi].start = base;
            base += chunk;
            state->slices[wi].end = base;
            state->worker_result_counts[wi] = 0;
        }

        // Drain any stale wake_event left over from a prior spurious wakeup.
        // Without this, WaitForMultipleObjects below can fire immediately on
        // the stale event, aborting the round before results are published.
        WaitForSingleObject(state->wake_event, 0);

        InterlockedIncrement(&state->search_round);
        {
            char dbg[160];
            snprintf(dbg, sizeof(dbg), "[SRCH] coord: START round=%ld k=%d ver=%ld\n",
                (long)InterlockedOr(&state->search_round, 0), (int)k, (long)last_version);
            OutputDebugStringA(dbg);
        }
        ResetEvent(state->done_event);
        InterlockedExchange(&state->done_count, 0);
        // Slice assignment: coordinator writes each worker's slice directly,
        // then signals its dedicated event.  No self‑assignment, no token theft.
        state->active_query_tree = root;
        InterlockedExchange(&state->query_version_snapshot, last_version);
        for (i32 wi = 0; wi < k; wi++)
            SetEvent(state->worker_events[wi]);

        HANDLE handles[2] = { state->done_event, state->wake_event };
        DWORD r = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (!state->running)
            break;

        {
            char dbg[128];
            snprintf(dbg, sizeof(dbg), "[SRCH] coord: WAIT ret=%d (0=DONE 1=WAKE)\n",
                (int)(r - WAIT_OBJECT_0));
            OutputDebugStringA(dbg);
        }

        if (r == WAIT_OBJECT_0 + 1)
        {
            {
                char dbg[128];
                snprintf(dbg, sizeof(dbg), "[SRCH] coord: WAKE path, waiting for done_event\n");
                OutputDebugStringA(dbg);
            }
            // New query or stop.  Workers will abort at their next checkpoint,
            // but we must wait until every worker has signalled done_count
            // before resetting scratch arena / done_count for the next round.
            if (state->running)
                WaitForSingleObject(state->done_event, INFINITE);
            continue;
        }

        if (InterlockedOr(&state->query_version, 0) != last_version)
        {
            {
                char dbg[128];
                snprintf(dbg, sizeof(dbg), "[SRCH] coord: DISCARD ver_chk1: ver=%ld last=%ld\n",
                    (long)InterlockedOr(&state->query_version, 0), (long)last_version);
                OutputDebugStringA(dbg);
            }
            continue;
        }

        SearchResult merged_results[SEARCH_MAX_RESULTS];
        i32 merged_count = merge_k_sorted(state, merged_results, SEARCH_MAX_RESULTS);

        AcquireSRWLockExclusive(&state->results_lock);
        // Re-check under the lock — query_version may have advanced between the
        // first check and here (after merge_k_sorted, which can be slow).
        if (InterlockedOr(&state->query_version, 0) != last_version)
        {
            {
                char dbg[128];
                snprintf(dbg, sizeof(dbg), "[SRCH] coord: DISCARD ver_chk2: ver=%ld last=%ld\n",
                    (long)InterlockedOr(&state->query_version, 0), (long)last_version);
                OutputDebugStringA(dbg);
            }
            ReleaseSRWLockExclusive(&state->results_lock);
            continue;
        }
        if (merged_count > 0)
            memcpy(state->results, merged_results, (usize)merged_count * sizeof(SearchResult));
        state->result_count = merged_count;
        state->published_version = last_version;
        {
            char dbg[128];
            snprintf(dbg, sizeof(dbg), "[SRCH] coord: PUBLISH count=%d pub_ver=%ld\n",
                (int)merged_count, (long)last_version);
            OutputDebugStringA(dbg);
        }
        ReleaseSRWLockExclusive(&state->results_lock);
    }

    arena_release(&scratch);
    return 0;
}

#undef SEARCH_CHECKPOINT_INTERVAL

//
// Public API
//

b32 search_init(SearchState* state, const void* corpus, i32 corpus_size, isize entry_stride, const FieldDef* fields,
                i32 field_count, FieldExtractFn key_extract)
{
    if (!corpus || corpus_size <= 0 || entry_stride <= 0)
        return False;
    if (!fields || field_count <= 0 || !key_extract)
        return False;

    memset(state, 0, sizeof(*state));
    state->corpus = corpus;
    state->corpus_size = corpus_size;
    state->entry_stride = entry_stride;
    InitializeSRWLock(&state->query_lock);
    InitializeSRWLock(&state->results_lock);

    /* Wake event: UI → coordinator (auto‑reset). */
    state->wake_event = CreateEventExW(NULL, NULL, 0, EVENT_ALL_ACCESS);
    if (!state->wake_event)
        return False;

    /* Done event: last worker → coordinator (auto‑reset). */
    state->done_event = CreateEventExW(NULL, NULL, 0, EVENT_ALL_ACCESS);
    if (!state->done_event)
    {
        CloseHandle(state->wake_event);
        state->wake_event = NULL;
        return False;
    }

    /* Per-worker wake events: coordinator → worker[i] (auto‑reset). */
    for (i32 i = 0; i < SEARCH_MAX_WORKERS; i++)
    {
        state->worker_events[i] = CreateEventExW(NULL, NULL, 0, EVENT_ALL_ACCESS);
        if (!state->worker_events[i])
        {
            CloseHandle(state->wake_event);
            CloseHandle(state->done_event);
            state->wake_event = NULL;
            state->done_event = NULL;
            for (i32 j = 0; j < i; j++)
            {
                CloseHandle(state->worker_events[j]);
                state->worker_events[j] = NULL;
            }
            return False;
        }
    }

    /* worker_count = min(logical_cores, SEARCH_MAX_WORKERS). */
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        i32 cores = (i32)si.dwNumberOfProcessors;
        if (cores < 1)
            cores = 1;
        state->worker_count = cores < SEARCH_MAX_WORKERS ? cores : SEARCH_MAX_WORKERS;
    }

    state->worker_results = malloc((usize)state->worker_count * sizeof(state->worker_results[0]));
    if (!state->worker_results)
    {
        CloseHandle(state->wake_event);
        CloseHandle(state->done_event);
        state->wake_event = NULL;
        state->done_event = NULL;
        for (i32 i = 0; i < SEARCH_MAX_WORKERS; i++)
        {
            if (state->worker_events[i])
            {
                CloseHandle(state->worker_events[i]);
                state->worker_events[i] = NULL;
            }
        }
        return False;
    }

    state->active_fields = fields;
    state->active_field_count = field_count;
    state->key_extract = key_extract;

    return True;
}

b32 search_start(SearchState* state)
{
    if (state->running || state->thread_handle)
        return True;

    state->running = 1;

    state->thread_handle = CreateThread(NULL, 0, search_coordinator, state, 0, NULL);
    if (!state->thread_handle)
    {
        state->running = 0;
        return False;
    }

    i32 k = state->worker_count;
    for (i32 i = 0; i < k; i++)
    {
        state->worker_params[i].state = state;
        state->worker_params[i].worker_id = i;
        state->worker_handles[i] = CreateThread(NULL, 0, search_worker_thread, &state->worker_params[i], 0, NULL);
        if (!state->worker_handles[i])
        {
            state->running = 0;
            SetEvent(state->wake_event);

            // Wake only the workers we successfully created (i of them, not k).
            for (i32 j = 0; j < i; j++)
                SetEvent(state->worker_events[j]);

            // Join coordinator.
            WaitForSingleObject(state->thread_handle, INFINITE);
            CloseHandle(state->thread_handle);
            state->thread_handle = NULL;

            // Join any workers that were already created.
            for (i32 j = 0; j < i; j++)
            {
                if (state->worker_handles[j])
                {
                    WaitForSingleObject(state->worker_handles[j], INFINITE);
                    CloseHandle(state->worker_handles[j]);
                    state->worker_handles[j] = NULL;
                }
            }
            return False;
        }
    }

    return True;
}

void search_stop(SearchState* state)
{
    state->running = 0;

    /* Wake coordinator and all workers so they see running=0 and exit. */
    if (state->wake_event)
        SetEvent(state->wake_event);
    if (state->done_event)
        SetEvent(state->done_event);
    for (i32 i = 0; i < state->worker_count; i++)
    {
        if (state->worker_events[i])
            SetEvent(state->worker_events[i]);
    }

    /* Join coordinator. */
    if (state->thread_handle)
    {
        WaitForSingleObject(state->thread_handle, INFINITE);
        CloseHandle(state->thread_handle);
        state->thread_handle = NULL;
    }

    /* Join all workers. */
    for (i32 i = 0; i < state->worker_count; i++)
    {
        if (state->worker_handles[i])
        {
            WaitForSingleObject(state->worker_handles[i], INFINITE);
            CloseHandle(state->worker_handles[i]);
            state->worker_handles[i] = NULL;
        }
    }

    /* Destroy synchronization objects. */
    if (state->wake_event)
    {
        CloseHandle(state->wake_event);
        state->wake_event = NULL;
    }
    if (state->done_event)
    {
        CloseHandle(state->done_event);
        state->done_event = NULL;
    }
    for (i32 i = 0; i < SEARCH_MAX_WORKERS; i++)
    {
        if (state->worker_events[i])
        {
            CloseHandle(state->worker_events[i]);
            state->worker_events[i] = NULL;
        }
    }

    free(state->worker_results);
    state->worker_results = NULL;
}

void search_set_query(SearchState* state, String query)
{
    isize len = query.len;
    if (len > SEARCH_QUERY_BUF - 1)
        len = SEARCH_QUERY_BUF - 1;

    // Skip unchanged queries to avoid aborting in-progress searches
    // every frame (the search popup render loop calls us unconditionally).
    AcquireSRWLockShared(&state->query_lock);
    b32 same = (len == state->query_len &&
                (len == 0 || memcmp(state->query_buf, query.data, (usize)len) == 0));
    ReleaseSRWLockShared(&state->query_lock);
    if (same)
        return;

    /* Invalidate old results immediately so the UI never reads stale data
       while the new search is in flight. */
    AcquireSRWLockExclusive(&state->results_lock);
    state->result_count = 0;
    state->published_version = 0;
    ReleaseSRWLockExclusive(&state->results_lock);

    /* Write query under exclusive lock so the worker never sees
       a torn write (partial query_buf with mismatched query_len). */
    AcquireSRWLockExclusive(&state->query_lock);
    if (len > 0)
        memcpy(state->query_buf, query.data, (usize)len);
    state->query_len = len;
    InterlockedIncrement(&state->query_version);
    {
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "[SRCH] set_query: len=%lld ver=%ld\n",
            (long long)len, (long)state->query_version);
        OutputDebugStringA(dbg);
    }
    ReleaseSRWLockExclusive(&state->query_lock);

    SetEvent(state->wake_event);
}

i32 search_get_results(SearchState* state, SearchResult* out, i32 max_count)
{
    AcquireSRWLockShared(&state->results_lock);
    LONG cur_ver = InterlockedOr(&state->query_version, 0);
    i32 count = 0;
    if (state->published_version == cur_ver)
    {
        count = state->result_count;
        if (count > max_count)
            count = max_count;
        if (count > 0)
            memcpy(out, state->results, (usize)count * sizeof(SearchResult));
    }
    {
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "[SRCH] get_results: pub_ver=%ld cur_ver=%ld -> %s count=%d\n",
            (long)state->published_version, (long)cur_ver,
            state->published_version == cur_ver ? "MATCH" : "MISS",
            count);
        OutputDebugStringA(dbg);
    }
    ReleaseSRWLockShared(&state->results_lock);
    return count;
}

//
// Standalone test
//

#ifdef SEARCH_STANDALONE_TEST

#    include <stdio.h>

/* Standalone test: owns its own corpus, entry layout, and extraction functions.
   search.c itself has zero knowledge of entry structure or data contents. */

typedef struct
{
    const u8* key;
    const u8* text;
} STEntry;

static const STEntry st_corpus[] = {
    { "linear", "线性回归是一种利用最小二乘法来建模自变量与因变量之间关系的统计方法" },
    { "algorithm", "算法是一组用于执行特定任务或解决特定问题的定义明确的计算步骤或规则" },
    { "dictionary", "字典是一种按字母顺序或其他顺序排列的词语列表提供定义发音词源及其他信息的工具书" },
    { "compile", "编译是将高级编程语言编写的源代码转换为机器可执行目标代码的过程通常由编译器完成" },
    { "function", "函数是程序中一段具有独立功能的代码块接收输入参数执行特定运算后返回计算结果" },
    { "variable", "变量是程序中用于存储和表示数据值的命名存储位置其值在程序运行过程中可以改变" },
    { "database", "数据库是按照数据结构来组织存储和管理数据的仓库通常由数据库管理系统进行统一控制" },
    { "interface", "接口是定义了类或模块之间交互规范的一组方法声明而不包含具体实现细节的抽象边界" },
    { "debug", "调试是开发和维护软件过程中发现定位分析并修复程序错误和缺陷的一系列活动" },
    { "optimize", "优化是在不改变程序外部行为的前提下通过改进算法或数据结构提升程序性能与效率" },
    { "configure", "配置是指根据特定需求和使用环境对软件系统的参数选项和功能进行设置和调整" },
    { "abstract", "抽象是将复杂系统中不相关的细节隐藏起来只关注核心特性和行为的思维过程" },
    { "network", "网络是将多台计算机或设备通过通信链路连接起来实现数据交换和资源共享的系统" },
    { "security", "安全是保护信息系统和数据免受未授权访问、使用、泄露、破坏、修改或销毁的综合措施" },
    { "performance", "性能是衡量计算机系统或软件在给定条件下完成任务所需资源和响应时间的综合指标" },
    { "framework", "框架是为了解决特定领域问题而预先设计好的一组类和规范提供可复用的软件架构" },
    { "deploy", "部署是将开发完成的软件系统安装配置到目标运行环境中使其能够正常对外提供服务" },
    { "module", "模块是将大型程序按照功能职责划分为若干独立单元每个单元具有明确接口和内聚性" },
    { "recursion", "递归是在函数定义中直接或间接调用自身来解决问题需要明确终止条件的编程技术" },
    { "iteration", "迭代是通过重复执行一组指令逐步逼近目标结果的过程常用于循环和数值计算方法" },
    { "encryption", "加密是利用密码学算法将明文数据转换为不可读密文以保护信息机密性的技术手段" },
    { "cache", "缓存是一种高速小容量存储区域用于临时保存频繁访问数据以减少对慢速存储的访问次数" },
    { "protocol", "协议是计算机网络中通信双方共同遵守的规则和标准确保数据能够正确传输和解析" },
    { "kernel", "内核是操作系统的核心部分负责管理硬件资源、进程调度、内存分配和文件系统等底层功能" },
    { "compiler", "编译器是将一种编程语言编写的源代码翻译为另一种语言或机器代码的程序" },
    { "pointer", "指针是存储内存地址的变量允许程序直接访问和操作内存中的数据实现高效数据结构" },
    { "exception", "异常是程序运行过程中发生的意外情况需要特殊处理机制来保证程序的健壮性和稳定性" },
    { "template", "模板是允许函数或类在不指定具体类型的情况下进行通用编程以提高代码复用率的语言特性" },
    { "translate", "翻译是指将一种语言的文字或语音转换为另一种语言的过程需要兼顾语义、语法和文化背景" },
    { "schedule", "调度是操作系统决定哪个进程在何时使用处理器的机制是多任务系统的核心功能之一" },
    { "document", "文档是用自然语言或形式化语言记录软件设计、功能、接口和使用方法的文字说明材料" },
    { "container", "容器技术是将应用程序及其依赖环境打包在一个隔离的容器中实现跨平台一致运行和快速部署" },
    { "good", "积极正面的评价或状态表示事物符合预期标准具备令人满意的品质或达到了理想的效果" },
    { "bad", "负面的不合格的表示某事物存在缺陷或未能达到最低可接受标准的情况或状态" },
    { "middleware", "中间件是位于操作系统和应用程序之间的软件层为分布式应用提供通信和数据管理服务" },
    { "cluster", "集群是将多台服务器组合在一起使其表现得像一台单一系统提供高可用性和负载均衡" },
    { "concurrency", "并发编程是允许多个计算任务同时执行的模式需要处理资源共享同步和死锁等问题" },
    { "memory", "内存泄漏是程序在分配内存后未能正确释放导致可用内存逐渐减少最终可能引发系统崩溃的问题" },
    { "garbage", "垃圾回收是一种自动内存管理技术由运行时环境自动识别和释放不再被引用的内存对象" },
    { "pattern", "设计模式是软件开发过程中针对常见问题的经过验证的可复用的解决方案模板" },
    { "virtual", "虚拟内存是操作系统提供的内存管理机制将硬盘空间作为内存的扩展使进程拥有独立的地址空间" },
    { "object", "面向对象编程是一种将数据和行为封装在对象中的程序设计范式支持继承多态和封装特性" },
    { "function", "函数式编程是一种将计算视为数学函数求值过程的编程范式强调不可变数据和无副作用" },
    { "service", "微服务架构是将单一应用程序划分为一组小型独立服务每个服务运行在自己的进程中并通过轻量级机制通信" },
    { "queue", "消息队列是一种异步通信协议允许应用程序之间通过传递消息进行解耦提高系统的可扩展性" },
    { "test", "单元测试是针对软件中最小可测试单元进行正确性验证的方法通常由开发人员自行编写" },
    { "integration", "持续集成是一种软件开发实践要求团队成员频繁将代码合并到主干并通过自动化构建和测试验证" },
    { "regex", "正则表达式是描述字符串匹配模式的形式化语言广泛用于文本搜索替换和输入验证等场景" },
    { "balance", "负载均衡是将网络流量或计算任务分配到多台服务器上以提高系统整体处理能力和可用性的技术" },
    { "control", "版本控制是记录文件内容变化以便将来查阅特定版本修订情况的系统用于协作开发中跟踪代码变更" },
    { "structure", "数据结构是计算机存储和组织数据的方式精心设计的数据结构可显著提升算法的运行效率" },
    { "server", "服务器是一种高性能计算机专门用于处理来自多个客户端的请求并提供相应的服务和资源" },
};

static String st_extract_key(const void* e)
{
    const STEntry* entry = (const STEntry*)e;
    return (String){ (u8*)entry->key, (isize)strlen(entry->key) };
}

static String st_extract_text(const void* e)
{
    const STEntry* entry = (const STEntry*)e;
    return (String){ (u8*)entry->text, (isize)strlen(entry->text) };
}

static const FieldDef ST_FIELDS_ALL[] = {
    { "key", st_extract_key, 1.2f },
    { "text", st_extract_text, 1.0f },
};
static const FieldDef ST_FIELDS_KEY[] = {
    { "key", st_extract_key, 1.0f },
};
static const FieldDef ST_FIELDS_TEXT[] = {
    { "text", st_extract_text, 1.0f },
};

int main(void)
{
    printf("=== Search Engine Test ===\n\n");

    SearchState state;
    if (!search_init(&state, st_corpus, countof(st_corpus), sizeof(STEntry), ST_FIELDS_ALL, countof(ST_FIELDS_ALL),
                     st_extract_key))
    {
        printf("FATAL: search_init failed\n");
        return 1;
    }
    if (!search_start(&state))
    {
        printf("FATAL: search_start failed\n");
        return 1;
    }

    /* Give the worker a moment to spin up */
    Sleep(50);

    /* Test 1: search with Chinese characters (ST_FIELDS_ALL — default) */
    printf("[1] Query: linear definition (all fields)\n");
    search_set_query(&state, str("线性回归"));
    Sleep(100);

    {
        SearchResult results[16];
        i32 n = search_get_results(&state, results, 16);
        printf("    %d results:\n", n);
        for (i32 i = 0; i < n; i++)
            printf("      [%d] %.*s  score=%.1f  ranges=%d\n", i, (int)results[i].key.len, results[i].key.data,
                   results[i].score, results[i].range_count);
    }

    /* Test 2: English prefix (ST_FIELDS_ALL — default) */
    printf("[2] Query: com (all fields)\n");
    search_set_query(&state, str("com"));
    Sleep(100);

    {
        SearchResult results[16];
        i32 n = search_get_results(&state, results, 16);
        printf("    %d results:\n", n);
        for (i32 i = 0; i < n; i++)
            printf("      [%d] %.*s  score=%.1f  ranges=%d\n", i, (int)results[i].key.len, results[i].key.data,
                   results[i].score, results[i].range_count);
    }

    /* Test 3: empty query */
    printf("[3] Query: (empty)\n");
    search_set_query(&state, (String){ NULL, 0 });
    Sleep(100);

    {
        SearchResult results[16];
        i32 n = search_get_results(&state, results, 16);
        printf("    %d results (expect 0)\n", n);
    }

    /* Test 4: key-only search — verify ST_FIELDS_KEY path */
    printf("[4] Query: com (key only)\n");
    state.active_fields = ST_FIELDS_KEY;
    state.active_field_count = countof(ST_FIELDS_KEY);
    search_set_query(&state, str("com"));
    Sleep(100);

    {
        SearchResult results[16];
        i32 n = search_get_results(&state, results, 16);
        printf("    %d results (should only match keys, not text):\n", n);
        for (i32 i = 0; i < n; i++)
            printf("      [%d] %.*s  score=%.1f  ranges=%d\n", i, (int)results[i].key.len, results[i].key.data,
                   results[i].score, results[i].range_count);
    }

    /* Test 5: text-only search — verify ST_FIELDS_TEXT path */
    printf("[5] Query: 回归 (text only)\n");
    state.active_fields = ST_FIELDS_TEXT;
    state.active_field_count = countof(ST_FIELDS_TEXT);
    search_set_query(&state, str("回归"));
    Sleep(100);

    {
        SearchResult results[16];
        i32 n = search_get_results(&state, results, 16);
        printf("    %d results (should match text content only):\n", n);
        for (i32 i = 0; i < n; i++)
            printf("      [%d] %.*s  score=%.1f  ranges=%d\n", i, (int)results[i].key.len, results[i].key.data,
                   results[i].score, results[i].range_count);
    }

    search_stop(&state);

    printf("\n=== All tests complete ===\n");
    return 0;
}

#endif // SEARCH_STANDALONE_TEST
