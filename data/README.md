# Data

The NASDAQ TotalView-ITCH 5.0 sample files are public but large, so they are not
committed. To reproduce the results in the top-level README:

```bash
curl -O "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz"   # 3.5 GB
gunzip -k 12302019.NASDAQ_ITCH50.gz                                             # 8.25 GB
```

Every tool also reads the `.gz` directly (streaming zlib), at lower throughput.
Small slices used by the unit tests live in `tests/data/` (see `itch_slice`).
