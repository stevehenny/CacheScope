# CacheScope Report

## Metadata
- Binary: build/src/test/false_share
- Event: ibs_op
- Sample period: 1000

## Sample Statistics
- Total samples: 169793
- Samples with address: 75774
- Samples with IP: 169793
- Samples with SP: 0
- Samples with BP: 0
- Unique threads: 5
- Unique CPUs: 1

## False Sharing Analysis

### Summary
| # | Base Address   | Samples | Reads | Writes | Threads |
|---|----------------|---------|-------|--------|---------|
| 1 | 0x62b5cefbc000 | 6951    | 6951  | 0      | 4       |

### Offsets
| # | Distinct Offsets | Shared Offsets | Private Fraction | Top Offsets |
|---|------------------|----------------|------------------|-------------|
| 1 | 4                | 0              | 1.00             | 4           |

### Bounce
| # | Thread Switches | Bounce Score |
|---|-----------------|--------------|
| 1 | 2557            | 0.368        |

### Address Range
| # | Min Address    | Max Address    | Range Bytes |
|---|----------------|----------------|-------------|
| 1 | 0x62b5cefbc030 | 0x62b5cefbc03c | 12          |
