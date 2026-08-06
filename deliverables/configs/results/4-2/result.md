# Experiment 4-2 -- the black box

Run: 2026-07-27T07:41:33Z

| | |
|---|---|
| rows in `detections` | 607 |
| configured ring size | 1000 |
| lifetime detection counter | see stats table |

Storage is bounded by an `AFTER INSERT` trigger that deletes anything older
than the newest `db_ring_size` rows, so the database cannot grow without limit
on a board with a 28 GB card and no log rotation for it. The running totals live
in a separate `stats` table precisely so they survive the ring -- otherwise
"how many people were seen today" would silently reset every 1000 detections.

`/api/v1/history` serves the most recent records from the same table, which is
the reporting path the brief asks for.

Schema, sample rows and the trigger definition are in `cmd.log`.
