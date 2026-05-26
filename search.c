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
// Worker thread — runs the search loop on a background thread.
//

// Lifecycle:
//   1. Create scratch arena once (reused across all searches).
//   2. Wait INFINITE on wake_event — SetEvent() in search_set_query() wakes us.
//   3. Snapshot query atomically under query_lock → never read shared
//      query_buf after this point.
//   4. Build a QueryNode tree from the query and active_fields,
//      then evaluate it against each corpus entry via query_eval().
//   5. Per-entry version checkpoint: if query changed mid-search, abort early
//      so a newer query can take over immediately.
//   6. Final version check (latest-state-wins) → publish under results_lock.
//   7. On exit (running == 0): release scratch arena.
//

// How many corpus entries between version-checkpoints when searching.
// Tuned so a 50-entry corpus with dual fuzzy_match() per entry still
// aborts within ~1 ms of a new keystroke.
#define SEARCH_CHECKPOINT_INTERVAL 8

static DWORD WINAPI search_worker(LPVOID param)
{
    SearchState* state = (SearchState*)param;
    i32 last_version = 0;

    /* Scratch arena is worker-thread-local: created on entry,
       released on exit.  search_stop() + search_start() naturally
       produces a fresh arena on the next worker incarnation. */
    Arena scratch = arena_new(MB(SEARCH_SCRATCH_MB));

    while (state->running)
    {
        /* INFINITE wait — purely event-driven.
           search_set_query() calls SetEvent() after publishing. */
        WaitForSingleObject(state->wake_event, INFINITE);
        if (!state->running)
            break;

        /* ---------- snapshot query under query_lock ----------
           The version check and the query-buffer copy happen inside
           the same shared-lock critical section so the worker sees a
           self-consistent (version, query_buf, query_len) triple.
           After this point the worker never reads shared query state
           again — it works exclusively from the local snapshot. */

        byte local_query[SEARCH_QUERY_BUF];
        isize local_len = 0;
        AcquireSRWLockShared(&state->query_lock);
        {
            i32 cur_version = state->query_version;
            if (cur_version == last_version)
            {
                ReleaseSRWLockShared(&state->query_lock);
                continue;
            }
            last_version = cur_version;
            local_len = state->query_len;
            if (local_len > 0)
                memcpy(local_query, state->query_buf, (usize)local_len);
        }
        ReleaseSRWLockShared(&state->query_lock);

        if (local_len == 0)
        {
            AcquireSRWLockExclusive(&state->results_lock);
            state->result_count = 0;
            ReleaseSRWLockExclusive(&state->results_lock);
            continue;
        }

        String query = { local_query, local_len };

        /* Reset scratch arena for this search iteration.
           Only rewinds the allocation cursor — no VirtualFree,
           so committed pages stay hot across searches. */
        scratch.pos = 0;

        /* Build the QueryNode tree from the query and active fields.
           The tree is arena-allocated (freed on the next scratch reset) and
           evaluated against every corpus entry in the loop below. */
        QueryNode* root = qn_term(&scratch, query, state->active_fields, state->active_field_count);

        /* Per-search local results, insertion-sorted by score ascending.
           insert-sort is fine for the alpha 50-entry corpus;
           larger corpora will need a min-heap or partial sort. */
        SearchResult local_results[SEARCH_MAX_RESULTS];
        i32 local_count = 0;

        i32 corpus_size = state->corpus_size;
        i32 checkpoint = 0;

        for (i32 i = 0; i < corpus_size; i++)
        {
            /* Lock-free checkpoint read.

               This read is only an opportunistic early-abort hint used to reduce
               wasted work while scanning large corpora.  It is NOT relied upon
               for correctness.

               Actual correctness comes from:
                 1. snapshotting query state under query_lock
                 2. the final latest-state-wins guard before publishing results

               Therefore a slightly stale read here is acceptable in alpha-stage
               Win32/x64 builds. */
            if (++checkpoint >= SEARCH_CHECKPOINT_INTERVAL)
            {
                checkpoint = 0;
                if (state->query_version != last_version)
                    goto abort_search;
            }

            const void* entry = (const u8*)state->corpus + (isize)i * state->entry_stride;

            /* Evaluate the QueryNode tree against this entry.
               query_eval() handles field extraction, weighting, and
               boolean combination — the worker only interprets the result. */
            MatchAgg agg = query_eval(root, entry, &scratch);
            if (!agg.matched)
                continue;

            if (state->score_adjust)
                agg.score = state->score_adjust(entry, agg.score);

            /* Resolve the display key via the caller-provided extraction function. */
            String key_str = state->key_extract((const void*)entry);

            /* Insert into sorted position (ascending score).
               Alpha-stage simple top‑k for small corpora;
               larger vocab will switch to a heap or partial sort. */
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
                    .entry       = entry,
                    .key         = key_str,
                    .text        = (String){ (u8*)agg.ref_text, agg.ref_text_len },
                    .score       = agg.score,
                    .range_count = agg.range_count,
                };
                memcpy(local_results[pos].ranges, agg.ranges, (usize)agg.range_count * sizeof(FuzzyRange));
            }
        }

    abort_search:

        /* Final latest-state-wins validation.

           Even if the checkpoint above missed a concurrent query update,
           results are never published unless the worker still matches the
           newest known query_version at commit time. */
        if (state->query_version != last_version)
            continue;

        /* ---------- publish results ----------
           memcpy and result_count assignment happen inside the same
           exclusive lock; the UI's shared-lock read sees a consistent
           snapshot regardless of ordering within the critical section. */
        AcquireSRWLockExclusive(&state->results_lock);
        if (local_count > 0)
            memcpy(state->results, local_results, (usize)local_count * sizeof(SearchResult));
        state->result_count = local_count;
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
    state->wake_event = CreateEventExW(NULL, NULL, 0, EVENT_ALL_ACCESS);
    if (!state->wake_event)
        return False;

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
    state->thread_handle = CreateThread(NULL, 0, search_worker, state, 0, NULL);
    if (!state->thread_handle)
    {
        state->running = 0;
        return False;
    }
    return True;
}

void search_stop(SearchState* state)
{
    /* Order matters: set running=0 first so the worker exits its loop,
       then SetEvent() to wake a potentially sleeping worker. */
    state->running = 0;
    if (state->wake_event)
        SetEvent(state->wake_event);
    if (state->thread_handle)
    {
        WaitForSingleObject(state->thread_handle, INFINITE);
        CloseHandle(state->thread_handle);
        state->thread_handle = NULL;
    }
    if (state->wake_event)
    {
        CloseHandle(state->wake_event);
        state->wake_event = NULL;
    }
}

void search_set_query(SearchState* state, String query)
{
    isize len = query.len;
    if (len > SEARCH_QUERY_BUF - 1)
        len = SEARCH_QUERY_BUF - 1;

    /* Write query under exclusive lock so the worker never sees
       a torn write (partial query_buf with mismatched query_len). */
    AcquireSRWLockExclusive(&state->query_lock);
    if (len > 0)
        memcpy(state->query_buf, query.data, (usize)len);
    state->query_len = len;
    InterlockedIncrement(&state->query_version);
    ReleaseSRWLockExclusive(&state->query_lock);

    SetEvent(state->wake_event);
}

i32 search_get_results(SearchState* state, SearchResult* out, i32 max_count)
{
    /* Shared lock — copies results out atomically so the UI always
       reads a self-consistent snapshot.  No blocking, no search work
       on the UI thread. */
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
            printf("      [%d] %.*s  score=%.1f  ranges=%d\n", i, (int)results[i].key.len, results[i].key.data, results[i].score,
                   results[i].range_count);
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
            printf("      [%d] %.*s  score=%.1f  ranges=%d\n", i, (int)results[i].key.len, results[i].key.data, results[i].score,
                   results[i].range_count);
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
            printf("      [%d] %.*s  score=%.1f  ranges=%d\n", i, (int)results[i].key.len, results[i].key.data, results[i].score,
                   results[i].range_count);
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
            printf("      [%d] %.*s  score=%.1f  ranges=%d\n", i, (int)results[i].key.len, results[i].key.data, results[i].score,
                   results[i].range_count);
    }

    search_stop(&state);

    printf("\n=== All tests complete ===\n");
    return 0;
}

#endif // SEARCH_STANDALONE_TEST
