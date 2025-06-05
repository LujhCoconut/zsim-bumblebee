#include "page_table.h"
#include <cstring>
#include <cstdlib>
#include <cassert>

/**
 * @brief 构造函数，初始化页表和PFN管理结构
 * @param max_pfn 最大支持的PFN数量
 */
PageTable::PageTable(uint64_t max_pfn_)
    : max_pfn(max_pfn_) {
    // 顶层L3页表分配512个指针，初始化为nullptr
    l3_table = new PTLevel2[PT_LEVEL_ENTRIES]();
    // PFN使用标记数组，初始化全部为false
    pfn_in_use = new bool[max_pfn]();
    // 反向映射数组，PFN->VA，初始化为0
    pfn_to_va_map = new uint64_t[max_pfn]();
    // 下一个分配的PFN编号，从1开始（0为无效）
    next_free_pfn = 1;
}

/**
 * @brief 析构函数，释放所有分配的页表和管理内存
 */
PageTable::~PageTable() {
    // 逐层释放页表子结构
    for (int i = 0; i < PT_LEVEL_ENTRIES; ++i) {
        PTLevel2 l2 = l3_table[i];
        if (l2) {
            for (int j = 0; j < PT_LEVEL_ENTRIES; ++j) {
                PTLevel1 l1 = l2[j];
                if (l1) {
                    free_level1(l1);  // 释放L1页表
                }
            }
            free_level2(l2);  // 释放L2页表
        }
    }
    // 释放顶层L3页表和管理数组
    delete[] l3_table;
    delete[] pfn_in_use;
    delete[] pfn_to_va_map;
}

/**
 * @brief 映射虚拟地址到一个新的PFN
 * @param va 虚拟地址
 * @return 分配的PFN编号
 */
PageTable::PFN PageTable::map_page(uint64_t va) {
    // 分配一个新的PFN（循环替换策略）
    PFN pfn = allocate_pfn();

    // 根据虚拟地址计算3级索引
    uint64_t i3 = l3_index(va);
    uint64_t i2 = l2_index(va);
    uint64_t i1 = l1_index(va);

    // 如果对应的L2页表不存在，先分配
    if (!l3_table[i3]) {
        l3_table[i3] = new PTLevel1[PT_LEVEL_ENTRIES]();
    }
    // 如果对应的L1页表不存在，先分配
    if (!l3_table[i3][i2]) {
        l3_table[i3][i2] = new PFN[PT_LEVEL_ENTRIES]();
    }

    // 设置L1页表项为新分配的PFN
    l3_table[i3][i2][i1] = pfn;

    // 更新反向映射PFN->VA
    pfn_to_va_map[pfn] = va;

    return pfn;
}

/**
 * @brief 解除虚拟地址的映射关系，释放对应PFN
 * @param va 虚拟地址
 * @return 成功返回true，地址未映射返回false
 */
bool PageTable::unmap_page(uint64_t va) {
    // 计算3级索引
    uint64_t i3 = l3_index(va);
    uint64_t i2 = l2_index(va);
    uint64_t i1 = l1_index(va);

    PTLevel2 l2 = l3_table[i3];
    if (!l2) return false;

    PTLevel1 l1 = l2[i2];
    if (!l1) return false;

    PFN old_pfn = l1[i1];
    if (old_pfn == 0) return false;  // 没有映射

    // 清除页表项
    l1[i1] = 0;

    // 标记PFN可用，清空反向映射
    pfn_in_use[old_pfn] = false;
    pfn_to_va_map[old_pfn] = 0;
    return true;
}

/**
 * @brief 查询虚拟地址对应的PFN
 * @param va 虚拟地址
 * @param out_pfn 输出PFN编号（若返回true则有效）
 * @return 找到返回true，否则false
 */
bool PageTable::lookup_pfn(uint64_t va, PFN &out_pfn) const {
    // 计算索引
    uint64_t i3 = l3_index(va);
    uint64_t i2 = l2_index(va);
    uint64_t i1 = l1_index(va);

    PTLevel2 l2 = l3_table[i3];
    if (!l2) return false;

    PTLevel1 l1 = l2[i2];
    if (!l1) return false;

    out_pfn = l1[i1];
    return out_pfn != 0;
}

/**
 * @brief 分配一个新的PFN，循环覆盖旧PFN的映射（不抛异常）
 * @return 新分配的PFN编号
 */
PageTable::PFN PageTable::allocate_pfn() {
    PFN candidate = next_free_pfn;

    // 下一个待分配PFN指针移动
    next_free_pfn++;
    if (next_free_pfn >= max_pfn) {
        next_free_pfn = 1;  // 循环回起点，0为无效
    }

    if (pfn_in_use[candidate]) {
        // 如果该PFN已经被占用，则解除其旧映射
        uint64_t old_va = pfn_to_va_map[candidate];
        unmap_page(old_va);
    }

    // 标记PFN为已占用
    pfn_in_use[candidate] = true;
    return candidate;
}


PageTable::PFN PageTable::get_or_map_page(uint64_t va) {
    PFN existing;
    if (lookup_pfn(va, existing)) {
        return existing;
    }
    return map_page(va);  // 没有则分配新页
}

// -----------------------------------
// 以下为页表索引辅助函数和内存释放函数
// -----------------------------------

/**
 * @brief 计算L3页表索引
 * @param va 虚拟地址
 * @return L3索引
 */
inline uint64_t PageTable::l3_index(uint64_t va) {
    return (va >> (PAGE_SHIFT + 2 * PT_LEVEL_BITS)) & (PT_LEVEL_ENTRIES - 1);
}

/**
 * @brief 计算L2页表索引
 * @param va 虚拟地址
 * @return L2索引
 */
inline uint64_t PageTable::l2_index(uint64_t va) {
    return (va >> (PAGE_SHIFT + PT_LEVEL_BITS)) & (PT_LEVEL_ENTRIES - 1);
}

/**
 * @brief 计算L1页表索引
 * @param va 虚拟地址
 * @return L1索引
 */
inline uint64_t PageTable::l1_index(uint64_t va) {
    return (va >> PAGE_SHIFT) & (PT_LEVEL_ENTRIES - 1);
}

/**
 * @brief 释放L1页表数组
 * @param l1 L1页表指针
 */
void PageTable::free_level1(PTLevel1 l1) {
    delete[] l1;
}

/**
 * @brief 释放L2页表数组
 * @param l2 L2页表指针
 */
void PageTable::free_level2(PTLevel2 l2) {
    delete[] l2;
}