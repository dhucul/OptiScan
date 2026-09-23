# C1 observed-rate policy

All C1 assessments use `ScanQuality::RateC1` in `ScanQualityRating.cpp`.
This applies to Q-Check, BLER, hardware fallbacks from C2/BLER, Disc Rot,
Disc Balance speed samples, per-track BLER summaries, graphs and CSV reports.

| Observed C1 rate (errors/sec) | Label |
| --- | --- |
| 0 to below 5 | EXCELLENT |
| 5 to below 50 | GOOD |
| 50 to below 220 | FAIR |
| 220 or above | POOR |
| Missing, unverified or invalid measurement | NOT RATED |

These are **OptiScan descriptive bands**, not an industry grading scale. The
5/sec boundary is an application convention. The 50/sec and 220/sec reference
values have context in preservation guidance, but that does not validate these
particular labels, a predicted disc lifetime, or the measurement accuracy of a
consumer drive. No label certifies archival suitability or a correct rip.

## Evidence and limits

- [IASA TC-04 section 8.1.9](https://www.iasa-web.org/tc04/errors-life-expectancy-and-testing-and-analysis)
  describes BLER using a 10-second measuring period at the standard playback
  data rate and a 220/sec reference. Its stricter archival CD-R guidance lists
  average BLER below 10, peak BLER below 50, and additional E22/E32, burst and
  jitter requirements. OptiScan does not implement that complete assessment.
- [Canadian Conservation Institute, Notes 19/1](https://www.canada.ca/en/conservation-institute/services/conservation-preservation-publications/canadian-conservation-institute-notes/longevity-recordable-cds-dvds.html)
  discusses the 220 BLER limit and a preferred initial CD-R error rate below 50.
  This guidance is not a source for OptiScan's four named bands.

OptiScan uses the **average C1 rate** to explain whether the total count is low
or high for the amount of audio measured. The total line includes that assessment,
the measured audio duration and the average rate. For example:
"Total C1 observed: 11832 - EXCELLENT for 54:24.733 of measured audio (3.62/sec)."
This is the same average-rate assessment shown below the total; more audio can
accumulate more C1 counts. If measurement or timing is unverified, the report
explains why the total cannot be rated. Coverage of the requested range is
displayed separately.
The mean is total observed C1 divided by **covered sectors / 75**. It never uses
host elapsed time or the number of polls as a substitute for disc duration.

The host-driven Lite-On and Pioneer readers attach their actual interval
length, including shortened final intervals. Lite-On head-read failures leave
duration unknown, and a failed interval-counter reset ends the scan rather
than carrying accumulated counts into another interval. READ CD counts only successful
C1 observations; failed sectors and non-audio gaps do not dilute the average.
Startup exclusions for untimed responses and skipped invalid vendor responses reduce reported
coverage; explicitly timed intervals are retained from the start. Duplicate/overlapping or reversed intervals invalidate rate timing.
Classic asynchronous Plextor and newer Lite-On responses do not currently
establish counter-interval duration, so raw counts remain available but C1
counts-per-sample graphs remain available while rates, coverage and the associated grade are unavailable.

The sustained diagnostic uses three **complete contiguous measured seconds**.
Partial intervals contribute to the mean and peak interval rate, but counts
are never proportionally split to manufacture one-second observations. The
worst observed 10-second average uses complete contiguous windows aligned to
sample boundaries; it is unavailable without 750 covered contiguous sectors.
It is a local diagnostic, **not a Red Book compliance test**.

The average is graded; sustained activity and the raw peak are shown as separate measurements. Raw peaks and percentiles remain visible. Brief excursions are not
automatically attributed to the drive; they may still matter. Recorded early
spikes are no longer deleted based on their size. Untimed hardware responses still use a
three-response startup warmup; explicitly timed intervals are retained.

Graphs use absolute bands: below 50 green, 50 through 219 yellow, and 220 or
higher red. The green range combines EXCELLENT and GOOD. Rescaling the chart
does not change a value's colour. The 220 line is a reference, not a PASS/FAIL
test. C1 counts do not measure remaining C2 correction capacity.

Scan speed and drive/firmware can affect counts. C1 labels describe the observed
rate at every speed; poor readings are no longer hidden solely because a drive
ran fast. Speed cautions accompany the results. The existing 8x/16x caution
bands and separate E22 diagnostic gate are application heuristics.

Disc Rot uses the shared FAIR/POOR C1 boundaries alongside its own zone-pattern
and reread-consistency risk model. That model does not diagnose chemical rot.
Disc Balance compares speed-dependent changes, timing and read stability; its
C1 labels apply only to the sampled region, not the whole disc. These menu
items answer different questions, so their overall risk/balance scores remain
distinct from C1 rate labels. C2 pointer counts and vendor decoder counters
also have different meanings; their other diagnostics remain separate.

Missing C1 data never establishes an excellent disc. Independently observed
read failures or C2/CU activity remain reportable. A combined quality result
cannot be better than a measured C1 rating merely because it took a C2 branch.

## Comprehensive assessment and failure precedence

The Comprehensive Scan caps its score using the same **average C1 band**:
EXCELLENT permits up to 100/A, GOOD up to 89/B, FAIR up to 79/C, and POOR
up to 59/F. Other scan findings may lower the score further. These caps are
an OptiScan reporting policy, not an industry standard or a life prediction.
Missing/unverified C1 or C2 makes the grade INCOMPLETE and caps the contextual
score at 79. Confirmed read failure or uncorrectable data takes precedence:
the grade is F, and BLER CSV summaries retain BAD even with missing channels.

Host-driven LiteOn samples now report interval **start** positions consistently,
including the final full or partial interval. Reaching the end is represented
by the completion flag; it no longer changes the coordinate convention and
cannot turn a final full interval into a false sampling gap.

## Graph positions and quiet balance samples

C1 bar graphs and hardware-scan heatmaps bucket intervals by their actual
LBA span over the requested disc range. A retained sample index is not a time
coordinate. Missing columns display `?`; partly measured columns display `~`.
Observed zero counts remain distinct from missing data. Screen-reader graphs
use the original interval's peak position and the weighted measured average,
not averages of pixel-column maxima. Nonzero scan starts and partial seconds
are retained in axis labels.

A completed, correctly timed balance sample is valid even when every counter
is zero. The sweep continues to later speeds; a flat or entirely quiet sweep
is assessed after collection. Failed, incomplete or untimed captures remain
excluded. Reports with no C1 observations show an unavailable raw peak, never
an invented zero peak.
