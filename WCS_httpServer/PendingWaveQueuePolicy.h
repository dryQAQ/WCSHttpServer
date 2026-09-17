#pragma once
// ============================================================================
// PendingWaveQueuePolicy.h — 待执行波次队列「什么时候允许出队执行」的**纯逻辑口径**
//
// ★ 2026-09-17 现场缺陷（"跳过一个波次"）根因与结论
//
//   缺陷现象（现场原话）：
//     "多次下发不同波次任务时，点「结束任务」后系统自动接收了下一个任务波次，
//      此时开关还是「开始接收任务」；再点「开始接收任务」时又接收了下一个波次
//      —— 错误地跳过了一个波次的任务数据。"
//
//   根因（release_WcsHttpServer/log/HTTP/http.log 2026-09-17 16:47~16:48 实录）：
//     ① 16:47:00/13/19  WMS 连下 3 个波次：PP202689634819、test-002、test-003
//                      （test-001 正在作业 → 三者全部进入待执行队列）；
//     ② 16:48:10.898  操作员点「结束任务」→ 发 H8；
//     ③ 16:48:10.941  H8 成功 → test-001 置**已完结(FINISHED)**；
//     ④ 16:48:10.984  **终态回调立刻把队首 PP202689634819 重放注册**
//                      —— 此刻接收尚未收尾（stopReceive 要等 H8 回调走完才执行，
//                         m_receiving 仍为 true），于是界面显示「开始接收任务」，
//                         系统却已经接收并注册了下一波（缺陷本体）；
//     ⑤ 16:48:22.27x  操作员点「开始接收任务」→ startReceive() 先把上一步被误注册的
//                      PP202689634819 **切出**（数据留在 DB，但不再执行），
//                      再执行队首 test-002 —— **PP 这一波从此被跳过**。
//
//   现场要求（本次口径）：**只有点了「开始接收任务」，才开始接收波次队列里的任务。**
//
//   因此本文件把"能否出队"收敛成一条可测的规则：
//     允许出队  ==  接收中（本次接收会话已开启） 且  本次会话内没点过「结束任务」
//     · 未接收（未点开始 / 已收尾停止）        → 冻结：队列原样保留，等下次「开始接收任务」；
//     · 已点「结束任务」（H8 回传等待中）      → 冻结：即使波次此刻走到终态也不出队；
//     · 接收中且未点过「结束任务」             → 放行：终态（已完结 / H5 已取消）后可接替下一波。
//
//   刻意保留的能力：**接收中**波次被 WMS 取消（H5）→ 自动接替下一波
//   （test/e2e_pending_replay.py 依赖该链路）；被冻结的只是"接收会话未开启 / 已要求结束"这一段。
//
// 使用方：
//   · 产品侧 HttpServer::maybeStartPendingWave()（出队判据）与
//     HttpServer::isPendingQueueFrozen()（UI「查看接收波次队列」弹窗冻结提示）；
//   · 测试侧 tests/test_pending_queue_gate.cpp（按现场时序逐条断言"不跳波次"）。
// ============================================================================

// ──── 出队判定结果（同时作为日志用语，现场可据此定位"为什么没执行下一波"）────
enum PendingDequeueDecision
{
    PDQ_ALLOW = 0,       // 允许出队：接收中且本次会话未点过「结束任务」
    PDQ_QUEUE_EMPTY,     // 无波次可执行（待执行队列为空）
    PDQ_NOT_RECEIVING,   // 冻结：未接收任务（未点「开始接收任务」，或已收尾停止）
    PDQ_END_REQUESTED,   // 冻结：已点「结束任务」（完结回传等待中/本次会话已要求结束）
};

// receiving     : HttpServer::m_receiving（接收层是否开启）
// endRequestBody: HttpServer::m_endRequested（本次接收会话内是否点过「结束任务」）
// queueSize     : 待执行队列长度
inline PendingDequeueDecision pendingDequeueDecision(bool receiving, bool endRequestBody, int queueSize)
{
    if (queueSize <= 0)  return PDQ_QUEUE_EMPTY;
    if (!receiving)      return PDQ_NOT_RECEIVING;
    if (endRequestBody)  return PDQ_END_REQUESTED;
    return PDQ_ALLOW;
}

// ──── 判定结果 → 中文短语（日志/UI 提示用；不改动判定本身）────
inline const char* pendingDequeueDecisionText(PendingDequeueDecision d)
{
    switch (d)
    {
    case PDQ_ALLOW:          return "放行（接收中且未点结束任务）";
    case PDQ_QUEUE_EMPTY:    return "队列为空";
    case PDQ_NOT_RECEIVING:  return "冻结（当前未接收任务）";
    case PDQ_END_REQUESTED:  return "冻结（本次会话已点「结束任务」）";
    default:                 return "未知";
    }
}

// ──── 队列是否处于冻结态（UI 提示用；与出队判据互为反面）────
inline bool pendingQueueFrozen(bool receiving, bool endRequestBody)
{
    return !(receiving && !endRequestBody);
}
