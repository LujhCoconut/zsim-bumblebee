#ifndef PAGE_TABLE_H
#define PAGE_TABLE_H

#include <cstdint>
#include <cstddef>

/**
 * @brief 支持48位地址的三级页表，循环分配PFN，满了自动替换旧映射
 * @author Jiahao Lu @ XMU
 */
class PageTable {
public:
    using PFN = uint64_t;

    explicit PageTable(uint64_t max_pfn);
    ~PageTable();

    /**
     * @brief 映射一个新的虚拟页，分配一个新的PFN返回
     */
    PFN map_page(uint64_t va);

    /**
     * @brief 解除虚拟地址映射，释放对应PFN
     */
    bool unmap_page(uint64_t va);

    /**
     * @brief 查询 VA 所映射的 PFN，成功则返回 true，并设置 out_pfn
     */
    bool lookup_pfn(uint64_t va, PFN &out_pfn) const;

    PFN get_or_map_page(uint64_t va);

private:
    static const int PT_LEVEL_BITS = 9;                 // 每级页表512项
    static const int PT_LEVEL_ENTRIES = 1 << PT_LEVEL_BITS;
    static const int PAGE_SHIFT = 12;                   // 页大小4KB

    using PTLevel1 = PFN*;        // L1页表叶子：PFN数组
    using PTLevel2 = PTLevel1*;   // L2页表：指向L1
    using PTLevel3 = PTLevel2*;   // L3页表：指向L2

    PTLevel3 l3_table;            // 顶层页表

    // PFN管理
    const uint64_t max_pfn;       // PFN最大值
    PFN next_free_pfn = 1;        // 下一个分配的PFN（从1开始，0为无效）
    bool *pfn_in_use;             // PFN是否已被占用
    uint64_t *pfn_to_va_map;      // 反向映射：PFN -> VA

    // 地址解析
    static inline uint64_t l3_index(uint64_t va);
    static inline uint64_t l2_index(uint64_t va);
    static inline uint64_t l1_index(uint64_t va);

    // 释放页表子表
    static void free_level1(PTLevel1 l1);
    static void free_level2(PTLevel2 l2);

    // PFN分配（循环分配，满了覆盖旧映射）
    PFN allocate_pfn();
};

#endif // PAGE_TABLE_H