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

OptiScan reports observed whole-scan or sampled-region means. Its sustained
diagnostic is the maximum of the minimum values in each group of three
consecutive samples. This is **not a three-second average or a ten-second BLER
test**. Where positions are supplied, gaps, duplicates and reversed positions
break persistence; fewer than three consecutive samples cannot receive a
sustained rating. Unknown timing is not converted into a standards claim.

The average and sustained diagnostic share the same bands but measure different
things. Raw peaks and percentiles remain visible. Brief excursions are not
automatically attributed to the drive; they may still matter. Recorded early
spikes are no longer deleted based on their size. Hardware scans still omit a
fixed three-response startup warmup; their output describes retained samples.

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
