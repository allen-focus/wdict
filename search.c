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
    Arena scratch = arena_new(MB(SEARCH_SCRATCH_MB), KB(64));

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
        state->worker_handles[i] = CreateThread(NULL, 0, search_worker_thread, &state->worker_params[i], 0, NULL);
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
        state->worker_handles[i] = CreateThread(NULL, 0, search_worker_thread, &state->worker_params[i], 0, NULL);
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
        /* Re‑check under the lock — another thread may have merged between
           the worker_done_rounds check and acquiring the exclusive lock. */
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
