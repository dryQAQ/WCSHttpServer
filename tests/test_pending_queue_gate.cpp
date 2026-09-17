// ============================================================================
// test_pending_queue_gate.cpp — ★ 2026-09-17 现场缺陷（"跳过一个波次"）回归自测
//
// 现场现象（原话）：
//   多次下发不同波次任务时，点「结束任务」后系统自动接收了下一个任务波次
//   （此时开关还是「开始接收任务」）；再点「开始接收任务」时又接收了下一个波次
//   —— 错误地跳过一个波次数据。
//
// 现场要求：
//   只有点了「开始接收任务」，才开始接收波次队列里的任务。
//
// 本测试锁定"待执行波次队列出队时机"的口径（纯逻辑，不依赖 GUI/网络/DB），
// 判据直接取自产品侧同一份头文件 PendingWaveQueuePolicy.h（不是测试里另写一套）：
//   ① 未接收任务            → 冻结（不出队，队列原样保留）
//   ② 已点「结束任务」      → 冻结（哪怕本波次随后走到终态也不出队 = 缺陷本体）
//   ③ 接收中且未点结束任务  → 放行（终态后接替下一波；H5 取消同样放行 = 既有能力保留）
//   ④ 端到端时序（现场复现路径）：4 个波次依次下发 → 反复「结束任务/开始接收任务」
//      → 执行顺序必须与下发顺序完全一致（不跳号、不重复），且全程无"未点开始就执行"
//   ⑤ 重放被拦下的回滚必须是**放回队首**（同一波次仍最先执行），否则等价于跳过该波次
//
// 构建：见 tests\run_tests.bat（第 8 项）
// ============================================================================

#include <QCoreApplication>
#include <QString>
#include <deque>
#include <vector>
#include <cstdio>

#include "PendingWaveQueuePolicy.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(bool ok, const QString& what, const QString& got = QString())
{
    if (ok) { ++g_pass; std::printf("  [PASS] %s\n", what.toUtf8().constData()); }
    else
    {
        ++g_fail;
        std::printf("  [FAIL] %s%s\n", what.toUtf8().constData(),
                    got.isEmpty() ? "" : ("  <- 实际: " + got).toUtf8().constData());
    }
}

// ──── 判定结果 → 名字（断言失败时看得懂）────
static QString decName(PendingDequeueDecision d)
{
    switch (d)
    {
    case PDQ_ALLOW:         return "PDQ_ALLOW";
    case PDQ_QUEUE_EMPTY:   return "PDQ_QUEUE_EMPTY";
    case PDQ_NOT_RECEIVING: return "PDQ_NOT_RECEIVING";
    case PDQ_END_REQUESTED: return "PDQ_END_REQUESTED";
    }
    return "?";
}

// ──── 队列内容 → "A,B,C"（失败信息用）────
static QString join(const std::deque<QString>& q)
{
    QString s;
    for (const QString& x : q) { if (!s.isEmpty()) s += ","; s += x; }
    return s.isEmpty() ? QString("(空)") : s;
}
static QString join(const std::vector<QString>& v)
{
    QString s;
    for (const QString& x : v) { if (!s.isEmpty()) s += ","; s += x; }
    return s.isEmpty() ? QString("(空)") : s;
}

// ============================================================================
// 会话模型：用**产品侧同一套判据**驱动，复现 HttpServer 的行为
//   （m_receiving / m_endRequested / 待执行队列 / 已执行波次序列）
//   事件源与产品侧一一对应：
//     onStartReceive()     ← 点「开始接收任务」→ HttpServer::startReceive()（解冻后执行队首）
//     onEndTask()          ← 点「结束任务」    → HttpServer::sendEnd()（置 m_endRequested → 冻结）
//     onStopReceive()      ← H8 回调走完      → doActualStop() → HttpServer::stopReceive()
//     onTerminalStatus()   ← 波次终态信号      → maybeStartPendingWave()
//     onReplayBlocked(id)  ← 重放结果到达时会话已冻结 → enqueuePendingWave(front=true) 回滚
// ============================================================================
struct SessionModel
{
    bool m_receiving    = false;   // HTTP 接收层是否开启
    bool m_endRequested = false;   // 本次会话是否点过「结束任务」
    std::deque<QString>  queue;    // 待执行队列（FIFO）
    std::vector<QString> started;  // 已开始执行的波次（按顺序）
    std::vector<QString> startedWhileStopped;   // 违规：未接收(界面=开始接收任务)时被执行

    PendingDequeueDecision decision() const
    {
        return pendingDequeueDecision(m_receiving, m_endRequested, (int)queue.size());
    }

    // 终态事件 → 产品侧 maybeStartPendingWave()
    bool onTerminalStatus()
    {
        if (decision() != PDQ_ALLOW) return false;   // 冻结：不出队（产品侧只记日志）
        if (queue.empty())           return false;
        const QString head = queue.front();
        queue.pop_front();
        started.push_back(head);
        if (!m_receiving) startedWhileStopped.push_back(head);   // 判据失效才会发生
        return true;
    }

    // 点「开始接收任务」→ startReceive()：先解冻，再执行队首
    void onStartReceive()
    {
        m_receiving    = true;
        m_endRequested = false;
        onTerminalStatus();          // startReceive() 末尾的 maybeStartPendingWave()
    }

    void onEndTask()      { m_endRequested = true; }    // sendEnd()
    void onStopReceive()  { m_receiving = false; }      // stopReceive()

    // 重放结果被拦下 → 产品侧放回**队首**（并撤销这次"开始执行"）
    void onReplayBlocked(const QString& orderCode)
    {
        queue.push_front(orderCode);
        if (!started.empty()) started.pop_back();
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ── ① 未接收任务（没点「开始接收任务」）→ 冻结 ──
    std::printf("== ① 未接收任务（没点「开始接收任务」）→ 冻结 ==\n");
    {
        SessionModel s;
        s.queue = { "W1", "W2" };
        check(s.decision() == PDQ_NOT_RECEIVING,
              QString("未接收 → 判定 PDQ_NOT_RECEIVING"), decName(s.decision()));
        check(s.onTerminalStatus() == false && s.started.empty(),
              QString("未接收时终态事件不出队（started 为空）"));
        check(s.queue.size() == 2, QString("队列原样保留 2 个"), join(s.queue));
        check(pendingQueueFrozen(false, false) == true,
              QString("pendingQueueFrozen(未接收)=true（UI 弹窗据此提示已冻结）"));
        check(pendingDequeueDecisionText(PDQ_NOT_RECEIVING) != nullptr,
              QString("判定结果有可读原因（日志/界面提示用）：%1")
                  .arg(QString::fromUtf8(pendingDequeueDecisionText(PDQ_NOT_RECEIVING))));
    }

    // ── ② 已点「结束任务」→ 冻结（缺陷本体）──
    std::printf("\n== ② 已点「结束任务」→ 冻结（H8 成功让本波次变已完结也不出队）==\n");
    {
        SessionModel s;
        s.onStartReceive();                    // 接收中（当前波次 test-001 作业中）
        s.queue = { "PP", "T2", "T3" };
        s.onEndTask();                         // 点「结束任务」→ sendEnd()
        check(s.decision() == PDQ_END_REQUESTED,
              QString("已点结束任务 → 判定 PDQ_END_REQUESTED"), decName(s.decision()));
        s.onTerminalStatus();                  // H8 成功 → test-001 变已完结（旧实现此刻执行 PP）
        check(s.started.empty(),
              QString("终态回调**未**执行任何排队波次（旧实现会执行 PP = 现场缺陷）"),
              join(s.started));
        check(!s.queue.empty() && s.queue.front() == "PP",
              QString("队首仍是 PP（现场最早下发的波次，不得跳过）"), join(s.queue));
        s.onStopReceive();                     // H8 回调走完 → 停止接收（界面显示「开始接收任务」）
        check(join(s.queue) == "PP,T2,T3", QString("停止接收后队列顺序不变：PP,T2,T3"), join(s.queue));
        check(s.startedWhileStopped.empty(),
              QString("全程没有波次在「界面显示开始接收任务」时被执行"), join(s.startedWhileStopped));
    }

    // ── ③ 接收中且未点结束任务 → 放行（H5 取消自动接替下一波：既有能力保留）──
    std::printf("\n== ③ 接收中且未点结束任务 → 放行（H5 取消自动接替下一波，既有能力保留）==\n");
    {
        SessionModel s;
        s.onStartReceive();
        s.queue = { "W2" };
        check(s.decision() == PDQ_ALLOW, QString("接收中未点结束 → 判定 PDQ_ALLOW"), decName(s.decision()));
        check(s.onTerminalStatus() == true && !s.started.empty() && s.started.back() == "W2",
              QString("接收中 W1 被取消(H5) → 自动开始 W2"), join(s.started));
        check(s.queue.empty(), QString("出队后队列为空"), join(s.queue));
    }

    // ── ④ 端到端时序（现场复现路径）：不跳号、不重复、冻结时绝不执行 ──
    std::printf("\n== ④ 现场时序复现：4 个排队波次依次执行，顺序必须与下发顺序一致 ==\n");
    {
        SessionModel s;
        s.onStartReceive();                                  // 点「开始接收任务」
        const std::vector<QString> issued = { "PP", "T2", "T3", "T4" };   // WMS 依次下发
        s.queue = { "PP", "T2", "T3", "T4" };                // 当前波次 test-001 作业中 → 全部排队

        // 现场路径：点「结束任务」→ H8 成功（本波次已完结）→ 收尾停止接收
        s.onEndTask();
        s.onTerminalStatus();
        s.onStopReceive();

        // 此后每轮：「开始接收任务」（执行队首）→「结束任务」→ H8 成功 → 停止接收
        while (!s.queue.empty())
        {
            s.onStartReceive();
            s.onEndTask();
            s.onTerminalStatus();
            s.onStopReceive();
        }

        check(s.started.size() == issued.size(),
              QString("共执行 %1 个排队波次（WMS 下发 %2 个）")
                  .arg(issued.size()).arg(issued.size()),
              QString("%1 个：%2").arg(s.started.size()).arg(join(s.started)));
        bool sameOrder = (s.started.size() == issued.size());
        for (size_t i = 0; sameOrder && i < issued.size(); ++i)
            sameOrder = (s.started[i] == issued[i]);
        check(sameOrder, QString("执行顺序与下发顺序完全一致：PP,T2,T3,T4（**无跳波次**）"),
              join(s.started));
        check(s.startedWhileStopped.empty(),
              QString("全程无波次在「未点开始接收」时被执行（现场症状：开关还在开始接收任务却已接收）"),
              join(s.startedWhileStopped));
    }

    // ── ⑤ 重放被拦下的回滚：必须放回队首（等价于不跳过该波次）──
    std::printf("\n== ⑤ 重放结果到达时会话已冻结 → 回滚放回**队首**（不跳号）==\n");
    {
        SessionModel s;
        s.onStartReceive();
        s.queue = { "PP", "T2" };
        s.onTerminalStatus();            // PP 出队 → 交给 ParseWorker 重放
        s.onEndTask();                   // 重放结果还没回来，操作员点了「结束任务」
        s.onReplayBlocked("PP");         // 产品侧：放回队首，等下次「开始接收任务」

        check(join(s.queue) == "PP,T2",
              QString("PP 回到队首（下次开始接收仍先执行 PP，不会跳到 T2）"), join(s.queue));

        s.onStopReceive();
        s.onStartReceive();              // 点「开始接收任务」
        check(s.started.size() == 1 && !s.started.empty() && s.started.back() == "PP",
              QString("重新开始接收 → 执行 PP（而不是 T2）"), join(s.started));
    }

    // ── ⑥ 空队列：判定为"无波次可执行"，不产生任何动作 ──
    std::printf("\n== ⑥ 空队列 → PDQ_QUEUE_EMPTY ==\n");
    {
        SessionModel s;
        s.onStartReceive();
        check(s.decision() == PDQ_QUEUE_EMPTY && s.onTerminalStatus() == false,
              QString("空队列判定 PDQ_QUEUE_EMPTY 且不出队"), decName(s.decision()));
    }

    std::printf("\n==== 结果：通过 %d 项，失败 %d 项 ====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
