# TZCITIES.DAT

Baked city/timezone table for the OOBE Date & Time page's world-map picker
(`docs/OOBE_TIME_APPEARANCE.html`, section 4). The bitmap `WORLDMAP.BMP` is
purely a visual backdrop; it carries no selection data of its own. This file
is the single source of truth the port reads to place markers, answer clicks,
and drive the searchable zone list. Both are generated, not hand-authored:
regenerate with

```
tools/assetgen/gen_worldmap.py  --out userland/apps/setup/assets/WORLDMAP.BMP
tools/assetgen/gen_tzcities.py  --out userland/apps/setup/assets/TZCITIES.DAT
```

Edit the `CITIES` list in `gen_tzcities.py`, not the `.DAT` file.

## Why this format

Parsed by a `no_std`-ish freestanding Rust/C app with no libc, no allocator
niceties beyond a fixed-size static array, and no float unit (the kernel
target is soft-float with SSE disabled; userland matches that discipline for
this data). A fixed-width binary record format needs no parser at all: read
the 16-byte header once, then index record `i` at `16 + i*100` and slice
fields by constant offset. No delimiters, no line-splitting, no heap.

## Layout

All integers little-endian (x86-64 host and target throughout, no ambiguity).

### Header, 16 bytes

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `magic` | ASCII `"TZC1"`, not NUL-terminated |
| 4 | 2 | `record_size` | u16 LE, currently `100` |
| 6 | 2 | `record_count` | u16 LE, number of records that follow |
| 8 | 8 | `reserved` | zero, reserved for future use |

### Record, 100 bytes, `record_count` of these immediately after the header

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 32 | `name` | ASCII, NUL-padded. City display name only (e.g. `"London"`), NOT `"City, Country"`. |
| 32 | 24 | `country` | ASCII, NUL-padded (e.g. `"United Kingdom"`). |
| 56 | 32 | `tz_id` | ASCII, NUL-padded IANA timezone id (e.g. `"Europe/London"`). |
| 88 | 2 | `utc_off_min` | i16 LE. **Standard-time** UTC offset in minutes, east positive (e.g. `+330` for UTC+05:30, `-480` for UTC-08:00). Same convention as the existing `TZ[]` array in `userland/apps/setup/main.rs`: this is the zone's fixed standard offset, not whatever is in effect today. |
| 90 | 1 | `dst` | u8. `1` if the zone observes a +60min DST shift at some point in the year, `0` if it never does. One bit of information only; when/how much is out of scope for a first-boot picker. |
| 91 | 1 | `reserved0` | zero |
| 92 | 2 | `lat_e2` | i16 LE. Latitude in degrees * 100, north positive. Range -9000..9000. |
| 94 | 2 | `lon_e2` | i16 LE. Longitude in degrees * 100, east positive. Range -18000..18000. |
| 96 | 4 | `reserved1` | zero |

String fields are zero-padded to their full width but not guaranteed
zero-terminated exactly at the used length beyond the first NUL; a reader
should `strnlen` up to the field width, same as reading any fixed C char
array.

## Fixed-point lat/lon, not float

The kernel target is soft-float with SSE disabled (`x86_64-unknown-none`,
`-mno-sse -mno-sse2`; see `CLAUDE.md`). Latitude/longitude are stored as
degrees * 100, signed 16-bit integers, so the userland reader can match that
discipline and never needs a float unit either.

Precision: 0.01 degree is about 1.1km at the equator. `WORLDMAP.BMP` is
352x138 pixels for the full -180..180 / -90..90 sphere, i.e. about 1.02
degrees per pixel horizontally. The stored precision is roughly 100x finer
than a single map pixel, so it is never the limiting factor on marker
placement; the bitmap's own pixel grid is.

## Projection (must match the port exactly)

Equirectangular (Plate Carree), applied with the page-relative map box from
`docs/OOBE_TIME_APPEARANCE.html` section 2 row 5 (`MAP_X=32, MAP_Y=86,
MAP_W=352, MAP_H=138`):

```
marker_x = MAP_X + (lon + 180) / 360 * MAP_W
marker_y = MAP_Y + (90 - lat) / 180 * MAP_H
```

Greenwich (lon 0) lands at exactly half the map's width by construction:
`(0 + 180) / 360 * 352 = 176.0 = 352 / 2`.

## City selection (51 cities, all 35 shipping offsets covered)

The wizard's `TZ[]` array (`userland/apps/setup/main.rs`) ships a 35-entry
list of UTC offsets. `TZCITIES.DAT`'s `utc_off_min` values are constrained
to exactly that offset set (`gen_tzcities.py` fails closed if a city uses an
offset outside it, or if any offset has zero representatives) so the map
picker and the shipping offset list can never silently drift apart.

**#745 (user-reported 2026-08-12), corrected the entry this section used to
carry.** `TZ[]` used to stop at 26 entries (UTC-12:00..UTC+10:00, three
non-hourly steps at +03:30/+05:30/+05:45) and this section said extending it
was "a separate, larger change out of scope here." That change is this
revision: 9 more offsets were APPENDED to `TZ[]`/`TZ_OFF_MIN[]` at indices
26..34 (not inserted in UTC-sorted position - see the block comment above
`TZ[]` in main.rs for why: index 12 is hardcoded elsewhere in that file as
"the" UTC+00:00 default, and inserting the two new negative offsets ahead of
it would have silently redefined that constant). The 9 new offsets, each
with at least one new city: UTC+09:30 (Adelaide - the reported bug -, and
Darwin, to show DST really does differ at a shared offset), UTC+10:30 (Lord
Howe Island), UTC+11:00 (Honiara), UTC+12:00 (Auckland - New Zealand had NO
selectable zone at all before this), UTC+12:45 (Waitangi/Chatham Islands,
the only quarter-hour offset that exists anywhere), UTC+13:00 (Apia),
UTC+14:00 (Kiritimati), UTC-03:30 (St. John's/Newfoundland), UTC-09:30
(the Marquesas). Brisbane and Melbourne were also added at the EXISTING
UTC+10:00 offset (the report's other two named cities; that offset already
existed, this was a pure city-table addition, no offset-table change), with
real, differing dst flags at that shared offset too: Brisbane/Queensland has
never observed DST, Melbourne/Victoria does.

Every one of the 35 offsets gets at least one representative city. Offsets
covering large populations or landmass (the Americas' business-hours belt,
Western Europe/West Africa, East/South/Southeast Asia, and now south-east
Australia) get 2-3, chosen for global recognisability or, for the three
Australian pairs above, to make a real same-offset DST divergence visible in
the picker. Offsets that are inherently rare (UTC+03:30 = Iran only,
UTC+05:45 = Nepal only, UTC+12:45 = Chatham Islands only) get exactly one,
because that is genuinely all there is.

At 352x138 ten pairs of cities sit under 10px apart (halo diameter is 13px):
London/Paris, Tehran/Dubai, Karachi/Mumbai, Kathmandu/Dhaka, Pago Pago/Apia,
and five new pairs among Adelaide/Sydney/Brisbane/Melbourne/Lord Howe Island
(south-east Australia genuinely is that crowded with required offsets at
this map scale). Each pair is two DISTINCT required offsets, or in the
Brisbane/Melbourne/Adelaide case two cities the report specifically asked
to be distinguishable, that happen to be geographically adjacent in real
life; removing any of them would either drop coverage of a required offset
or reintroduce the exact bug this file exists to fix, so the crowding is
accepted as genuine geography, not overcrowding from redundant entries.
Every other redundant secondary city that caused a collision (Phoenix,
Athens, Riyadh, Tashkent, Jakarta, Seoul, Vladivostok) was pruned, since
those offsets already had a representative elsewhere.

## Fallback

If `TZCITIES.DAT` or `WORLDMAP.BMP` fail to load, the page degrades per
`docs/OOBE_TIME_APPEARANCE.html` section 4: the map box renders as a flat
`#122420` panel (identical to the ocean fill, reads as "recessed panel", not
a broken image) and the search/list panel, which is never dependent on the
bitmap, remains the primary, always-available selection path.
