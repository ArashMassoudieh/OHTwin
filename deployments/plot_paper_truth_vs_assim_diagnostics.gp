# =============================================================================
# plot_paper_truth_vs_assim_diagnostics.gp
#
# Two-window comparison of truth and assimilation model outputs for two
# diagnostic (non-calibrated) channels of the Bioretention twin
# experiment: Evaporation and Groundwater Recharge. These channels are
# not used by the GA misfit; their agreement (or disagreement) with truth
# indicates whether the calibrated model is reproducing the full water
# balance or only the three channels it was tuned to.
#
# Overflow is omitted because it is essentially zero throughout. Inflow
# and Precipitation are forcings (identical between truth and assimilation
# by construction) and are not informative for this comparison.
#
# Column layout in the OHQ selected-output CSVs (paired t,v):
#   1,2:   Evaporation
#   3,4:   Groundwater Recharge (m3/day)
#
# Inputs:
#   Bioretention_truth/outputs/selected_output.csv
#   Bioretention_assimilation/outputs/selected_output.csv
#
# Output:
#   paper_truth_vs_assim_diagnostics.png
# =============================================================================

set datafile separator ","
set datafile missing "nan"

truth_file = "Bioretention_truth/outputs/selected_output.csv"
assim_file = "Bioretention_assimilation/outputs/selected_output.csv"
outfile    = "paper_truth_vs_assim_diagnostics.png"

if (!exists("drift")) drift = 0
if (drift) {
    truth_file = "Bioretention_truth_drift/outputs/selected_output.csv"
    assim_file = "Bioretention_assimilation_drift/outputs/selected_output.csv"
    outfile    = "paper_truth_vs_assim_diagnostics_drift.png"
}

# --- output canvas ---------------------------------------------------------
set terminal pngcairo size 1400,950 enhanced font "Helvetica,18"
set output outfile

# --- time conversions -----------------------------------------------------
epoch(d) = (d - 25569.0) * 86400.0
set xdata time
set timefmt "%s"

# --- window bounds --------------------------------------------------------
early_start = 43865.0   # 2020-02-04
early_end   = 43875.0   # 2020-02-14
late_start  = 44047.0   # 2020-08-04
late_end    = 44057.0   # 2020-08-14

# --- truth sub-sampling ---------------------------------------------------
sub_every = 1

# --- style ----------------------------------------------------------------
c_truth = "#000000"
c_assim = "#1f77b4"
ps_truth = 0.5
lw_assim = 1.8

date_format = "%b %d"

# --- layout: 2 rows x 2 columns -------------------------------------------
set multiplot layout 2,2

set grid xtics ytics lc rgb "#cccccc" lw 0.8
set tics nomirror
set border 3

# ----- Row 1, left: Evaporation (early window) -- legend lives here ------
set xrange [epoch(early_start):epoch(early_end)]
set format x ""
unset xlabel
set ylabel "Evaporation (m^3/day)" offset 1,0
set yrange [0:2.2]
set ytics 0.5
set format y "%g"
set title "Early window: 2020-02-04 -- 2020-02-14" font "Helvetica,18"

set key top right inside box opaque samplen 2 spacing 1.2 \
    font "Helvetica,16" width 0

plot \
    assim_file using (epoch($1)):2 with lines \
        lc rgb c_assim lw lw_assim title "Assimilation model", \
    truth_file every sub_every using (epoch($1)):2 with points \
        lc rgb c_truth pt 7 ps ps_truth title "Truth (synthetic obs.)"

unset key

# ----- Row 1, right: Evaporation (late window) ----------------------------
set xrange [epoch(late_start):epoch(late_end)]
set format x ""
unset ylabel
set yrange [0:2.2]
set format y ""
set title "Late window: 2020-08-04 -- 2020-08-14" font "Helvetica,18"

plot \
    assim_file using (epoch($1)):2 with lines \
        lc rgb c_assim lw lw_assim notitle, \
    truth_file every sub_every using (epoch($1)):2 with points \
        lc rgb c_truth pt 7 ps ps_truth notitle

# ----- Row 2, left: Groundwater Recharge (early window) -- bottom row -----
set xrange [epoch(early_start):epoch(early_end)]
set xtics 172800   # 2 days
set format x date_format
set xlabel "Date (2020)" offset 0,-0.5
set ylabel "Groundwater Recharge (m^3/day)" offset 1,0
set yrange [0:0.65]
set ytics 0.1
set format y "%g"
unset title

plot \
    assim_file using (epoch($3)):4 with lines \
        lc rgb c_assim lw lw_assim notitle, \
    truth_file every sub_every using (epoch($3)):4 with points \
        lc rgb c_truth pt 7 ps ps_truth notitle

# ----- Row 2, right: Groundwater Recharge (late window) -------------------
set xrange [epoch(late_start):epoch(late_end)]
set xtics 172800
set format x date_format
set xlabel "Date (2020)" offset 0,-0.5
unset ylabel
set yrange [0:0.65]
set format y ""
unset title

plot \
    assim_file using (epoch($3)):4 with lines \
        lc rgb c_assim lw lw_assim notitle, \
    truth_file every sub_every using (epoch($3)):4 with points \
        lc rgb c_truth pt 7 ps ps_truth notitle

unset multiplot
unset output
