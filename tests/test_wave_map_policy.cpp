// ============================================================================
// test_wave_map_policy.cpp — 「波次 SKU→格口 映射留痕」体积闸门回归测试
//
// 直接 #include 产品侧 WCS_httpServer\WaveMapLogPolicy.h（同一份判据）：
// 若有人把闸门改回"无上限逐 SKU 落行"，第 ③④⑤ 项立刻失败。
//
// 现场背景：新增的留痕写 log/WAVE_MAP/wave_map.log；硬约束是
//   "不能影响 UI / 性能 / 主工作流" —— 所以超大波次必须只落摘要。
// ============================================================================
#include <cstdio>
#include <cstring>
#include "WaveMapLogPolicy.h"

static int g_fail = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (cond) { printf("  [OK]   %s\n", msg); }                          \
        else      { printf("  [FAIL] %s\n", msg); ++g_fail; }                \
    } while (0)

// 模拟 ParseWorker 的循环：给定 SKU 总数，返回实际落行数
static int simulate(int skuTotal, int limit, int tail, bool forceFull)
{
    int written = 0;
    for (int i = 0; i < skuTotal; ++i)
        if (waveMapLineMode(i, skuTotal, limit, tail, forceFull) != WMLM_NONE)
            ++written;
    return written;
}

int main()
{
    const int LIMIT = 5000;   // = define.h WAVE_MAP_LOG_MAX_SKU
    const int TAIL  = 30;     // = define.h PARSE_SKU_LOG_TAIL

    printf("== ① 小波次（现场日常 29~132 SKU）：必须逐 SKU 全量落行 ==\n");
    CHECK(simulate(29, LIMIT, TAIL, false) == 29, "29 SKU -> 29 行");
    CHECK(simulate(132, LIMIT, TAIL, false) == 132, "132 SKU -> 132 行");
    CHECK(waveMapLineMode(0, 29, LIMIT, TAIL, false) == WMLM_FULL, "首行全量");
    CHECK(waveMapLineMode(28, 29, LIMIT, TAIL, false) == WMLM_FULL, "末行全量");
    CHECK(!waveMapIsSummary(29, LIMIT, false), "不处于摘要模式");

    printf("== ② 边界：SKU 数 == limit 仍全量；limit+1 转摘要 ==\n");
    CHECK(simulate(LIMIT, LIMIT, TAIL, false) == LIMIT, "SKU==limit -> 全量 limit 行");
    CHECK(waveMapLineMode(LIMIT - 1, LIMIT, LIMIT, TAIL, false) == WMLM_FULL, "SKU==limit 末行全量");    CHECK(waveMapIsSummary(LIMIT + 1, LIMIT, false), "SKU==limit+1 处于摘要模式");

    printf("== ③ 4,114 件真实大波次（4,007 SKU）==\n");
    // 现场 PP202600000593：4,007 SKU / 4,114 计划行 -> 未超闸门，应全量
    CHECK(simulate(4007, LIMIT, TAIL, false) == 4007, "4007 SKU -> 4007 行（未超闸门，全量）");

    printf("== ④ 5 万件波次（约 32,000 SKU）：必须只落摘要，绝不能 32,000 行 ==\n");
    const int SKU_50K = 32000;
    int w = simulate(SKU_50K, LIMIT, TAIL, false);
    CHECK(w == 2 * TAIL, "32000 SKU -> 仅首尾各 30 行 = 60 行");
    CHECK(w < SKU_50K, "落行数远小于 SKU 数");
    CHECK(waveMapIsSummary(SKU_50K, LIMIT, false), "处于摘要模式");
    CHECK(waveMapLineMode(0, SKU_50K, LIMIT, TAIL, false) == WMLM_HEAD, "第 0 行在首区间");
    CHECK(waveMapLineMode(100, SKU_50K, LIMIT, TAIL, false) == WMLM_NONE, "中间行不落");
    CHECK(waveMapLineMode(SKU_50K - 1, SKU_50K, LIMIT, TAIL, false) == WMLM_TAIL, "末行在尾区间");
    // 不变量：首尾区间都不落空（第一行与最后一行必须留痕，便于人工核对波次内容）
    CHECK(waveMapLineMode(0, SKU_50K, LIMIT, TAIL, false) != WMLM_NONE, "首个 SKU 一定留痕");
    CHECK(waveMapLineMode(SKU_50K - 1, SKU_50K, LIMIT, TAIL, false) != WMLM_NONE, "末个 SKU 一定留痕");

    printf("== ⑤ 环境变量强制全量 WCS_WAVE_MAP_FULL=1 ==\n");
    CHECK(simulate(SKU_50K, LIMIT, TAIL, true) == SKU_50K, "强制全量 -> 32000 行");
    CHECK(!waveMapIsSummary(SKU_50K, LIMIT, true), "强制全量时不属摘要模式");

    printf("== ⑥ 退化输入不得崩溃/越界 ==\n");
    CHECK(simulate(0, LIMIT, TAIL, false) == 0, "0 SKU -> 0 行");
    CHECK(waveMapLineMode(0, 0, LIMIT, TAIL, false) == WMLM_NONE, "空映射不落行");
    CHECK(waveMapLineMode(-1, 10, LIMIT, TAIL, false) == WMLM_NONE, "负下标不落行");
    CHECK(waveMapLineMode(10, 10, LIMIT, TAIL, false) == WMLM_NONE, "越界下标不落行");
    CHECK(waveMapLinesWritten(0, LIMIT, TAIL, false) == 0, "计数：0 SKU");
    // limit=0 的语义：没有任何 SKU 满足"全量"，→ 退化为摘要（首尾各 tail）。
    //   刻意不定义成"0 行也不落"：那样一个误配置的 0 会让留痕整体静默失效，
    //   反而更难发现；退化为摘要既保留首尾可核对，又天然限流。
    CHECK(waveMapLinesWritten(10, 0, TAIL, false) == 10, "limit=0 -> 退化摘要，10 SKU 全落在首尾区间 = 10 行");
    CHECK(waveMapLineMode(0, 10, 0, TAIL, false) != WMLM_FULL, "limit=0 绝不是全量模式");
    CHECK(simulate(32000, 0, TAIL, false) == 2 * TAIL, "limit=0 + 32000 SKU -> 仍只落首尾各 30 行（限流有效）");

    printf("== ⑦ 首尾区间重叠（SKU 数 ≤ 2×tail）不得重复计数 ==\n");
    CHECK(simulate(40, 10, 30, false) == 40, "40 SKU / limit=10 / tail=30 -> 覆盖全部 40 行");
    CHECK(waveMapLinesWritten(40, 10, 30, false) == 40, "计数与循环一致（无重复）");
    CHECK(simulate(60, 10, 30, false) == 60, "60 SKU（恰 2×tail）-> 60 行");
    CHECK(waveMapLinesWritten(61, 10, 30, false) == 60, "61 SKU -> 60 行");

    printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "[OK] all wave-map policy tests passed" : "[FAIL]", g_fail);
    return g_fail == 0 ? 0 : 1;
}
