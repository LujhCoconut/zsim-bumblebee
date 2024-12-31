# Zsim-Bumblebee [Current Version v1.0.1]

This project aims to reproduce the **DAC'2023** paper `"Bumblebee: A MemCache Design for Die-stacked and Off-chip Heterogeneous Memory Systems"` using ZSim. The project is implemented based on the open-source Banshee project. 

The link to the open-source Banshee project is `https://github.com/yxymit/banshee`.

## v1.0.1 (Current Version)
The second version of Zsim-Bumblebee is released. The project is implemented based on the open-source Banshee project. The following changes are made:
1. Fixed the issue with outliers of `SL`; the root cause was the repeated calculation of parameters related to hotTracker in our implementation.
2. Made some adjustments to the hyperparameters to increase the likelihood of the first requests being served by HBM.

## v1.0.0
The first version of Zsim-Bumblebee is released. The project is implemented based on the open-source Banshee project.
