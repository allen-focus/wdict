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
#include <stdlib.h>
#include <string.h>

#define SEARCH_THREAD_STACK_SIZE KB(64)

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

//
// Multi-thread worker pool — K parallel workers, no coordinator
//
// search_set_query (UI thread): slices corpus → SetEvent(worker_events[i])
//   per worker → returns immediately (no blocking).
//
// Workers (K threads): WaitForSingleObject(worker_events[worker_id]) →
//   snapshot query from query_buf → build own QueryNode tree →
//   process own pre‑assigned slice with checkpoint version‑abort →
//   write results to per‑slot buffer → InterlockedExchange worker_done_rounds[i].
//
// search_get_results (UI thread): non‑blocking poll of worker_done_rounds;
//   when every worker's round matches search_round, K‑way merge + publish
//   under results_lock.  Per‑slot round counters eliminate the done_count
//   reset race (old workers cannot pollute new round counters).
//
// Arena isolation: each worker has its own arena — tree is built in the
// low region, fuzzy_match DP tables reuse the tail per‑entry.
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

    /* Per-worker arena — tree in low region, fuzzy_match DP tables reuse the tail. */
    MEM_TRACK("[mem] search worker %d: scratch arena = KB(256), commit_block = KB(64)\n", my_id);
    Arena scratch = arena_new(KB(256), KB(64));

    while (state->running)
    {
        WaitForSingleObject(state->worker_events[my_id], INFINITE);
        if (!state->running)
            break;

        i32 my_index = my_id;
        i32 start = state->slices[my_index].start;
        i32 end = state->slices[my_index].end;
        i32 snapshot_version = InterlockedOr(&state->query_version_snapshot, 0);
        i32 my_round = InterlockedOr(&state->search_round, 0);

        /* Snapshot query — each worker reads query_buf independently
           so no shared tree pointer or arena reuse race. */
        byte local_query[SEARCH_QUERY_BUF];
        isize local_len = 0;
        AcquireSRWLockShared(&state->query_lock);
        {
            local_len = state->query_len;
            if (local_len > 0)
                memcpy(local_query, state->query_buf, (usize)local_len);
        }
        ReleaseSRWLockShared(&state->query_lock);

        if (local_len == 0)
        {
            if (InterlockedOr(&state->search_round, 0) == my_round)
                state->worker_result_counts[my_index] = 0;
            InterlockedExchange(&state->worker_done_rounds[my_index], my_round);
            continue;
        }

        /* Build QueryNode tree in the worker's own arena.  Tree sits at the
           bottom; per‑entry fuzzy_match scratch reuses remaining arena. */
        scratch.pos = 0;
        String qs = { local_query, local_len };
        QueryNode* root = qn_term(&scratch, qs, state->active_fields, state->active_field_count);
        isize tree_pos = scratch.pos;

        SearchResult local_results[SEARCH_MAX_RESULTS];
        i32 local_count = 0;
        i32 checkpoint = 0;

        for (i32 i = start; i < end; i++)
        {
            /* Roll back arena past the tree so fuzzy_match has the full
               remainder to use for its DP tables. */
            scratch.pos = tree_pos;

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

        /* Mark completion unconditionally — round mismatch is naturally
           filtered by search_get_results which checks against search_round. */
        InterlockedExchange(&state->worker_done_rounds[my_index], my_round);
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

    /* Per-worker wake events: UI → worker[i] (auto‑reset). */
    for (i32 i = 0; i < SEARCH_MAX_WORKERS; i++)
    {
        state->worker_events[i] = CreateEventExW(NULL, NULL, 0, EVENT_ALL_ACCESS);
        if (!state->worker_events[i])
        {
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
    MEM_TRACK("[mem] malloc: worker_results = %d workers x %zu B = %zu B\n",
              state->worker_count, sizeof(state->worker_results[0]),
              (usize)state->worker_count * sizeof(state->worker_results[0]));
    if (!state->worker_results)
    {
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
    if (state->running)
        return True;

    state->running = 1;
    i32 k = state->worker_count;

    for (i32 i = 0; i < k; i++)
    {
        state->worker_params[i].state = state;
        state->worker_params[i].worker_id = i;
        MEM_TRACK("[mem] CreateThread: search worker %d (stack = %lld KB)\n", i, (long long)(SEARCH_THREAD_STACK_SIZE / 1024));
        state->worker_handles[i] = CreateThread(NULL, SEARCH_THREAD_STACK_SIZE, search_worker_thread, &state->worker_params[i], 0, NULL);
        if (!state->worker_handles[i])
        {
            state->running = 0;

            // Wake only the workers we successfully created (i of them, not k).
            for (i32 j = 0; j < i; j++)
                SetEvent(state->worker_events[j]);

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

    /* Wake all workers so they see running=0 and exit. */
    for (i32 i = 0; i < state->worker_count; i++)
    {
        if (state->worker_events[i])
            SetEvent(state->worker_events[i]);
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
    b32 same = (len == state->query_len && (len == 0 || memcmp(state->query_buf, query.data, (usize)len) == 0));
    ReleaseSRWLockShared(&state->query_lock);
    if (same)
        return;

    /* Write query under exclusive lock so the worker never sees
       a torn write (partial query_buf with mismatched query_len). */
    AcquireSRWLockExclusive(&state->query_lock);
    if (len > 0)
        memcpy(state->query_buf, query.data, (usize)len);
    state->query_len = len;
    InterlockedIncrement(&state->query_version);
    ReleaseSRWLockExclusive(&state->query_lock);

    /* Empty query: publish immediately, no workers to wake. */
    if (len == 0)
    {
        AcquireSRWLockExclusive(&state->results_lock);
        state->result_count = 0;
        state->published_version = InterlockedOr(&state->query_version, 0);
        ReleaseSRWLockExclusive(&state->results_lock);
        return;
    }

    /* Slice corpus + dispatch workers — all on the UI thread inline,
       eliminating the coordinator scheduling hop. */
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

    InterlockedIncrement(&state->search_round);
    InterlockedExchange(&state->query_version_snapshot, InterlockedOr(&state->query_version, 0));

    for (i32 wi = 0; wi < k; wi++)
        SetEvent(state->worker_events[wi]);
}

void search_reconfigure(SearchState* state, const FieldDef* fields, i32 field_count)
{
    if (!state->running)
        return;

    /* 1. Suspend */
    state->running = 0;
    for (i32 i = 0; i < state->worker_count; i++)
        SetEvent(state->worker_events[i]);
    for (i32 i = 0; i < state->worker_count; i++)
    {
        if (state->worker_handles[i])
        {
            WaitForSingleObject(state->worker_handles[i], INFINITE);
            CloseHandle(state->worker_handles[i]);
            state->worker_handles[i] = NULL;
        }
    }

    /* 2. Swap fields */
    state->active_fields = fields;
    state->active_field_count = field_count;

    /* 3. Invalidate version so workers treat next query as fresh */
    InterlockedIncrement(&state->query_version);

    /* 4. Resume */
    state->running = 1;
    for (i32 i = 0; i < state->worker_count; i++)
    {
        state->worker_params[i].state = state;
        state->worker_params[i].worker_id = i;
        MEM_TRACK("[mem] CreateThread: search worker %d (reconfigure, stack = %lld KB)\n", i, (long long)(SEARCH_THREAD_STACK_SIZE / 1024));
        state->worker_handles[i] = CreateThread(NULL, SEARCH_THREAD_STACK_SIZE, search_worker_thread, &state->worker_params[i], 0, NULL);
    }

    /* 5. Re-trigger search with current query — inline slice setup
       (search_set_query would dedup since we're comparing the same buffer). */
    {
        String current_query = { state->query_buf, state->query_len > 0 ? state->query_len : 0 };
        if (current_query.len > 0)
        {
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
        }
    }

    /* 6. Force-sync snapshot and round so workers see the correct
       checkpoint version and actually start processing. */
    InterlockedIncrement(&state->search_round);
    InterlockedExchange(&state->query_version_snapshot,
                        InterlockedOr(&state->query_version, 0));

    /* 7. Wake workers */
    for (i32 i = 0; i < state->worker_count; i++)
        SetEvent(state->worker_events[i]);
}

i32 search_get_results(SearchState* state, SearchResult* out, i32 max_count)
{
    LONG cur_ver = InterlockedOr(&state->query_version, 0);

    /* Non‑blocking: if all workers reached the current round but we haven't
       merged yet, do it here on the UI thread.  Per‑slot round counters
       eliminate the done_count reset race. */
    LONG cur_round = InterlockedOr(&state->search_round, 0);
    b32 all_done = True;
    for (i32 i = 0; i < state->worker_count; i++)
    {
        if (InterlockedOr(&state->worker_done_rounds[i], 0) != cur_round)
        {
            all_done = False;
            break;
        }
    }

    if (all_done && InterlockedOr(&state->published_version, 0) != cur_ver)
    {
        AcquireSRWLockExclusive(&state->results_lock);
        /* Re‑check under the lock — workers may have advanced between
           the first check and acquiring the exclusive lock. */
        all_done = True;
        for (i32 i = 0; i < state->worker_count; i++)
        {
            if (InterlockedOr(&state->worker_done_rounds[i], 0) != cur_round)
            {
                all_done = False;
                break;
            }
        }
        if (all_done && InterlockedOr(&state->published_version, 0) != cur_ver)
        {
            SearchResult merged_results[SEARCH_MAX_RESULTS];
            i32 merged_count = merge_k_sorted(state, merged_results, SEARCH_MAX_RESULTS);
            if (merged_count > 0)
                memcpy(state->results, merged_results, (usize)merged_count * sizeof(SearchResult));
            state->result_count = merged_count;
            state->published_version = cur_ver;
        }
        ReleaseSRWLockExclusive(&state->results_lock);
    }

    AcquireSRWLockShared(&state->results_lock);
    i32 count = state->result_count;
    if (count > max_count)
        count = max_count;
    if (count > 0)
        memcpy(out, state->results, (usize)count * sizeof(SearchResult));
    ReleaseSRWLockShared(&state->results_lock);
    return count;
}
