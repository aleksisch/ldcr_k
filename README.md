# CFL reachability for C++

This project explores pointer analysis for C++ through context-free-language
(CFL) reachability. Program statements become labeled graph edges, and paths
accepted by the analysis languages describe how pointers can reach objects.

The goal is to implement field- and call-site-sensitive analysis with on-the-fly
call-graph construction, including receiver-sensitive virtual dispatch. We aim
to port the L_DC and L_DCR formulations to C++ and compare their precision and
cost with kCFA.

LLVM and SVF provide the program representation and frontend infrastructure.
Andersen analysis supplies initial call-target candidates and a
context-insensitive baseline; the CFL analysis is implemented in this project.
