# CacheScope Report

## Metadata
- Binary: build/src/test/cache_thrash
- Event: ibs_op
- Sample period: 10000

## Sample Statistics
- Total samples: 63400
- Samples with address: 32481
- Samples with physical address: 32481
- Samples with IP: 63400
- Samples with SP: 0
- Samples with BP: 0
- Unique threads: 1
- Unique CPUs: 1

## False Sharing Analysis
No hot cache lines detected.

## Cache Thrashing Analysis

### Detected Cache Topology
| Level | Type    | ID | Size (KiB) | Line | Sets  | Ways | Shared CPUs | Source |
|-------|---------|----|------------|------|-------|------|-------------|--------|
| L1    | Data    | 0  | 32         | 64   | 64    | 8    | 0,12        | sysfs  |
| L1    | Data    | 1  | 32         | 64   | 64    | 8    | 1,13        | sysfs  |
| L1    | Data    | 2  | 32         | 64   | 64    | 8    | 2,14        | sysfs  |
| L1    | Data    | 3  | 32         | 64   | 64    | 8    | 3,15        | sysfs  |
| L1    | Data    | 4  | 32         | 64   | 64    | 8    | 4,16        | sysfs  |
| L1    | Data    | 5  | 32         | 64   | 64    | 8    | 5,17        | sysfs  |
| L1    | Data    | 8  | 32         | 64   | 64    | 8    | 6,18        | sysfs  |
| L1    | Data    | 9  | 32         | 64   | 64    | 8    | 7,19        | sysfs  |
| L1    | Data    | 10 | 32         | 64   | 64    | 8    | 8,20        | sysfs  |
| L1    | Data    | 11 | 32         | 64   | 64    | 8    | 9,21        | sysfs  |
| L1    | Data    | 12 | 32         | 64   | 64    | 8    | 10,22       | sysfs  |
| L1    | Data    | 13 | 32         | 64   | 64    | 8    | 11,23       | sysfs  |
| L2    | Unified | 0  | 1024       | 64   | 2048  | 8    | 0,12        | sysfs  |
| L2    | Unified | 1  | 1024       | 64   | 2048  | 8    | 1,13        | sysfs  |
| L2    | Unified | 2  | 1024       | 64   | 2048  | 8    | 2,14        | sysfs  |
| L2    | Unified | 3  | 1024       | 64   | 2048  | 8    | 3,15        | sysfs  |
| L2    | Unified | 4  | 1024       | 64   | 2048  | 8    | 4,16        | sysfs  |
| L2    | Unified | 5  | 1024       | 64   | 2048  | 8    | 5,17        | sysfs  |
| L2    | Unified | 8  | 1024       | 64   | 2048  | 8    | 6,18        | sysfs  |
| L2    | Unified | 9  | 1024       | 64   | 2048  | 8    | 7,19        | sysfs  |
| L2    | Unified | 10 | 1024       | 64   | 2048  | 8    | 8,20        | sysfs  |
| L2    | Unified | 11 | 1024       | 64   | 2048  | 8    | 9,21        | sysfs  |
| L2    | Unified | 12 | 1024       | 64   | 2048  | 8    | 10,22       | sysfs  |
| L2    | Unified | 13 | 1024       | 64   | 2048  | 8    | 11,23       | sysfs  |
| L3    | Unified | 0  | 32768      | 64   | 32768 | 16   | 0-5,12-17   | sysfs  |
| L3    | Unified | 1  | 32768      | 64   | 32768 | 16   | 6-11,18-23  | sysfs  |

### Detected Episodes
| # | Level | Type | ID | Shared CPUs | Set | Address Basis       | Samples | Lines | Evictions | Reloads | Reload Ratio | Pressure | Score | Duration (ns) |
|---|-------|------|----|-------------|-----|---------------------|---------|-------|-----------|---------|--------------|----------|-------|---------------|
| 1 | L1    | Data | 10 | 8,20        | 0   | virtual-page-offset | 2145    | 9     | 222       | 221     | 0.995        | 1.12x    | 0.560 | 630052149     |
