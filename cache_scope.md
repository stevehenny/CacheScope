# CacheScope Report

## Metadata
- Binary: ./build/src/test/false_share
- Event: ibs_op
- Sample period: 10000

## Sample Statistics
- Total samples: 995511
- Samples with address: 465888
- Samples with IP: 995511
- Samples with SP: 0
- Samples with BP: 0
- Unique threads: 5
- Unique CPUs: 1

## False Sharing Analysis

### Summary
| # | Base Address   | Samples | Reads | Writes | Threads |
|---|----------------|---------|-------|--------|---------|
| 1 | 0x6266cd543000 | 45365   | 45365 | 0      | 4       |

### Offsets
| # | Distinct Offsets | Shared Offsets | Private Fraction | Top Offsets |
|---|------------------|----------------|------------------|-------------|
| 1 | 4                | 0              | 1.00             | 4           |

### Bounce
| # | Thread Switches | Bounce Score |
|---|-----------------|--------------|
| 1 | 34321           | 0.757        |

### Address Range
| # | Min Address    | Max Address    | Range Bytes |
|---|----------------|----------------|-------------|
| 1 | 0x6266cd543030 | 0x6266cd54303c | 12          |
