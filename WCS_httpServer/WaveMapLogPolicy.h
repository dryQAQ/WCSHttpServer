#pragma once
// ============================================================================
// WaveMapLogPolicy.h — 「波次 SKU→格口 映射留痕」的落行判据（纯逻辑，无 Qt / 无 IO）
//
// 为什么单独抽出来：
//   现场要求"每次下发解析出的 item 都把 SKU 对应的格口留一份记录"，
//   同时硬约束"不能影响 UI、不能影响性能、不能影响主工作流"。
//   于是留痕必须带**体积闸门**（否则 5 万件波次会往日志里写 3 万多行）。
//   闸门逻辑一旦写错（例如漏了截断、或把摘要模式写成全量），
//   就会在最大波次上把磁盘/时间吃掉，而小波次测不出来 —— 正是需要单测锁住的类型。
//
// 口径（与 ParseWorker 的调用点、tests\test_wave_map_policy.cpp 共用这一份）：
//   · 全量模式：SKU 数 ≤ limit            → 每个 SKU 落一行
//   · 摘要模式：SKU 数 >  limit            → 只落「首 tail 行 + 尾 tail 行」
//   · forceFull（环境变量 WCS_WAVE_MAP_FULL=1）：无视 limit，一律全量（现场自行承担日志量）
//   · limit ≤ 0：没有任何 SKU 满足"全量" → **退化为摘要**（而非"一行都不落"）。
//       刻意如此：一个误配置的 0 若让留痕整体静默失效，比多写首尾 60 行更难发现；
//       退化后既保留首尾可核对，又天然限流（5 万件仍只落 60 行）。
//   · skuTotal ≤ 0（空映射）：一行都不落（由调用方决定是否打摘要头/尾）
//
// 复杂度 O(1)/次判定；调用方（ParseWorker）按 const_iterator 顺序调用即按 SKU 升序。
// ============================================================================

enum WaveMapLineMode
{
    WMLM_NONE = 0,   // 该 SKU 不落行
    WMLM_HEAD,       // 摘要模式：落在"开头 tail 条"区间
    WMLM_TAIL,       // 摘要模式：落在"结尾 tail 条"区间
    WMLM_FULL,       // 全量模式：一律落行
};

// 单个 SKU 是否落行
inline WaveMapLineMode waveMapLineMode(int skuIndex, int skuTotal, int limit, int tail, bool forceFull)
{
    if (skuTotal <= 0 || skuIndex < 0 || skuIndex >= skuTotal)
        return WMLM_NONE;
    if (forceFull || skuTotal <= limit)
        return WMLM_FULL;
    if (skuIndex < tail)
        return WMLM_HEAD;
    if (skuIndex >= skuTotal - tail)
        return WMLM_TAIL;
    return WMLM_NONE;
}

// 本次实际会落多少行（供摘要行自证与单测断言）
inline int waveMapLinesWritten(int skuTotal, int limit, int tail, bool forceFull)
{
    if (skuTotal <= 0)
        return 0;
    if (forceFull || skuTotal <= limit)
        return skuTotal;
    // 首尾区间可能重叠（skuTotal ≤ 2×tail）：此时覆盖全部，不重复计数
    if (skuTotal <= 2 * tail)
        return skuTotal;
    return 2 * tail;
}

// 是否处于摘要模式（供日志文案与自证）
inline bool waveMapIsSummary(int skuTotal, int limit, bool forceFull)
{
    return (!forceFull && skuTotal > limit);
}
