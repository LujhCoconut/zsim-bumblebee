# Zsim-Bumblebee [Current Version v3.0.1]

This project aims to reproduce the **DAC'2023** paper `"Bumblebee: A MemCache Design for Die-stacked and Off-chip Heterogeneous Memory Systems"` using ZSim. The project is implemented based on the open-source Banshee project. 

The link to the open-source Banshee project is `https://github.com/yxymit/banshee`.

## v3.0.1(Current Version)
This version has undergone critical logic modifications, resulting in a significant performance improvement compared to the previous version.The specific changes in version 3.0.1 are as follows:
1. Fixed a critical bug in address mapping
2. added latency information specifically for handling metadata access in ddr_mem.cpp. `rd_dram_tag_latency` & `wt_dram_tag_latency`
3. Fixed a bug in Line`2311-2325` about `free_ddr`
4. Optimized the logic of tryEvict and added the `sl_state` field.
5. The status related to SL has been modified; the status of memory Type=1 or Type=2 is associated with sl_state.
6. The logic for `is_pop` has been enhanced to be associated with the HBM occupancy rate.

## v3.0.0
This version has undergone critical logic modifications, resulting in a significant performance improvement compared to the previous version.The specific changes in version 3.0.0 are as follows:
1. Fixed the bug related to the incorrect calculation of block_offset.
2. The logical organization has been optimized to enhance code locality.

## v2.0.0 
The 2.0.0 version of Zsim-Bumblebee is released. Version 2.0.0 is an unstable release with significant room for performance optimization. The specific changes in version 2.0.0 are as follows:
1. Added the impact of asynchronous migration and eviction on system traffic and overhead, and fixed the "happens-before" issue introduced by asynchronous migration and eviction in the v1.0.x versions.
2. The code structure has been optimized, and some functions have been decoupled to reduce code redundancy.

## v1.0.1 
The second version of Zsim-Bumblebee is released. The project is implemented based on the open-source Banshee project. The following changes are made:
1. Fixed the issue with outliers of `SL`; the root cause was the repeated calculation of parameters related to hotTracker in our implementation.
2. Made some adjustments to the hyperparameters to increase the likelihood of the first requests being served by HBM.

## v1.0.0
The first version of Zsim-Bumblebee is released. The project is implemented based on the open-source Banshee project.
