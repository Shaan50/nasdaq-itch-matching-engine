| variant | what changed | best (M msg/s) | median | ns/msg | vs naive |
|---|---|---:|---:|---:|---:|
| `parse` | decode only (no book): upper bound | 160.12 | 137.26 | 6.2 | — |
| `naive` | std::map levels + std::list FIFO + std::unordered_map | 2.44 | 2.07 | 410.2 | 1.0x |
| `std-index` | pooled 32-B orders, intrusive FIFO, SoA ladder; std::unordered_map index | 5.37 | 4.57 | 186.1 | 2.2x |
| `flat-murmur-linear` | + open-addressing index (murmur, linear probing, 4M slots) | 8.61 | 8.47 | 116.1 | 3.5x |
| `flat-ident-linear` | identity hash, 16M slots, linear probing | 10.95 | 10.30 | 91.3 | 4.5x |
| `flat-murmur` | murmur, Robin Hood, 4M slots | 8.65 | 8.19 | 115.6 | 3.5x |
| `flat-ident-4M` | identity, Robin Hood, 4M slots (clustering pathology) | 3.04 | 2.82 | 328.8 | 1.2x |
| `flat-ident` | identity, Robin Hood, 16M slots (default) | 11.01 | 10.91 | 90.8 | 4.5x |
| `batched-ident` | + decode-ahead batches, staged prefetch (index, order, level, book, ladder) | 16.74 | 14.82 | 59.8 | 6.9x |
| `pipeline-ident` | + parser thread → SPSC ring → book thread | 18.44 | 16.39 | 54.2 | 7.6x |

Section numbers below come from the A/B session in which each change was made. Absolute
throughput drifts between sessions on this shared host, so compare ratios within a table, not
across tables.
