// ============================================================================
// ConsoleGraph.h - Unified graph renderer for the OptiScan GUI
//
// All graphs in the app route through this header.  Three primitives are
// exposed:
//
//   DrawBarGraph         - Single-series bar chart with optional reference
//                          line, severity-coloured bars, smart Y-axis labels
//                          (top / reference / midpoint / zero), and time-
//                          based X-axis with start, reference-position and
//                          end labels.
//   DrawHeatmap          - Severity ribbon: one row per metric, each cell
//                          tiered into none/low/moderate/high by absolute
//                          thresholds (used by the QCheck disc health map).
//   DrawScoreBar         - Horizontal severity-coloured score bar (used by
//                          the C2-margin headroom bar and the disc-rot zone
//                          bars).
//
// Style decisions follow modern GUI dashboards rather than the legacy CLI
// look: a thin rule above the plot, ┤ y-tick markers, └─→ x-axis, vertical
// reference connectors (╷│┴), per-bar severity gradient via ANSI truecolour,
// and a compact legend below.  ANSI codes are honoured by the RichEdit sink
// in GuiSink.cpp, so the gradient renders in the GUI.
// ============================================================================
#pragma once

#include "Accessibility.h"
#include "ConsoleColor.h"
#include "ConsoleSymbols.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace Console {

// ────────────────────────────────────────────────────────────────────────────
// Options
// ────────────────────────────────────────────────────────────────────────────

struct GraphOptions {
	std::string title;
	std::string subtitle;
	int width = 60;
	int height = 12;       // rows in the plot area
	int refLine = 0;       // value of the reference line (0 = none)
	std::string refLabel;  // label for the reference line
	bool colorize = true;  // severity gradient on the bars
	std::string unitSuffix; // e.g. "/sec" appended to y-axis values; optional
	int severityLowThreshold = 0;   // optional absolute low/moderate boundary
	int severityHighThreshold = 0;  // optional absolute moderate/high boundary
	std::string severityLowLabel = "low";
	std::string severityModerateLabel = "moderate";
	std::string severityHighLabel = "high";
};

// One row in a severity heatmap.  Each per-column value is bucketed into one
// of four tiers (none / low / moderate / high) by the two thresholds below
// and rendered as a coloured cell.  Use absolute, semantically meaningful
// thresholds rather than peak-normalised ones — that is the whole point of
// a heatmap vs. an overlay graph.
struct HeatmapRow {
	std::string label;
	std::vector<int> values;
	// Tier boundaries (semantically meaningful, not peak-normalised):
	//   v == 0                          → none      (░ dim)
	//   0  < v < lowThresh              → low       (█ green)
	//   lowThresh <= v < highThresh     → moderate  (█ yellow)
	//   v >= highThresh                 → high      (█ red)
	int lowThresh = 1;
	int highThresh = 1;
};

// ────────────────────────────────────────────────────────────────────────────
// Colour helpers
// ────────────────────────────────────────────────────────────────────────────

// Severity → RGB on a green→yellow→red gradient. Endpoints match the
// categorical colours used by DrawHeatmap (Theme::Green / Yellow / Red),
// so a low-severity gradient bar reads as "the same green" as a "low"
// heatmap cell.
inline void SeverityRGB(double severity, int& r, int& g, int& b) {
	if (severity < 0.0) severity = 0.0;
	if (severity > 1.0) severity = 1.0;
	if (severity < 0.5) {
		// Green (133,224,175) → Yellow (236,200,110)
		double t = severity * 2.0;
		r = static_cast<int>(Theme::GreenR + t * (Theme::YellowR - Theme::GreenR));
		g = static_cast<int>(Theme::GreenG + t * (Theme::YellowG - Theme::GreenG));
		b = static_cast<int>(Theme::GreenB + t * (Theme::YellowB - Theme::GreenB));
	}
	else {
		// Yellow (236,200,110) → Red (248,124,124)
		double t = (severity - 0.5) * 2.0;
		r = static_cast<int>(Theme::YellowR + t * (Theme::RedR - Theme::YellowR));
		g = static_cast<int>(Theme::YellowG + t * (Theme::RedG - Theme::YellowG));
		b = static_cast<int>(Theme::YellowB + t * (Theme::RedB - Theme::YellowB));
	}
}

inline void SetBarColor(double severity) {
	int r, g, b;
	SeverityRGB(severity, r, g, b);
	SetColorRGB(r, g, b);
}

namespace detail {

	inline bool HasAbsoluteSeverity(const GraphOptions& opts) {
		return opts.severityLowThreshold > 0 &&
			opts.severityHighThreshold > opts.severityLowThreshold;
	}

	inline double AbsoluteSeverity(int value, const GraphOptions& opts) {
		if (value <= 0) return 0.0;
		if (value < opts.severityLowThreshold) return 0.20;
		if (value < opts.severityHighThreshold) return 0.55;
		return 0.90;
	}

	inline void SetValueBarColor(int value, int maxVal, const GraphOptions& opts) {
		if (HasAbsoluteSeverity(opts)) {
			SetBarColor(AbsoluteSeverity(value, opts));
		}
		else {
			SetBarColor(maxVal > 0 ? static_cast<double>(value) / maxVal : 0.0);
		}
	}

	inline std::string FormatTime(unsigned seconds) {
		unsigned m = seconds / 60;
		unsigned s = seconds % 60;
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%u:%02u", m, s);
		return std::string(buf);
	}

	// Width-aware string truncation for labels.
	inline std::string Clip(const std::string& s, size_t maxLen) {
		if (s.size() <= maxLen) return s;
		if (maxLen <= 1) return s.substr(0, maxLen);
		return s.substr(0, maxLen - 1) + ".";
	}

	// Format a numeric tick value into a fixed-width string with optional unit.
	inline std::string FormatTick(int value, const std::string& unit, int width) {
		std::string s = std::to_string(value);
		if (!unit.empty()) s += unit;
		if (static_cast<int>(s.size()) >= width) return s;
		return std::string(width - s.size(), ' ') + s;
	}

	// A thin horizontal rule of the given visual width. Drawn in a muted
	// border tone so it reads as chrome, not as a heading.
	inline void DrawRule(int width) {
		SetColorRGB(Theme::WaveR, Theme::WaveG, Theme::WaveB);
		for (int i = 0; i < width; i++) std::cout << Sym::Horizontal;
		std::cout << "\n";
		Reset();
	}

	// Header block: title (bold-bright), optional subtitle (dim), thin rule.
	inline void DrawHeader(const std::string& title,
		const std::string& subtitle, int width) {
		std::cout << "\n";
		SetColorRGB(Theme::WhiteR, Theme::WhiteG, Theme::WhiteB);
		std::cout << "  " << title << "\n";
		if (!subtitle.empty()) {
			SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
			std::cout << "  " << subtitle << "\n";
		}
		Reset();
		std::cout << "  ";
		DrawRule(width);
	}

	// Legend showing the three severity bands used by the bar gradient.
	// The reference-line annotation is drawn inline on the plot itself, so
	// it does not appear here.
	inline void DrawSeverityLegend(int leftPad, const GraphOptions& opts) {
		std::cout << std::string(leftPad, ' ');

		auto swatch = [&](double sev, const std::string& label) {
			SetBarColor(sev);
			std::cout << Sym::Bar8 << Sym::Bar8;
			SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
			std::cout << " " << label << "   ";
			};

		swatch(0.20, opts.severityLowLabel);
		swatch(0.55, opts.severityModerateLabel);
		swatch(0.90, opts.severityHighLabel);

		Reset();
		std::cout << "\n";
	}

	// ── Accessible (screen-reader) text equivalents ──────────────────────────
	// When accessibility mode is on we replace the Unicode bar / heatmap /
	// score art - which a screen reader cannot interpret - with a concise
	// plain-text summary carrying the same numbers. No ANSI colour is emitted.

	inline void AccessibleHeader(const std::string& title, const std::string& subtitle) {
		std::cout << "\n";
		if (!title.empty())    std::cout << "  " << title << "\n";
		if (!subtitle.empty()) std::cout << "  " << subtitle << "\n";
	}

	inline const char* SeverityWord(double severity) {
		if (severity < 0.34) return "low";
		if (severity < 0.67) return "moderate";
		return "high";
	}

	inline void AccessibleBarGraph(const std::vector<int>& buckets, int maxVal,
		const GraphOptions& opts, DWORD totalSeconds) {
		(void)maxVal;
		AccessibleHeader(opts.title, opts.subtitle);

		const size_t n = buckets.size();
		int peak = 0; size_t peakIdx = 0; long long sum = 0; int nonZero = 0; int overRef = 0;
		for (size_t i = 0; i < n; i++) {
			const int v = buckets[i];
			sum += v;
			if (v > peak) { peak = v; peakIdx = i; }
			if (v > 0) nonZero++;
			if (opts.refLine > 0 && v >= opts.refLine) overRef++;
		}
		const std::string& unit = opts.unitSuffix;

		std::cout << "  Peak " << peak << unit;
		if (totalSeconds > 0 && n > 0) {
			const unsigned t = static_cast<unsigned>(peakIdx * totalSeconds / n);
			std::cout << " at " << FormatTime(t);
		}
		std::cout << ". ";
		if (n > 0) {
			const int avg = static_cast<int>(sum / static_cast<long long>(n));
			std::cout << "Average " << avg << unit << ". ";
		}
		std::cout << nonZero << " of " << n << " intervals had activity.";
		if (opts.refLine > 0) {
			const std::string refName = opts.refLabel.empty() ? std::string("reference") : opts.refLabel;
			std::cout << " " << overRef << " at or above the " << opts.refLine << unit
			          << " " << refName << ".";
		}
		std::cout << "\n\n";
	}

	inline void AccessibleHeatmap(const std::vector<HeatmapRow>& rows,
		const std::string& title, const std::string& subtitle, DWORD totalSeconds) {
		AccessibleHeader(title, subtitle);
		for (const auto& row : rows) {
			int none = 0, low = 0, mod = 0, high = 0, peak = 0;
			for (int v : row.values) {
				if (v > peak) peak = v;
				if (v <= 0) none++;
				else if (v < row.lowThresh) low++;
				else if (v < row.highThresh) mod++;
				else high++;
			}
			(void)none;
			std::cout << "  " << row.label << ": peak " << peak
			          << "; low " << low << ", moderate " << mod << ", high " << high
			          << " (of " << row.values.size() << " intervals).\n";
		}
		if (totalSeconds > 0) {
			std::cout << "  Span 0:00 to " << FormatTime(static_cast<unsigned>(totalSeconds)) << ".\n";
		}
		std::cout << "\n";
	}

	inline void AccessibleScoreBar(const std::string& label,
		double fillFraction, double severity, const std::string& suffix) {
		if (fillFraction < 0.0) fillFraction = 0.0;
		if (fillFraction > 1.0) fillFraction = 1.0;
		const int pct = static_cast<int>(fillFraction * 100.0 + 0.5);
		std::cout << "  " << label << " " << pct << "% (" << SeverityWord(severity) << ")";
		if (!suffix.empty()) std::cout << " " << suffix;
		std::cout << "\n";
	}

} // namespace detail

// ────────────────────────────────────────────────────────────────────────────
// Single-series bar graph
// ────────────────────────────────────────────────────────────────────────────

inline void DrawBarGraph(const std::vector<int>& buckets, int maxVal,
	const GraphOptions& opts, DWORD totalSeconds = 0) {
	if (buckets.empty() || maxVal <= 0) return;

	if (Accessibility::IsEnabled()) {
		detail::AccessibleBarGraph(buckets, maxVal, opts, totalSeconds);
		return;
	}

	const int width = static_cast<int>(buckets.size());
	const int height = std::max(4, opts.height);

	// Y-axis label width: widest of "maxVal", "refLine", "0".
	auto numLen = [&](int v) {
		std::string s = std::to_string(v);
		if (!opts.unitSuffix.empty()) s += opts.unitSuffix;
		return static_cast<int>(s.size());
		};
	int labelW = std::max(numLen(maxVal), numLen(0));
	if (opts.refLine > 0 && opts.refLine < maxVal) labelW = std::max(labelW, numLen(opts.refLine));
	if (opts.refLine == 0) labelW = std::max(labelW, numLen(maxVal / 2));
	labelW = std::max(labelW, 3);

	// Visual column where the first bar prints, measured from column 0.
	// Every bar row is "  " (2) + label (labelW) + " " (1) + tick (1) = labelW + 4.
	const int barCol = 2 + labelW + 1 + 1;
	const int frameW = barCol + width;

	detail::DrawHeader(opts.title, opts.subtitle, frameW);

	// Row that the reference line falls on (1-based, top = height).
	// refRow == height would collide with the maxVal label/tick, so we
	// require refRow strictly less than height for a visible reference.
	int refRow = -1;
	int refCol = -1;
	if (opts.refLine > 0 && opts.refLine < maxVal) {
		refRow = (opts.refLine * height + maxVal / 2) / maxVal;
		if (refRow < 1) refRow = 1;
		if (refRow >= height) refRow = -1;
	}
	// First column where the bar value exceeds the reference line — anchor
	// for the ╷│┴ connector. Only meaningful if refRow itself is meaningful.
	if (opts.refLine > 0 && refRow > 0) {
		for (int c = 0; c < width; c++) {
			if (buckets[c] >= opts.refLine) { refCol = c; break; }
		}
	}

	// Pick which rows get y-tick labels: top, reference, midpoint, zero.
	auto rowLabel = [&](int row) -> std::string {
		if (row == height) return detail::FormatTick(maxVal, opts.unitSuffix, labelW);
		if (row == refRow && opts.refLine > 0)
			return detail::FormatTick(opts.refLine, opts.unitSuffix, labelW);
		// Midpoint only if it doesn't collide with the reference row label.
		int mid = (height + 1) / 2;
		if (row == mid && row != refRow) {
			int midVal = (maxVal + 1) / 2;
			return detail::FormatTick(midVal, opts.unitSuffix, labelW);
		}
		if (row == 1) return detail::FormatTick(0, opts.unitSuffix, labelW);
		return std::string(labelW, ' ');
		};

	for (int row = height; row >= 1; row--) {
		// Y-tick label
		SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
		std::string lbl = rowLabel(row);
		std::cout << "  " << lbl << " ";

		// Tick marker: ┤ on labeled rows, faint vertical on others.
		bool hasLabel = (row == height) || (row == 1) ||
			(row == refRow && opts.refLine > 0) ||
			(row == (height + 1) / 2 && row != refRow);
		if (hasLabel) {
			SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
			std::cout << "\xe2\x94\xa4"; // ┤
		}
		else {
			SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
			std::cout << " ";
		}

		// Sub-row precision: 8 fill levels per row.
		double rowBase = static_cast<double>(row - 1) / height * maxVal;
		double rowTop = static_cast<double>(row) / height * maxVal;
		double rowSpan = rowTop - rowBase;

		for (int col = 0; col < width; col++) {
			double val = static_cast<double>(buckets[col]);

			if (val >= rowTop) {
				if (opts.colorize) detail::SetValueBarColor(static_cast<int>(val), maxVal, opts);
				else SetColorRGB(Theme::CyanR, Theme::CyanG, Theme::CyanB);
				std::cout << Sym::Bar8;
			}
			else if (val > rowBase) {
				double fill = (val - rowBase) / rowSpan;
				int level = static_cast<int>(fill * 8.0);
				if (level < 1) level = 1;
				if (level > 8) level = 8;
				if (opts.colorize) detail::SetValueBarColor(static_cast<int>(val), maxVal, opts);
				else SetColorRGB(Theme::CyanR, Theme::CyanG, Theme::CyanB);
				std::cout << Sym::BarLevels[level];
			}
			else if (row == refRow && opts.refLine > 0) {
				// Reference line through empty cells; ╷ at the anchor col,
				// ─ everywhere else. The ╷ doubles as the top of a vertical
				// drop-line that hangs through subsequent rows under refCol.
				SetColorRGB(Theme::YellowR, Theme::YellowG, Theme::YellowB);
				if (refCol >= 0 && col == refCol) std::cout << "\xe2\x95\xb7"; // ╷
				else std::cout << "\xe2\x94\x80";                              // ─
			}
			else if (opts.refLine > 0 && refCol >= 0 && col == refCol
				&& row < refRow) {
				// Vertical drop-line from the reference line to the x-axis
				// at the breach column, drawn in empty cells only so bars
				// remain on top.
				SetColorRGB(Theme::YellowR, Theme::YellowG, Theme::YellowB);
				std::cout << "\xe2\x94\x82"; // │
			}
			else {
				std::cout << " ";
			}
		}

		// Inline reference label on the ref row. Shown even when no bar
		// breaches, so the yellow horizontal line is never unexplained.
		if (row == refRow && opts.refLine > 0 && !opts.refLabel.empty()) {
			SetColorRGB(Theme::YellowR, Theme::YellowG, Theme::YellowB);
			std::cout << " " << opts.refLabel;
		}

		Reset();
		std::cout << "\n";
	}

	// X-axis with arrow terminus; ┴ marks the reference-breach column.
	SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
	std::cout << "  " << std::string(labelW, ' ') << " " << Sym::BottomLeft;
	for (int i = 0; i < width; i++) {
		if (i == refCol && opts.refLine > 0) {
			SetColorRGB(Theme::YellowR, Theme::YellowG, Theme::YellowB);
			std::cout << "\xe2\x94\xb4"; // ┴
			SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
		}
		else {
			std::cout << Sym::Horizontal;
		}
	}
	std::cout << "\xe2\x86\x92"; // →
	std::cout << "\n";

	// X-axis time labels: start, end, plus reference time if known.
	if (totalSeconds > 0) {
		std::string endStr = detail::FormatTime(totalSeconds);
		std::string startStr = "0:00";
		// Row is a string of total visual width = barCol + width. We place
		// the start time at index barCol and right-align the end time.
		std::string row(static_cast<size_t>(barCol + width), ' ');
		for (size_t i = 0; i < startStr.size() && barCol + i < row.size(); i++)
			row[barCol + i] = startStr[i];
		int endLeft = barCol + width - static_cast<int>(endStr.size());
		if (endLeft < barCol + static_cast<int>(startStr.size()) + 1)
			endLeft = barCol + static_cast<int>(startStr.size()) + 1;
		for (size_t i = 0; i < endStr.size() && static_cast<size_t>(endLeft) + i < row.size(); i++)
			row[static_cast<size_t>(endLeft) + i] = endStr[i];

		SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
		std::cout << row << "\n";
		Reset();
	}

	// Legend (indented under the bars).
	detail::DrawSeverityLegend(barCol, opts);

	std::cout << "\n";
	Reset();
}

// ────────────────────────────────────────────────────────────────────────────
// Severity heatmap (used for the QCheck combined C1/C2/CU view)
//
// One row per metric.  Each cell is bucketed into none/low/moderate/high by
// the row's absolute thresholds and rendered as a single coloured block.
// Reads at a glance — easy to spot a moment in time where multiple metrics
// degrade simultaneously, without the misleading peak-normalisation of an
// overlay chart.
// ────────────────────────────────────────────────────────────────────────────

inline void DrawHeatmap(const std::vector<HeatmapRow>& rows,
	const std::string& title, const std::string& subtitle,
	DWORD totalSeconds = 0) {
	if (rows.empty()) return;

	if (Accessibility::IsEnabled()) {
		detail::AccessibleHeatmap(rows, title, subtitle, totalSeconds);
		return;
	}

	int width = 0;
	int labelW = 0;
	for (const auto& r : rows) {
		width = std::max(width, static_cast<int>(r.values.size()));
		labelW = std::max(labelW, static_cast<int>(r.label.size()));
	}
	if (width == 0) return;

	// "  " (2) + label (labelW) + " " (1) = barCol.
	const int barCol = 2 + labelW + 1;
	const int frameW = barCol + width;

	detail::DrawHeader(title, subtitle, frameW);

	for (const auto& row : rows) {
		// Label, right-aligned to labelW.
		SetColorRGB(Theme::WhiteR, Theme::WhiteG, Theme::WhiteB);
		std::string lbl = row.label;
		if (static_cast<int>(lbl.size()) < labelW)
			lbl = std::string(labelW - lbl.size(), ' ') + lbl;
		std::cout << "  " << lbl << " ";

		for (int col = 0; col < width; col++) {
			int v = (col < static_cast<int>(row.values.size())) ? row.values[col] : 0;
			if (v <= 0) {
				// "none" — dim shade, no colour emphasis.
				SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
				std::cout << "\xe2\x96\x91"; // ░
			}
			else if (v < row.lowThresh) {
				SetColorRGB(Theme::GreenR, Theme::GreenG, Theme::GreenB);
				std::cout << Sym::Bar8;
			}
			else if (v < row.highThresh) {
				SetColorRGB(Theme::YellowR, Theme::YellowG, Theme::YellowB);
				std::cout << Sym::Bar8;
			}
			else {
				SetColorRGB(Theme::RedR, Theme::RedG, Theme::RedB);
				std::cout << Sym::Bar8;
			}
		}
		Reset();
		std::cout << "\n";
	}

	// Time labels (no x-axis line — the coloured row IS the x-axis).
	if (totalSeconds > 0) {
		std::string endStr = detail::FormatTime(totalSeconds);
		std::string startStr = "0:00";
		std::string trow(static_cast<size_t>(barCol + width), ' ');
		for (size_t i = 0; i < startStr.size() && barCol + i < trow.size(); i++)
			trow[barCol + i] = startStr[i];
		int endLeft = barCol + width - static_cast<int>(endStr.size());
		if (endLeft < barCol + static_cast<int>(startStr.size()) + 1)
			endLeft = barCol + static_cast<int>(startStr.size()) + 1;
		for (size_t i = 0; i < endStr.size() && static_cast<size_t>(endLeft) + i < trow.size(); i++)
			trow[static_cast<size_t>(endLeft) + i] = endStr[i];
		SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
		std::cout << trow << "\n";
	}

	// Legend: none / low / moderate / high.
	std::cout << std::string(barCol, ' ');
	SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
	std::cout << "\xe2\x96\x91 none   ";
	SetColorRGB(Theme::GreenR, Theme::GreenG, Theme::GreenB);
	std::cout << Sym::Bar8;
	SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
	std::cout << " low   ";
	SetColorRGB(Theme::YellowR, Theme::YellowG, Theme::YellowB);
	std::cout << Sym::Bar8;
	SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
	std::cout << " moderate   ";
	SetColorRGB(Theme::RedR, Theme::RedG, Theme::RedB);
	std::cout << Sym::Bar8;
	SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
	std::cout << " high";
	Reset();
	std::cout << "\n\n";
}

// ────────────────────────────────────────────────────────────────────────────
// Horizontal severity-coloured score / progress bar
//
// Used for compact "score" or "headroom" indicators in textual reports.
// `severity` (0.0 = green, 1.0 = red) drives the bar colour; `fillFraction`
// (0.0 - 1.0) drives the fill ratio.  Set them independently — e.g. zone
// error rates use rate-based severity but rate-based fill, while a headroom
// score uses score-based fill but inverse severity.
// ────────────────────────────────────────────────────────────────────────────

inline void DrawScoreBar(const std::string& label,
	double fillFraction, double severity, int barWidth = 32,
	const std::string& suffix = "") {
	if (Accessibility::IsEnabled()) {
		detail::AccessibleScoreBar(label, fillFraction, severity, suffix);
		return;
	}

	if (fillFraction < 0.0) fillFraction = 0.0;
	if (fillFraction > 1.0) fillFraction = 1.0;

	int filled = static_cast<int>(fillFraction * barWidth + 0.5);
	if (fillFraction > 0.0 && filled == 0) filled = 1;

	SetColorRGB(Theme::WhiteR, Theme::WhiteG, Theme::WhiteB);
	std::cout << "  " << label;
	SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
	std::cout << " \xe2\x94\x82"; // │

	// Filled portion with severity gradient.
	for (int i = 0; i < filled; i++) {
		double t = (filled > 1) ? static_cast<double>(i) / (filled - 1) : 0.0;
		double cellSev = severity * (0.5 + 0.5 * t);
		SetBarColor(std::min(1.0, cellSev));
		std::cout << Sym::Bar8;
	}
	// Empty portion as light shade.
	SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
	for (int i = filled; i < barWidth; i++) std::cout << "\xe2\x96\x91"; // ░

	std::cout << "\xe2\x94\x82"; // │

	if (!suffix.empty()) {
		SetColorRGB(Theme::DimR, Theme::DimG, Theme::DimB);
		std::cout << " " << suffix;
	}
	Reset();
	std::cout << "\n";
}

// ────────────────────────────────────────────────────────────────────────────
// Data bucketing helper
// ────────────────────────────────────────────────────────────────────────────

inline std::vector<int> BucketData(const std::vector<int>& perSecond, int width) {
	std::vector<int> buckets(width, 0);
	if (perSecond.empty() || width <= 0) return buckets;
	for (size_t i = 0; i < perSecond.size(); i++) {
		size_t b = (i * width) / perSecond.size();
		if (b >= static_cast<size_t>(width)) b = width - 1;
		buckets[b] = std::max(buckets[b], perSecond[i]);
	}
	return buckets;
}

}  // namespace Console
